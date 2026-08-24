/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Power-management stubs (ticket 0003), two halves:
 *
 * 1. The Alif power_mgr HAL (see ../include/power_mgr.h): no-op successes,
 *    so modules/halo/src/pm_manager.c runs UNMODIFIED. Its callback
 *    registry, suspend/resume chains and wakeup handshake are pure Zephyr
 *    and keep working — frame.light_sleep()/standby() genuinely park and
 *    fire the registered suspend/resume handlers; only the silicon state
 *    transitions vanish.
 *
 * 2. The soc PM hooks the POSIX arch does not provide. HALO_PM_MANAGER
 *    selects PM, and emulator/module/Kconfig asserts HAS_PM for it; that
 *    makes these two symbols this module's to supply (the 0002 Kconfig
 *    comment points here). Only SOFT_OFF can ever reach pm_state_set():
 *    native_sim's DT declares no cpu-power-states, so the idle policy
 *    never picks a state on its own, and SOFT_OFF is the one state
 *    pm_manager forces (halo_pm_sleep_deep). SOFT_OFF means "SoC powers
 *    off, reboot through MCUboot on wake" — the emulator equivalent is
 *    exiting the process.
 */

#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/pm/pm.h>

#include <power_mgr.h>

#include "posix_board_if.h" /* posix_exit() */

LOG_MODULE_REGISTER(emu_pm, CONFIG_HALO_LOG_LEVEL);

/* ------------------------------------------------ Alif power_mgr HAL stubs */

int power_mgr_set_offprofile(pm_state_mode_type_e pm_mode)
{
	ARG_UNUSED(pm_mode);
	return 0;
}

void power_mgr_ready_for_sleep(void)
{
}

/* A fresh process is always a cold boot, so pm_manager never consults the
 * (meaningless-on-host) wakeup reason: resolve/get below are just for
 * halo_pm_init()'s boot log line. */
bool power_mgr_cold_boot(void)
{
	return true;
}

uint32_t power_mgr_get_wakeup_reason(void)
{
	return 0;
}

uint32_t power_mgr_resolve_wakeup_reason(void)
{
	return 0;
}

void power_mgr_set_subsys_off_period(uint32_t period_ms)
{
	ARG_UNUSED(period_ms);
}

int power_mgr_set_rtc_wakeup_enable(bool enable)
{
	ARG_UNUSED(enable);
	return 0;
}

int power_mgr_set_wakeup_sources(bool button, bool mic, bool rtc)
{
	LOG_DBG("wakeup sources (emu no-op): button=%d mic=%d rtc=%d", button, mic, rtc);
	return 0;
}

void power_mgr_get_boot_pending(uint32_t *ispr0, uint32_t *ispr1)
{
	if (ispr0 != NULL) {
		*ispr0 = 0;
	}
	if (ispr1 != NULL) {
		*ispr1 = 0;
	}
}

/* --------------------------------------------------------- soc PM hooks */

void pm_state_set(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(substate_id);

	if (state == PM_STATE_SOFT_OFF) {
		LOG_WRN("SOFT_OFF (deep sleep): emulator powering off — exiting");
		log_panic(); /* pm_manager already flushed; harmless twice */
		posix_exit(0);
	}

	/* Unreachable (no other state is ever selected — see file header). */
	LOG_ERR("Unexpected PM state %d on emulator", state);
}

void pm_state_exit_post_ops(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(state);
	ARG_UNUSED(substate_id);

	/* Soc convention: suspend path enters with interrupts locked and the
	 * post-op re-enables them (mirrors the in-tree soc implementations). */
	irq_unlock(0);
}
