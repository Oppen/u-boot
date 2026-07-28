/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * BOOX bring-up probe. One shared, non-inlinable implementation so a card can
 * be VERIFIED to contain its marker: `nm u-boot | grep boox_step_blink` shows
 * the symbol, and the call sites are visible as real `bl` instructions.
 *
 * This exists because two probe builds shipped with the marker silently
 * compiled out -- once inlined away, once inserted into a #if-disabled copy of
 * a function -- and both produced "solid" readings that meant nothing.
 */
#ifndef __BOOX_PROBE_H
#define __BOOX_PROBE_H

#ifdef UBOOT_CURRENT_STEP
void boox_step_blink(void);
#define BOOX_STEP_AT(n)		do { if ((n) == UBOOT_CURRENT_STEP) boox_step_blink(); } while (0)
#else
#define BOOX_STEP_AT(n)		do { } while (0)
#endif

#endif
