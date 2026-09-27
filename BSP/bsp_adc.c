#include "bsp_adc.h"
#include "adc.h"
#include <math.h>
#include <stdbool.h>

#define NTC_CHANNELS 4

/* The 4 NTC channels feed the DWIN "JACK" temperature field and the charge
 * controller's jack over-temp soft-derating / fault (App/Charge). The ADC is
 * configured in Core/Src/adc.c as a *single-shot* 4-channel scan
 * (ContinuousConvMode = DISABLE) with a *one-shot* NORMAL-mode DMA
 * (hdma_adc1.Init.Mode = DMA_NORMAL): one HAL_ADC_Start_DMA() call yields
 * exactly one sample set, then both the ADC sequencer and the DMA disarm
 * themselves. Nothing re-triggers them.
 *
 * BSP_ADC_Process() therefore has to re-arm the scan on a timer -- without it
 * the readings freeze at their power-on values (which silently disabled the
 * jack over-temp protection: the derating/fault path only ever saw the boot
 * sample). This is deliberately a main-loop poll, not a free-running
 * peripheral + ISR: easier to reason about on a debugger, and the NTC thermal
 * time-constant is seconds so a coarse resample rate loses nothing.
 *
 * That single-shot ADC + one-shot DMA pairing is what this re-arm assumes; it
 * is pinned by check_ioc.py so a CubeMX "Generate Code" cannot silently switch
 * it to free-running and change the sampling contract out from under here. */
#define BSP_ADC_RESAMPLE_INTERVAL_MS  200U

static uint16_t adc_buffer[NTC_CHANNELS];
static uint32_t adc_last_sample_tick;
static bool     adc_started;

/* Filtered ADC values (EMA: Exponential Moving Average) */
#define ADC_FILTER_ALPHA              0.20f  /* ~1s response time at 5Hz (200ms sample interval) */
#define ADC_RAW_MIN_VALID             20U    /* Below 20 counts (~0.016V) considered shorted to GND */
#define ADC_RAW_MAX_VALID             4075U  /* Above 4075 counts (~3.28V) considered open-circuit */

static float s_adc_filtered[NTC_CHANNELS];
static bool  s_adc_filtered_valid[NTC_CHANNELS];

/* Simplified NTC parameters for a standard 10k NTC (B=3950) with 10k pullup to 3.3V */
#define R_REF 10000.0f
#define V_REF 3.3f
#define NTC_B 3950.0f
#define NTC_R25 10000.0f
#define KELVIN_25C 298.15f

void BSP_ADC_Init(void)
{
    adc_last_sample_tick = 0U;
    for (uint8_t i = 0; i < NTC_CHANNELS; i++) {
        s_adc_filtered[i] = 0.0f;
        s_adc_filtered_valid[i] = false;
    }

    /* 1. Self-calibration: compensate internal analog offset before enabling conversion */
    (void)HAL_ADCEx_Calibration_Start(&hadc1);

    /* 2. Kick the first scan; BSP_ADC_Process() keeps it going. If this fails we
     * retry every Process() call until it takes (adc_started stays false). */
    adc_started = (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_buffer, NTC_CHANNELS) == HAL_OK);
}

void BSP_ADC_Process(uint32_t now_tick)
{
    /* Plain unsigned subtraction: adc_last_sample_tick is written only here,
     * from the main loop -- wraparound-safe (AGENTS.md sec 9). */
    if (adc_started && (now_tick - adc_last_sample_tick) < BSP_ADC_RESAMPLE_INTERVAL_MS) {
        return;
    }
    adc_last_sample_tick = now_tick;

    /* The previous NORMAL-mode transfer has already completed and disarmed by
     * now (a 4-channel scan finishes in ~20 microseconds, the interval is 200 ms).
     * Update the digital filter with the newly acquired raw samples. */
    for (uint8_t i = 0; i < NTC_CHANNELS; i++) {
        uint16_t raw = adc_buffer[i];
        if (raw >= ADC_RAW_MIN_VALID && raw <= ADC_RAW_MAX_VALID) {
            if (!s_adc_filtered_valid[i]) {
                /* Seed filter immediately on first valid sample */
                s_adc_filtered[i] = (float)raw;
                s_adc_filtered_valid[i] = true;
            } else {
                /* Smooth using Exponential Moving Average */
                s_adc_filtered[i] += ADC_FILTER_ALPHA * ((float)raw - s_adc_filtered[i]);
            }
        } else {
            /* Open / short circuit / disconnected NTC */
            s_adc_filtered_valid[i] = false;
            s_adc_filtered[i] = (float)raw;
        }
    }

    /* Stop_DMA returns the HAL/ADC state machine to a known-idle point so
     * the restart is unconditional and deterministic. */
    (void)HAL_ADC_Stop_DMA(&hadc1);
    adc_started = (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_buffer, NTC_CHANNELS) == HAL_OK);
}

float BSP_ADC_GetTempC(uint8_t channel_index)
{
    if (channel_index >= NTC_CHANNELS) return NAN;

    float adc_val;
    if (s_adc_filtered_valid[channel_index]) {
        adc_val = s_adc_filtered[channel_index];
    } else {
        uint16_t raw = adc_buffer[channel_index];
        if (raw < ADC_RAW_MIN_VALID || raw > ADC_RAW_MAX_VALID) {
            return NAN; /* Open or shorted NTC, or no sample yet */
        }
        adc_val = (float)raw;
    }

    if (adc_val <= 0.0f || adc_val >= 4095.0f) {
        return NAN;
    }

    /* Calculate resistance of NTC */
    /* V_NTC = (adc_val / 4095.0) * V_REF */
    /* R_NTC = R_REF * V_NTC / (V_REF - V_NTC) */
    float r_ntc = R_REF * (adc_val / (4095.0f - adc_val));

    /* Calculate Temperature using Steinhart-Hart equation (B-parameter) */
    /* 1/T = 1/T0 + (1/B) * ln(R/R0) */
    float steinhart;
    steinhart = r_ntc / NTC_R25;
    steinhart = logf(steinhart);
    steinhart /= NTC_B;
    steinhart += 1.0f / KELVIN_25C;
    steinhart = 1.0f / steinhart;
    steinhart -= 273.15f; // Convert to Celsius

    return steinhart;
}
