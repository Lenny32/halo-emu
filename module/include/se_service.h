/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Emulator stand-in for the Alif SE services header
 * (modules/hal/alif/se_services/zephyr/include/se_service.h), which only
 * exists in Alif-silicon builds. main.c and several Lua bindings include it
 * unconditionally; this header (on the include path only under
 * CONFIG_HALO_EMULATOR) declares just the subset those files link against,
 * implemented in ../src/se_stubs.c.
 *
 * The profile/boot/TOC APIs (run_profile_t, se_service_boot_es0, ...) are
 * deliberately absent: their only users are se_mgmt.c and the Alif BLE
 * manager, both hardware-only (CONFIG_HALO_SE_MGMT=n, CONFIG_HALO_BLE_MANAGER=n
 * on the emulator) — an emu file reaching for them should fail loudly here.
 */

#ifndef HALO_EMU_SE_SERVICE_H_
#define HALO_EMU_SE_SERVICE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fills up to VERSION_RESPONSE_LENGTH (80) bytes, NOT NUL-terminated on
 * silicon — callers over-size and terminate themselves (lua_system.c). The
 * emu stub returns "EMU" (surfaces as frame.get_se_revision()). */
int se_service_get_se_revision(uint8_t *prev);

/* EUI extension: 3 bytes for EUI-48, 5 bytes for EUI-64 (deterministic on
 * emu so tests see a stable device identity). */
int se_system_get_eui_extension(bool is_eui48, uint8_t *eui_extension);

int se_service_get_rnd_num(uint8_t *buffer, uint16_t length);

#ifdef __cplusplus
}
#endif

#endif /* HALO_EMU_SE_SERVICE_H_ */
