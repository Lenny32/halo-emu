/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Emulator display seam (ticket 0007): read-side accessors over the fake
 * CDC200 scanout buffer that canvas/lua_display.c draw into. Consumers:
 * the SDL presenter (0008) and the control-plane screenshot (0009).
 */

#ifndef HALO_EMU_DISPLAY_H
#define HALO_EMU_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Get the emulated scanout framebuffer.
 *
 * Physical orientation, row-major RGB888 — uint8_t[height][width][3], the
 * exact buffer canvas rasterizes into (zero-copy, like the hardware). The
 * canvas 90° rotation (LOG_* vs PHYS_*) is already baked into the pixels.
 *
 * @param width  Optional out: buffer width in pixels.
 * @param height Optional out: buffer height in pixels.
 * @return Pointer to the framebuffer (static storage, never NULL).
 */
const uint8_t *halo_emu_display_fb(uint16_t *width, uint16_t *height);

/**
 * @brief Whether scanout is enabled (cdc200_set_enable state).
 *
 * Mirrors the hardware power-save semantics: false at boot, true only
 * between display resume and suspend. A presenter must show black/blank
 * while false, whatever the buffer contains.
 */
bool halo_emu_display_scanout_enabled(void);

/**
 * @brief Current pan calibration (vga020 border-register offset).
 *
 * Applies to the whole framebuffer at presentation time — never to draw
 * coordinates. Range on hardware is [-50, 50] per axis.
 */
void halo_emu_display_get_pan(int8_t *x_offset, int8_t *y_offset);

#endif /* HALO_EMU_DISPLAY_H */
