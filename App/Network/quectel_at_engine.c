/**
 * @file    quectel_at_engine.c
 * @brief   Application Network Layer - Quectel Cellular AT Engine & FSM
 * @note    Non-blocking state machine for SIM card, 4G network registration,
 *          and PDP context management.
 */

#include "quectel_at_engine.h"
#include "bsp_quectel.h"
#include "bsp_rtc.h"
#include "debug_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define AT_CMD_TIMEOUT_DEFAULT_MS       1500U
#define AT_CMD_TIMEOUT_ACT_MS           15000U  /* PDP activation can take several seconds */
#define CSQ_POLL_INTERVAL_MS            30000U  /* Poll signal every 30s when ready */
#define RETRY_BACKOFF_MS                5000U   /* Delay before retrying failed step */

#define TIME_SYNC_INITIAL_DELAY_MS      3000U       /* Wait 3s after entering READY before first QLTS */
#define TIME_SYNC_INTERVAL_MS           3600000U    /* 1 hour periodic synchronization */
#define TIME_SYNC_RETRY_MS              30000U      /* 30s retry interval when NITZ is not yet available */
#define TIME_SYNC_MAX_RETRIES           5U          /* Maximum consecutive empty QLTS retries */

typedef enum {
    READY_CMD_NONE = 0,
    READY_CMD_CSQ,
    READY_CMD_QLTS
} ReadyCmd_t;

static QuectelNetStatus_t s_net_status;
static char s_apn[32] = "v-internet";

static uint32_t s_state_tick = 0U;
static uint32_t s_retry_count = 0U;
static bool s_cmd_in_flight = false;
static uint32_t s_cmd_timeout_ms = AT_CMD_TIMEOUT_DEFAULT_MS;
static bool s_ota_exclusive = false;

static ReadyCmd_t s_ready_cmd = READY_CMD_NONE;
static uint32_t s_last_time_sync_tick = 0U;
static bool s_time_sync_done_once = false;
static uint8_t s_time_sync_retry_count = 0U;

static void transition_to(QuectelNetState_t new_state, uint32_t now_tick)
{
    s_net_status.state = new_state;
    s_state_tick = now_tick;
    s_retry_count = 0U;
    s_cmd_in_flight = false;
    s_ready_cmd = READY_CMD_NONE;
}

void QuectelEngine_Init(void)
{
    memset(&s_net_status, 0, sizeof(s_net_status));
    s_net_status.model[0] = '\0';
    s_ota_exclusive = false;
    s_net_status.state = QUECTEL_NET_STATE_OFF;
    s_net_status.csq_rssi = 99U; /* Unknown */

    /* Power on hardware modem */
    BSP_Quectel_PowerOn();
    s_net_status.state = QUECTEL_NET_STATE_POWERING_ON;
    s_state_tick = 0U;
    LOG("Quectel_Engine: Initialized, waiting hardware power on...\r\n");
}

void QuectelEngine_SetApn(const char *apn)
{
    if (apn != NULL && strlen(apn) > 0U && strlen(apn) < sizeof(s_apn)) {
        strncpy(s_apn, apn, sizeof(s_apn) - 1U);
        s_apn[sizeof(s_apn) - 1U] = '\0';
    }
}

bool QuectelEngine_IsNetReady(void)
{
    return (s_net_status.state == QUECTEL_NET_STATE_READY && s_net_status.pdp_active);
}

void QuectelEngine_SetOtaExclusive(bool exclusive)
{
    s_ota_exclusive = exclusive;
    if (exclusive) s_cmd_in_flight = false;
}

bool QuectelEngine_IsRtcSynced(void)
{
    return s_net_status.rtc_synced;
}

void QuectelEngine_GetStatus(QuectelNetStatus_t *out_status)
{
    if (out_status != NULL) {
        memcpy(out_status, &s_net_status, sizeof(QuectelNetStatus_t));
    }
}

