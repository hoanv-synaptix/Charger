/**
 * @file charge_energy_storage.h
 * @brief Persistent accumulated charge and energy counters.
 */

#ifndef CHARGE_ENERGY_STORAGE_H
#define CHARGE_ENERGY_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ChargeEnergyStorage_Init(void);
void ChargeEnergyStorage_Process(uint32_t now_tick, bool charging,
                                 float total_charged_ah,
                                 float total_energy_kwh);
void ChargeEnergyStorage_SaveNow(float total_charged_ah,
                                 float total_energy_kwh);
void ChargeEnergyStorage_Get(float *total_charged_ah,
                             float *total_energy_kwh);
bool ChargeEnergyStorage_Reset(void);
bool ChargeEnergyStorage_TakeResetRequest(void);

#ifdef __cplusplus
}
#endif

#endif /* CHARGE_ENERGY_STORAGE_H */
