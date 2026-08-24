/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * SDL display presenter (ticket 0008): makes the fake CDC200 scanout buffer
 * (display_fake.c, ticket 0007) visible in the sdl_dc window.
 *
 * Hardware scans the framebuffer out continuously; here a ~30 Hz thread
 * stands in for that: while scanout is enabled (cdc200_set_enable state via
 * halo/emu_display.h) it converts the canvas RGB888 buffer to ARGB8888 and
 * display_write()s the full frame to the chosen zephyr,panel (sdl_dc).
 * Scripts that draw without an explicit show() therefore still update the
 * window, exactly like on the device.
 *
 * Pan (the vga020 border-register calibration) is applied here, at
 * presentation time, as a whole-frame shift — never to draw coordinates.
 * Sign convention mirrors drivers/display/display_vga020.c: +x shifts the
 * image right, +y shifts it down; pixels shifted in from outside are black.
 *
 * Blanking needs no code here: lua_display.c's suspend/resume handlers call
 * display_blanking_on/off on the panel device themselves, and the SDL
 * driver implements both (blank shows black, unblank re-presents its
 * texture). This thread merely stops blitting while scanout is off, so the
 * buffer contents cannot leak into a blanked window.
 *
 * The canvas rasterizes into the framebuffer concurrently with this thread
 * reading it — a mid-draw blit shows a torn frame for one refresh, which is
 * precisely what continuous hardware scanout does, so no locking is added.
 *
 * Headless (CI, ticket 0017): SDL_VIDEODRIVER=dummy — everything here runs
 * unchanged against SDL's dummy backend (the software renderer is already
 * forced via CONFIG_SDL_DISPLAY_USE_HARDWARE_ACCELERATOR=n, see
 * boards/native_sim.conf).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/display.h>
#include <string.h>

#include <halo/emu_display.h>

LOG_MODULE_REGISTER(emu_present, CONFIG_HALO_LOG_LEVEL);

/* Same nodes lua_display.c uses: sdl_dc is both chosen zephyr,display
 * (canvas geometry) and chosen zephyr,panel (blanking target) on emu. */
#define EMU_PANEL_NODE DT_CHOSEN(zephyr_panel)
#define EMU_DISP_NODE  DT_CHOSEN(zephyr_display)
#define EMU_DISP_W     DT_PROP(EMU_DISP_NODE, width)
#define EMU_DISP_H     DT_PROP(EMU_DISP_NODE, height)

#define EMU_FRAME_MS (1000 / CONFIG_HALO_EMU_DISPLAY_FPS)

static const struct device *const emu_panel = DEVICE_DT_GET(EMU_PANEL_NODE);

/* ARGB8888 staging frame handed to display_write(). SDL_PIXELFORMAT_ARGB8888
 * is a packed 32-bit format, so 0xAARRGGBB uint32_t values are correct on
 * any host endianness. */
static uint32_t emu_stage[EMU_DISP_H][EMU_DISP_W];

/* Copy of the last-presented RGB888 frame: skips the SDL blit on idle
 * frames so a static screen costs nothing between changes. */
static uint8_t emu_shadow[EMU_DISP_H][EMU_DISP_W][3];

static void emu_compose(const uint8_t (*fb)[EMU_DISP_W][3], int pan_x, int pan_y)
{
	for (int dy = 0; dy < EMU_DISP_H; dy++) {
		int sy = dy - pan_y;

		if (sy < 0 || sy >= EMU_DISP_H) {
			memset(emu_stage[dy], 0, sizeof(emu_stage[dy]));
			continue;
		}
		for (int dx = 0; dx < EMU_DISP_W; dx++) {
			int sx = dx - pan_x;

			if (sx < 0 || sx >= EMU_DISP_W) {
				emu_stage[dy][dx] = 0xFF000000u;
			} else {
				emu_stage[dy][dx] = 0xFF000000u |
						    ((uint32_t)fb[sy][sx][0] << 16) |
						    ((uint32_t)fb[sy][sx][1] << 8) |
						    (uint32_t)fb[sy][sx][2];
			}
		}
	}
}

static void emu_present(void)
{
	const struct display_buffer_descriptor desc = {
		.buf_size = sizeof(emu_stage),
		.width = EMU_DISP_W,
		.height = EMU_DISP_H,
		.pitch = EMU_DISP_W,
	};
	int ret = display_write(emu_panel, 0, 0, &desc, emu_stage);

	if (ret) {
		LOG_ERR("display_write failed: %d", ret);
	}
}

static void emu_presenter_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	bool was_on = false;
	int8_t last_px = 0;
	int8_t last_py = 0;

	if (!device_is_ready(emu_panel)) {
		LOG_ERR("panel device not ready; presenter disabled");
		return;
	}

	LOG_INF("SDL presenter running (%d Hz, %dx%d)",
		CONFIG_HALO_EMU_DISPLAY_FPS, EMU_DISP_W, EMU_DISP_H);

	while (true) {
		bool on = halo_emu_display_scanout_enabled();

		if (on) {
			const uint8_t *fb = halo_emu_display_fb(NULL, NULL);
			int8_t px, py;

			halo_emu_display_get_pan(&px, &py);

			/* Re-present on the off->on edge even if the pixels
			 * did not change (blank/unblank of a static frame). */
			bool dirty = !was_on || px != last_px || py != last_py ||
				     memcmp(fb, emu_shadow, sizeof(emu_shadow)) != 0;

			if (dirty) {
				memcpy(emu_shadow, fb, sizeof(emu_shadow));
				emu_compose((const uint8_t (*)[EMU_DISP_W][3])fb, px, py);
				emu_present();
				last_px = px;
				last_py = py;
			}
		}
		was_on = on;
		k_msleep(EMU_FRAME_MS);
	}
}

K_THREAD_DEFINE(emu_presenter, 4096, emu_presenter_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
