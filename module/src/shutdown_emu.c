/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Emulator ship-mode ("shutdown") driver (ticket 0003).
 *
 * lua_system.c:332 and lua_button.c:264 do DEVICE_DT_GET(DT_ALIAS(shutdown))
 * unconditionally, so the alias must exist and bind to a ready device —
 * boards/native_sim.overlay adds an `sm-gpio` node (the firmware's own
 * binding, dts/bindings/sm-gpio.yaml) wired to the emulated gpio0 and
 * aliases it. This driver claims that compatible on the emulator and
 * implements the one-call API from <zephyr/drivers/sm/sm.h>: on silicon
 * shutdown() latches the ship-mode GPIO and the SoC browns out; here the
 * process exits.
 *
 * The real driver is excluded with CONFIG_SM_GPIO=n (native_sim.conf) —
 * both it and this file define the global shutdown() and the DT instance,
 * so exactly one may be in the build.
 */

#define DT_DRV_COMPAT sm_gpio

#include <zephyr/device.h>
#include <zephyr/drivers/sm/sm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

#include "posix_board_if.h" /* posix_exit() */

LOG_MODULE_REGISTER(emu_shutdown, CONFIG_HALO_LOG_LEVEL);

int shutdown(const struct device *dev)
{
	ARG_UNUSED(dev);

	LOG_WRN("Ship mode: emulator powering off — exiting");
	log_panic(); /* callers only k_sleep(100ms) before the power drops */
	posix_exit(0);

	return 0;
}

static int emu_sm_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#define EMU_SM_DEFINE(inst)                                                                        \
	DEVICE_DT_INST_DEFINE(inst, emu_sm_init, NULL, NULL, NULL, POST_KERNEL,                    \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);

DT_INST_FOREACH_STATUS_OKAY(EMU_SM_DEFINE)
