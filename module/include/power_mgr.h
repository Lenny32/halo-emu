/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Emulator stand-in for the Alif power-manager HAL header
 * (alif/subsys/powermgr/pm/power_mgr.h). On silicon that directory is added
 * to the include path by CONFIG_ALIF_POWER_MGR_LIB, which depends on
 * HAS_ALIF_POWER_MANAGER and so is off on native_sim — this copy of the API
 * lets modules/halo/src/pm_manager.c compile UNMODIFIED; the no-op
 * implementations live in ../src/pm_stubs.c.
 *
 * The enums must stay bit-identical to the Alif header: pm_manager.c
 * compares power_mgr_resolve_wakeup_reason() against PM_WAKEUP_* directly.
 */

#ifndef HALO_EMU_POWER_MGR_H_
#define HALO_EMU_POWER_MGR_H_

#include <inttypes.h>
#include <stdbool.h>

typedef enum {
	PM_STATE_MODE_IDLE,
	PM_STATE_MODE_STANDBY,
	PM_STATE_MODE_STOP
} pm_state_mode_type_e;

typedef enum {
	PM_WAKEUP_BLE,
	PM_WAKEUP_RTC,
	PM_WAKEUP_LPGPIO0,
	PM_WAKEUP_LPGPIO1,
} pm_wakeup_source_e;

int power_mgr_set_offprofile(pm_state_mode_type_e pm_mode);
void power_mgr_ready_for_sleep(void);
bool power_mgr_cold_boot(void);
uint32_t power_mgr_get_wakeup_reason(void);
uint32_t power_mgr_resolve_wakeup_reason(void);
void power_mgr_set_subsys_off_period(uint32_t period_ms);
int power_mgr_set_rtc_wakeup_enable(bool enable);
int power_mgr_set_wakeup_sources(bool button, bool mic, bool rtc);
void power_mgr_get_boot_pending(uint32_t *ispr0, uint32_t *ispr1);

#endif /* HALO_EMU_POWER_MGR_H_ */
