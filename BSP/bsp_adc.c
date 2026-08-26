#include "bsp_adc.h"
#include "adc.h"
#include <math.h>

#define NTC_CHANNELS 4
static uint16_t adc_buffer[NTC_CHANNELS];

/* Simplified NTC parameters for a standard 10k NTC (B=3950) with 10k pullup to 3.3V */
#define R_REF 10000.0f
#define V_REF 3.3f
#define NTC_B 3950.0f
#define NTC_R25 10000.0f
#define KELVIN_25C 298.15f

void BSP_ADC_Init(void)
{
    /* Start ADC with DMA */
    HAL_ADC_Start_DMA(&hadc1, (uint32_t*)adc_buffer, NTC_CHANNELS);
}

float BSP_ADC_GetTempC(uint8_t channel_index)
{
    if (channel_index >= NTC_CHANNELS) return NAN;
    
    uint16_t adc_val = adc_buffer[channel_index];
    if (adc_val == 0 || adc_val >= 4095) {
        return NAN; // Invalid reading
    }
    
    /* Calculate resistance of NTC */
    /* V_NTC = (adc_val / 4095.0) * V_REF */
    /* R_NTC = R_REF * V_NTC / (V_REF - V_NTC) */
    float r_ntc = R_REF * ((float)adc_val / (4095.0f - (float)adc_val));
    
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
