/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Secure Enclave service stubs (ticket 0003). There is no SE on the host;
 * everything answers success with fixed, deterministic data so tests can
 * assert on device identity.
 */

#include <string.h>

#include <zephyr/kernel.h>

#include <se_service.h>

int se_service_get_se_revision(uint8_t *prev)
{
	if (prev == NULL) {
		return -EINVAL;
	}

	/* Silicon memcpy()s up to 80 non-terminated bytes; "EMU" plus a NUL
	 * stays well inside every caller's buffer either way. */
	memcpy(prev, "EMU", 4);
	return 0;
}

int se_system_get_eui_extension(bool is_eui48, uint8_t *eui_extension)
{
	/* Fixed extension -> stable EUI-64 2C:F7:F1:E3:00:00:00:01 through
	 * frame.get_eui() (lua_system.c prepends the OUI). */
	static const uint8_t ext64[5] = {0xE3, 0x00, 0x00, 0x00, 0x01};

	if (eui_extension == NULL) {
		return -EINVAL;
	}

	memcpy(eui_extension, &ext64[is_eui48 ? 2 : 0], is_eui48 ? 3 : 5);
	return 0;
}

int se_service_get_rnd_num(uint8_t *buffer, uint16_t length)
{
	/* xorshift32 keyed off the cycle counter: no entropy-driver Kconfig
	 * dependency, and nothing security-relevant consumes this on emu
	 * (the silicon users — BLE security / SE mgmt — are compiled out). */
	static uint32_t state;

	if (buffer == NULL) {
		return -EINVAL;
	}

	if (state == 0) {
		state = k_cycle_get_32() | 1;
	}

	for (uint16_t i = 0; i < length; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		buffer[i] = (uint8_t)state;
	}

	return 0;
}
