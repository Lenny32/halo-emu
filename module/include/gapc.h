/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Emulator stand-in for the Alif BLE ROM host header — see gap_le.h for why
 * this exists and why the layouts intentionally differ from silicon.
 */

#ifndef HALO_EMU_GAPC_H_
#define HALO_EMU_GAPC_H_

#include <stdint.h>

typedef struct {
	uint32_t opaque[4]; /* emu placeholder, never persisted */
} gapc_bond_data_t;

#endif /* HALO_EMU_GAPC_H_ */
