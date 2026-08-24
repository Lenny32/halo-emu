/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small link-level stand-ins (ticket 0003) for symbols the unconditional
 * firmware paths reference but whose providers cannot exist on native_sim.
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

#include <halo/led_manager.h>

#include "posix_board_if.h" /* posix_exit() */

LOG_MODULE_REGISTER(emu_stubs, CONFIG_HALO_LOG_LEVEL);

/* sys_reboot() (CONFIG_REBOOT=y for pm_manager's failsafes and the VM
 * reset path) needs the arch hook, which the POSIX arch does not provide.
 * A rebooted emulator is a restarted process — exit and let the caller
 * relaunch (restart-wrapper note lands in EMULATOR.md with ticket 0005). */
void sys_arch_reboot(int type)
{
	LOG_WRN("sys_reboot(%d): emulator exiting (relaunch = reboot)", type);
	log_panic();
	posix_exit(0);
}

/* vga020_set_pan() lived here until ticket 0007 moved it into
 * display_fake.c, where the recorded offset feeds the presenter (0008). */

#ifndef CONFIG_HALO_LED_MANAGER
/* lua_button.c:258 clears the LED unconditionally before ship mode; the LED
 * manager itself stays off until its fake (ticket 0013, which must drop
 * this stub when it re-enables CONFIG_HALO_LED_MANAGER). */
int halo_led_clear_state(halo_led_priority_t priority)
{
	ARG_UNUSED(priority);
	return 0;
}
#endif /* !CONFIG_HALO_LED_MANAGER */
