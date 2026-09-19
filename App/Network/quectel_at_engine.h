/**
 * @file    quectel_at_engine.h
 * @brief   Application Network Layer - Quectel Cellular AT Engine & FSM
 * @note    Non-blocking state machine for SIM card, 4G network registration,
 *          and PDP context management (EC200U, EC25, EG915...).
 */

#ifndef QUECTEL_AT_ENGINE_H
#define QUECTEL_AT_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    QUECTEL_NET_STATE_OFF = 0,
    QUECTEL_NET_STATE_POWERING_ON,
    QUECTEL_NET_STATE_AT_SYNC,
    QUECTEL_NET_STATE_ECHO_OFF,
    QUECTEL_NET_STATE_CHECK_INFO,
    QUECTEL_NET_STATE_CHECK_SIM,
    QUECTEL_NET_STATE_CHECK_CSQ,
    QUECTEL_NET_STATE_CHECK_REG,
    QUECTEL_NET_STATE_CONFIG_APN,
    QUECTEL_NET_STATE_ACTIVATE_PDP,
    QUECTEL_NET_STATE_CHECK_IP,
    QUECTEL_NET_STATE_READY,
    QUECTEL_NET_STATE_ERROR_RECOVERY
} QuectelNetState_t;

typedef struct {
    QuectelNetState_t state;
    bool powered;
    bool sim_ready;
    bool net_registered;
    bool pdp_active;
    uint8_t csq_rssi;           /* 0..31, 99 = unknown */
    char model[24];             /* E.g. "EC200U-CN" */
    char ip_addr[20];           /* E.g. "10.154.22.8" */
    uint32_t last_csq_update_ms;
} QuectelNetStatus_t;

/**
 * @brief Initialize the Quectel AT Engine and trigger hardware power-on.
 */
void QuectelEngine_Init(void);

/**
 * @brief Main periodic process function. Must be called in App_Loop().
 * @param now_tick Current tick in milliseconds.
 */
void QuectelEngine_Process(uint32_t now_tick);

/**
 * @brief Check if LTE data network is active and ready for HTTP/IP communication.
 */
bool QuectelEngine_IsNetReady(void);

/** Temporarily give the UART RX line parser exclusively to OTA QHTTP. */
void QuectelEngine_SetOtaExclusive(bool exclusive);

/**
 * @brief Get current network and modem status snapshot.
 */
void QuectelEngine_GetStatus(QuectelNetStatus_t *out_status);

/**
 * @brief Set custom APN (default is "v-internet" for Viettel).
 */
void QuectelEngine_SetApn(const char *apn);

#ifdef __cplusplus
}
#endif

#endif /* QUECTEL_AT_ENGINE_H */
