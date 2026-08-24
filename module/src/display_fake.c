/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display fake (ticket 0007, reworked — see the ticket for why this is not
 * a firmware-tree HAL extraction): lua_display.c's canvas path is compiled
 * via a compiler-level CONFIG_CDC200 define (module CMakeLists), and this
 * file provides the three hardware entry points it then needs:
 *
 *   - cdc200_get_framebuffer(): hands canvas a static host RGB888 buffer,
 *     standing in for the CDC200 layer-0 scanout buffer (zero-copy on hw).
 *   - cdc200_set_enable(): records scanout on/off (power-save semantics).
 *   - vga020_set_pan(): records the border-register offset (absorbed from
 *     misc_stubs.c; on hardware it shifts the whole framebuffer, never the
 *     draw coordinates).
 *
 * Nothing presents the buffer yet: hardware scans it out continuously, so
 * the SDL window blit (explicit present + refresh timer) is ticket 0008,
 * reading it through the halo/emu_display.h accessors below. The screenshot
 * path of the control plane (0009) reads the same seam.
 *
 * The dsi/panel devices need no fake: chosen zephyr,panel is the sdl_dc
 * device (blanking works, PM returns -ENOSYS which lua_display tolerates)
 * and the MIPI_DSI path is compiled out on native_sim.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/display/cdc200.h>

#include <drivers/display_vga020.h>
#include <halo/emu_display.h>

LOG_MODULE_REGISTER(emu_display, CONFIG_HALO_LOG_LEVEL);

/* Same source of truth as canvas.h's PHYS_WIDTH/PHYS_HEIGHT: the chosen
 * zephyr,display node (sdl_dc, sized 256x256 in native_sim.overlay to match
 * the hardware vga020 panel). */
#define EMU_DISP_NODE DT_CHOSEN(zephyr_display)
#define EMU_DISP_W    DT_PROP(EMU_DISP_NODE, width)
#define EMU_DISP_H    DT_PROP(EMU_DISP_NODE, height)

/* [row][col][rgb], matching canvas.c's uint8_t (*)[PHYS_WIDTH][3] layout. */
static uint8_t emu_fb[EMU_DISP_H][EMU_DISP_W][3];

static bool emu_scanout;
static int8_t emu_pan_x;
static int8_t emu_pan_y;

/* --- CDC200 API stand-ins (prototypes from the fork's cdc200.h) --------- */

void cdc200_get_framebuffer(const struct device *dev, uint8_t idx, struct cdc200_fb_desc *fb)
{
	ARG_UNUSED(dev);

	if (idx != 0) {
		/* lua_display.c only uses layer 0; a second layer has no
		 * backing store on the emulator. */
		LOG_WRN("cdc200 layer %u requested; only layer 0 is faked", idx);
		fb->fb_addr = NULL;
		fb->fb_size = 0;
		return;
	}

	fb->fb_addr = &emu_fb[0][0][0];
	fb->fb_size = sizeof(emu_fb);
}

void cdc200_set_enable(const struct device *dev, bool enable)
{
	ARG_UNUSED(dev);

	emu_scanout = enable;
	LOG_DBG("scanout %s", enable ? "enabled" : "disabled");
}

/* --- VGA020 pan (border register) --------------------------------------- */

int vga020_set_pan(const struct device *dev, int8_t x_offset, int8_t y_offset)
{
	ARG_UNUSED(dev);

	/* The real driver clamps to [-50, 50] before touching the border
	 * registers (display_vga020.c) — mirror that so the presenter and
	 * screenshot seam report the effective offset, not the request. */
	emu_pan_x = CLAMP(x_offset, -50, 50);
	emu_pan_y = CLAMP(y_offset, -50, 50);
	LOG_DBG("pan (%d, %d)", emu_pan_x, emu_pan_y);
	return 0;
}

/* --- Emulator-side accessors (presenter/screenshot seam, 0008/0009) ----- */

const uint8_t *halo_emu_display_fb(uint16_t *width, uint16_t *height)
{
	if (width) {
		*width = EMU_DISP_W;
	}
	if (height) {
		*height = EMU_DISP_H;
	}
	return &emu_fb[0][0][0];
}

bool halo_emu_display_scanout_enabled(void)
{
	return emu_scanout;
}

void halo_emu_display_get_pan(int8_t *x_offset, int8_t *y_offset)
{
	if (x_offset) {
		*x_offset = emu_pan_x;
	}
	if (y_offset) {
		*y_offset = emu_pan_y;
	}
}
