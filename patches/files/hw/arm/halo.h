/*
 * Halo machine — cross-model hooks.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_HALO_H
#define HW_ARM_HALO_H

/*
 * Make the next system reset keep ITCM/DTCM contents.  The machine's
 * default reset zeroes both TCMs (an SE SoC reset power-cycles them on
 * hardware, so __noinit does not survive); a CMSDK-watchdog reset is a
 * plain warm reset that retains them — the firmware's watchdog-fired
 * magic (a __noinit variable) depends on that.
 */
void halo_sram_preserve_next_reset(void);

#endif
