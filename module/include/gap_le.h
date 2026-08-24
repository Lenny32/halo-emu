/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Emulator stand-in for the Alif BLE ROM host header of the same name
 * (modules/hal/alif/ble/v1_2/include/ip/hl/). halo/ble_security.h includes
 * the ROM headers unconditionally, and lua_button.c includes ble_security.h
 * for halo_ble_sec_pairing_window_open() — so the header set must parse on
 * native_sim even though every BLE implementation file is compiled out
 * (CONFIG_HALO_BLE_MANAGER=n).
 *
 * Only the type NAMES matter here. The layouts deliberately do NOT match
 * silicon — nothing on the emulator may store or transport these
 * (CONFIG_HALO_BLE_BOND_STORAGE=n stays a hard requirement).
 */

#ifndef HALO_EMU_GAP_LE_H_
#define HALO_EMU_GAP_LE_H_

#include <stdint.h>

typedef struct {
	uint8_t addr[6];
	uint8_t addr_type;
} gap_bdaddr_t;

#endif /* HALO_EMU_GAP_LE_H_ */
