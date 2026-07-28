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
/*
 * Caller scope. Deep shared code (uclass, device core) cannot tell which
 * consumer it is running for, so the caller sets this around the call it cares
 * about and the probes below test it. Without it, a marker in uclass code
 * blinks for whichever device got there first and proves nothing.
 */
extern int boox_scope;
#define BOOX_STEP_AT(n)		do { if ((n) == UBOOT_CURRENT_STEP) boox_step_blink(); } while (0)
/* Same, but only when `cond` holds -- use it to scope a probe to one device,
 * otherwise a shared code path blinks for whichever consumer reaches it first
 * and the reading says nothing about the device under investigation. */
#define BOOX_STEP_AT_IF(n, cond)	do { if ((n) == UBOOT_CURRENT_STEP && (cond)) boox_step_blink(); } while (0)
#else
#define BOOX_STEP_AT(n)		do { } while (0)
#define BOOX_STEP_AT_IF(n, cond)	do { } while (0)
#endif

#endif