void QuectelEngine_Process(uint32_t now_tick)
{
    char line[256];

    s_net_status.powered = BSP_Quectel_IsReady();

    /* OTA owns the same UART RX ring while QHTTP is in CONNECT/binary mode.
     * Parsing here would consume OTA's CONNECT/OK lines or binary bytes. */
    if (s_ota_exclusive) return;

    /* Process incoming lines from modem */
    while (BSP_Quectel_ReadLine(line, sizeof(line))) {
        /* Ignore empty lines or echoes */
        if (strlen(line) == 0U) {
            continue;
        }

        /* Check for unsolicited or info messages */
        if (s_net_status.state == QUECTEL_NET_STATE_CHECK_INFO) {
            if (strcmp(line, "OK") != 0 &&
                strncmp(line, "ERROR", 5) != 0 &&
                strncmp(line, "+CME ERROR", 10) != 0 &&
                strncmp(line, "AT", 2) != 0 &&
                strncmp(line, "Quectel", 7) != 0 &&
                strncmp(line, "Revision", 8) != 0) {
                const char *p = line;
                if (strncmp(p, "+CGMM:", 6) == 0) {
                    p += 6;
                }
                while (*p == ' ' || *p == '\t' || *p == '"') {
                    p++;
                }
                size_t len = strlen(p);
                while (len > 0U && (p[len - 1] == ' ' || p[len - 1] == '\t' ||
                                   p[len - 1] == '"' || p[len - 1] == '\r' ||
                                   p[len - 1] == '\n')) {
                    len--;
                }
                if (len > 0U && len < sizeof(s_net_status.model)) {
                    memcpy(s_net_status.model, p, len);
                    s_net_status.model[len] = '\0';
                    LOG("Quectel_Engine: Detected Hardware Model: %s\r\n", s_net_status.model);
                }
            }
        }

        if (strncmp(line, "+CPIN: READY", 12) == 0) {
            s_net_status.sim_ready = true;
            LOG("Quectel_Engine: SIM Card READY\r\n");
        } else if (strncmp(line, "+CSQ:", 5) == 0) {
            int rssi = 99, ber = 99;
            if (sscanf(line + 5, "%d,%d", &rssi, &ber) >= 1) {
                s_net_status.csq_rssi = (uint8_t)rssi;
                s_net_status.last_csq_update_ms = now_tick;
                LOG("Quectel_Engine: Signal RSSI=%d (0..31, 99=unk)\r\n", rssi);
            }
        } else if (strncmp(line, "+CEREG:", 7) == 0 || strncmp(line, "+CREG:", 6) == 0) {
            int n = 0, stat = 0;
            const char *p = strchr(line, ':');
            if (p != NULL && sscanf(p + 1, "%d,%d", &n, &stat) >= 1) {
                if (stat == 1 || stat == 5) {
                    s_net_status.net_registered = true;
                    LOG("Quectel_Engine: Network registered (stat=%d)\r\n", stat);
                } else {
                    s_net_status.net_registered = false;
                }
            }
        } else if (strncmp(line, "+QIACT:", 7) == 0) {
            /* Example: +QIACT: 1,1,1,"10.154.22.8" */
            char *ip_start = strchr(line, '"');
            if (ip_start != NULL) {
                char *ip_end = strchr(ip_start + 1, '"');
                if (ip_end != NULL) {
                    size_t ip_len = (size_t)(ip_end - (ip_start + 1));
                    if (ip_len < sizeof(s_net_status.ip_addr)) {
                        strncpy(s_net_status.ip_addr, ip_start + 1, ip_len);
                        s_net_status.ip_addr[ip_len] = '\0';
                        s_net_status.pdp_active = true;
                        LOG("Quectel_Engine: PDP IP Assigned: %s\r\n", s_net_status.ip_addr);
                    }
                }
            }
        } else if (strncmp(line, "+QLTS:", 6) == 0) {
            /* Example: +QLTS: "2026/09/21,00:30:15+28,0" or empty: +QLTS: "" */
            const char *p = strchr(line, '"');
            if (p != NULL && *(p + 1) == '"') {
                LOG("Quectel_Engine: Network time not available yet (+QLTS is empty)\r\n");
                s_time_sync_retry_count++;
                if (s_time_sync_retry_count >= TIME_SYNC_MAX_RETRIES) {
                    LOG("Quectel_Engine: Max QLTS retries reached, delaying next time sync to 1h\r\n");
                    s_last_time_sync_tick = now_tick;
                    s_time_sync_done_once = true;
                }
            } else if (p != NULL) {
                int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
                p++; /* Skip opening quote */
                if (sscanf(p, "%d/%d/%d,%d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {
                    if (year >= 2024 && year <= 2099 &&
                        month >= 1 && month <= 12 &&
                        day >= 1 && day <= 31 &&
                        hour >= 0 && hour <= 23 &&
                        minute >= 0 && minute <= 59 &&
                        second >= 0 && second <= 59) {

                        BSP_RTC_DateTime_t dt_utc;
                        memset(&dt_utc, 0, sizeof(dt_utc));
                        dt_utc.year = (uint16_t)year;
                        dt_utc.month = (uint8_t)month;
                        dt_utc.day = (uint8_t)day;
                        dt_utc.hour = (uint8_t)hour;
                        dt_utc.minute = (uint8_t)minute;
                        dt_utc.second = (uint8_t)second;
                        dt_utc.weekday = 1U;

                        uint32_t utc_epoch = BSP_RTC_DateTimeToEpoch(&dt_utc);
                        bool accept_time = true;

                        if (BSP_RTC_IsTimeValid()) {
                            uint32_t cur_epoch = BSP_RTC_GetEpoch();
                            int32_t diff = (int32_t)(utc_epoch - cur_epoch);
                            /* Allow negative delta up to -300s (fast clock drift correction) but reject large backward jump */
                            if (diff < -300) {
                                LOG("Quectel_Engine: Rejecting backward time jump (cur=%lu, new=%lu, diff=%ld)\r\n",
                                    (unsigned long)cur_epoch, (unsigned long)utc_epoch, (long)diff);
                                accept_time = false;
                            }
                        }

                        if (accept_time && utc_epoch > 0U) {
                            if (BSP_RTC_SetEpoch(utc_epoch)) {
                                s_last_time_sync_tick = now_tick;
                                s_time_sync_done_once = true;
                                s_time_sync_retry_count = 0U;
                                s_net_status.last_rtc_sync_ms = now_tick;
                                s_net_status.rtc_synced = true;

                                BSP_RTC_DateTime_t local_dt = {0};
                                BSP_RTC_GetDateTime(&local_dt);
                                LOG("Quectel_Engine: RTC synchronized from LTE network (local UTC+7: %04u-%02u-%02u %02u:%02u:%02u)\r\n",
                                    (unsigned)local_dt.year, (unsigned)local_dt.month, (unsigned)local_dt.day,
                                    (unsigned)local_dt.hour, (unsigned)local_dt.minute, (unsigned)local_dt.second);
                            } else {
                                LOG("Quectel_Engine: BSP_RTC_SetEpoch failed\r\n");
                            }
                        }
                    } else {
                        LOG("Quectel_Engine: Invalid QLTS date values: %d/%d/%d %d:%d:%d\r\n",
                            year, month, day, hour, minute, second);
                    }
                }
            }
        }

        /* Check for command completion */
        if (s_cmd_in_flight) {
            if (strcmp(line, "OK") == 0) {
                s_cmd_in_flight = false;
                /* Handled below by state */
                switch (s_net_status.state) {
                case QUECTEL_NET_STATE_AT_SYNC:
                    transition_to(QUECTEL_NET_STATE_ECHO_OFF, now_tick);
                    break;
                case QUECTEL_NET_STATE_ECHO_OFF:
                    transition_to(QUECTEL_NET_STATE_CHECK_INFO, now_tick);
                    break;
                case QUECTEL_NET_STATE_CHECK_INFO:
                    transition_to(QUECTEL_NET_STATE_CHECK_SIM, now_tick);
                    break;
                case QUECTEL_NET_STATE_CHECK_SIM:
                    if (s_net_status.sim_ready) {
                        transition_to(QUECTEL_NET_STATE_CONFIG_TIMEZONE, now_tick);
                    } else {
                        transition_to(QUECTEL_NET_STATE_ERROR_RECOVERY, now_tick);
                    }
                    break;
                case QUECTEL_NET_STATE_CONFIG_TIMEZONE:
                    transition_to(QUECTEL_NET_STATE_CHECK_CSQ, now_tick);
                    break;
                case QUECTEL_NET_STATE_CHECK_CSQ:
                    transition_to(QUECTEL_NET_STATE_CHECK_REG, now_tick);
                    break;
                case QUECTEL_NET_STATE_CHECK_REG:
                    if (s_net_status.net_registered) {
                        transition_to(QUECTEL_NET_STATE_CONFIG_APN, now_tick);
                    } else {
                        /* Stay in CHECK_REG until registered or timeout */
                        if (s_retry_count < 15U) {
                            s_state_tick = now_tick - 1000U; /* Retest soon */
                        } else {
                            transition_to(QUECTEL_NET_STATE_ERROR_RECOVERY, now_tick);
                        }
                    }
                    break;
                case QUECTEL_NET_STATE_CONFIG_APN:
                    transition_to(QUECTEL_NET_STATE_ACTIVATE_PDP, now_tick);
                    break;
                case QUECTEL_NET_STATE_ACTIVATE_PDP:
                    s_net_status.pdp_active = true;
                    transition_to(QUECTEL_NET_STATE_CHECK_IP, now_tick);
                    break;
                case QUECTEL_NET_STATE_CHECK_IP:
                    transition_to(QUECTEL_NET_STATE_READY, now_tick);
                    LOG("Quectel_Engine: LTE Data Network is fully READY!\r\n");
                    break;
                case QUECTEL_NET_STATE_READY:
                    s_ready_cmd = READY_CMD_NONE;
                    break;
                default:
                    break;
                }
            } else if (strncmp(line, "ERROR", 5) == 0 || strncmp(line, "+CME ERROR", 10) == 0) {
                LOG("Quectel_Engine: Error on state %d: %s\r\n", s_net_status.state, line);
                s_cmd_in_flight = false;
                if (s_net_status.state == QUECTEL_NET_STATE_READY) {
                    s_ready_cmd = READY_CMD_NONE;
                    /* Non-fatal: in READY state, CSQ or QLTS query error does not drop PDP */
                } else if (s_net_status.state == QUECTEL_NET_STATE_CHECK_INFO) {
                    /* Non-fatal: proceed to check sim even if model query had error */
                    transition_to(QUECTEL_NET_STATE_CHECK_SIM, now_tick);
                } else if (s_net_status.state == QUECTEL_NET_STATE_CONFIG_TIMEZONE) {
                    /* Non-fatal: proceed to network registration even if CTZU had error */
                    transition_to(QUECTEL_NET_STATE_CHECK_CSQ, now_tick);
                } else {
                    transition_to(QUECTEL_NET_STATE_ERROR_RECOVERY, now_tick);
                }
            }
        }
    }

    /* Check command timeout */
    if (s_cmd_in_flight) {
        if ((now_tick - s_state_tick) > s_cmd_timeout_ms) {
            LOG("Quectel_Engine: Command timeout in state %d\r\n", s_net_status.state);
            s_cmd_in_flight = false;
            if (s_net_status.state == QUECTEL_NET_STATE_READY) {
                s_ready_cmd = READY_CMD_NONE;
            } else if (s_net_status.state == QUECTEL_NET_STATE_CHECK_INFO) {
                transition_to(QUECTEL_NET_STATE_CHECK_SIM, now_tick);
            } else if (s_net_status.state == QUECTEL_NET_STATE_CONFIG_TIMEZONE) {
                transition_to(QUECTEL_NET_STATE_CHECK_CSQ, now_tick);
            } else {
                transition_to(QUECTEL_NET_STATE_ERROR_RECOVERY, now_tick);
            }
        }
        return; /* Waiting for response */
    }

    /* Main state machine driving transmission of AT commands */
    switch (s_net_status.state) {
    case QUECTEL_NET_STATE_OFF:
        break;

    case QUECTEL_NET_STATE_POWERING_ON:
        if (BSP_Quectel_IsReady()) {
            LOG("Quectel_Engine: Modem hardware ready, starting AT sync\r\n");
            transition_to(QUECTEL_NET_STATE_AT_SYNC, now_tick);
        }
        break;

    case QUECTEL_NET_STATE_AT_SYNC:
        if ((now_tick - s_state_tick) >= 500U) {
            s_state_tick = now_tick;
            s_cmd_in_flight = true;
            s_cmd_timeout_ms = AT_CMD_TIMEOUT_DEFAULT_MS;
            s_retry_count++;
            if (s_retry_count > 20U) {
                LOG("Quectel_Engine: AT sync failed after 20 retries\r\n");
                transition_to(QUECTEL_NET_STATE_ERROR_RECOVERY, now_tick);
            } else {
                BSP_Quectel_SendCmd("AT");
            }
        }
        break;

    case QUECTEL_NET_STATE_ECHO_OFF:
        s_cmd_in_flight = true;
        s_cmd_timeout_ms = AT_CMD_TIMEOUT_DEFAULT_MS;
        s_state_tick = now_tick;
        BSP_Quectel_SendCmd("ATE0");
        break;

    case QUECTEL_NET_STATE_CHECK_INFO:
        s_cmd_in_flight = true;
        s_cmd_timeout_ms = AT_CMD_TIMEOUT_DEFAULT_MS;
        s_state_tick = now_tick;
        BSP_Quectel_SendCmd("AT+CGMM");
        break;

    case QUECTEL_NET_STATE_CHECK_SIM:
        s_cmd_in_flight = true;
        s_cmd_timeout_ms = AT_CMD_TIMEOUT_DEFAULT_MS;
        s_state_tick = now_tick;
        BSP_Quectel_SendCmd("AT+CPIN?");
        break;

    case QUECTEL_NET_STATE_CONFIG_TIMEZONE:
        s_cmd_in_flight = true;
        s_cmd_timeout_ms = AT_CMD_TIMEOUT_DEFAULT_MS;
        s_state_tick = now_tick;
        BSP_Quectel_SendCmd("AT+CTZU=1");
        break;

    case QUECTEL_NET_STATE_CHECK_CSQ:
        s_cmd_in_flight = true;
        s_cmd_timeout_ms = AT_CMD_TIMEOUT_DEFAULT_MS;
        s_state_tick = now_tick;
        BSP_Quectel_SendCmd("AT+CSQ");
        break;

    case QUECTEL_NET_STATE_CHECK_REG:
        if ((now_tick - s_state_tick) >= 1500U) {
            s_cmd_in_flight = true;
            s_cmd_timeout_ms = AT_CMD_TIMEOUT_DEFAULT_MS;
            s_state_tick = now_tick;
            s_retry_count++;
            BSP_Quectel_SendCmd("AT+CEREG?");
        }
        break;

    case QUECTEL_NET_STATE_CONFIG_APN: {
        char apn_cmd[64];
        snprintf(apn_cmd, sizeof(apn_cmd), "AT+QICSGP=1,1,\"%s\",\"\",\"\"", s_apn);
        s_cmd_in_flight = true;
        s_cmd_timeout_ms = AT_CMD_TIMEOUT_DEFAULT_MS;
        s_state_tick = now_tick;
        BSP_Quectel_SendCmd(apn_cmd);
        break;
    }

    case QUECTEL_NET_STATE_ACTIVATE_PDP:
        s_cmd_in_flight = true;
        s_cmd_timeout_ms = AT_CMD_TIMEOUT_ACT_MS;
        s_state_tick = now_tick;
        BSP_Quectel_SendCmd("AT+QIACT=1");
        break;

    case QUECTEL_NET_STATE_CHECK_IP:
        s_cmd_in_flight = true;
        s_cmd_timeout_ms = AT_CMD_TIMEOUT_DEFAULT_MS;
        s_state_tick = now_tick;
        BSP_Quectel_SendCmd("AT+QIACT?");
        break;

    case QUECTEL_NET_STATE_READY: {
        /* Check if time synchronization is needed */
        bool need_time_sync = false;
        if (!s_time_sync_done_once) {
            uint32_t interval = (s_time_sync_retry_count > 0U) ? TIME_SYNC_RETRY_MS : TIME_SYNC_INITIAL_DELAY_MS;
            if ((now_tick - s_state_tick) >= interval) {
                need_time_sync = true;
            }
        } else {
            if ((now_tick - s_last_time_sync_tick) >= TIME_SYNC_INTERVAL_MS) {
                need_time_sync = true;
            }
        }

        if (need_time_sync) {
            s_ready_cmd = READY_CMD_QLTS;
            s_cmd_in_flight = true;
            s_cmd_timeout_ms = AT_CMD_TIMEOUT_DEFAULT_MS;
            s_state_tick = now_tick;
            BSP_Quectel_SendCmd("AT+QLTS=1");
        } else if ((now_tick - s_net_status.last_csq_update_ms) >= CSQ_POLL_INTERVAL_MS) {
            s_ready_cmd = READY_CMD_CSQ;
            s_cmd_in_flight = true;
            s_cmd_timeout_ms = AT_CMD_TIMEOUT_DEFAULT_MS;
            s_state_tick = now_tick;
            BSP_Quectel_SendCmd("AT+CSQ");
        }
        break;
    }

    case QUECTEL_NET_STATE_ERROR_RECOVERY:
        if ((now_tick - s_state_tick) >= RETRY_BACKOFF_MS) {
            LOG("Quectel_Engine: Retrying network connection sequence...\r\n");
            transition_to(QUECTEL_NET_STATE_AT_SYNC, now_tick);
        }
        break;

    default:
        transition_to(QUECTEL_NET_STATE_AT_SYNC, now_tick);
        break;
    }
}
