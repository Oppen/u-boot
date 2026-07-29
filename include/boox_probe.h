/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * BOOX bring-up probe -- THE single implementation. Do not write another.
 *
 * Every instrumentation bug in this port has been a property of a COPY, not of
 * the idea: a `static` marker that got inlined away so no call site existed; a
 * marker placed in a #if-disabled copy of a function; a tail-called marker that
 * a bl-only grep reported as absent; a second blink symbol the build-time
 * checker did not know about. Each one produced a "step not reached" reading
 * that was false. So there is one implementation here, it is noinline and
 * non-static, and scripts/build-step-images.sh verifies a real branch to it
 * before shipping a card.
 *
 * SIGNALLING RULES, learned on hardware:
 *  - Markers blink FOREVER. A pattern that stops is not a marker.
 *  - Meaning is carried by DUTY RATIO, never by rate: rate is not judgeable by
 *    eye, and a wrong timebase scales every pattern by the same factor while
 *    leaving ratios intact.
 *  - A fixed level means the code never got there, or a read hung the bus.
 *
 * The one exception left in the tree is the SPL's own copy in
 * arch/arm/mach-rockchip/rk3026/rk3026-board-spl.c, kept because that binary is
 * proven to boot and is not worth disturbing for tidiness.
 */
#ifndef __BOOX_PROBE_H
#define __BOOX_PROBE_H

#ifdef UBOOT_CURRENT_STEP

/*
 * Two spin lengths, because the same loop differs by orders of magnitude
 * between contexts: pre-relocation with caches off and the PLLs possibly
 * bypassed to 24 MHz, versus post-relocation with caches on.
 *
 * boox_step_blink()	uncached/early sites
 * boox_step_blink_r()	cached sites, i.e. anything from board_init_r onward
 */
void boox_step_blink(void);
void boox_step_blink_r(void);
/* Set the LED to a known level and RETURN -- bracket a risky read with this, so
 * "unchanged forever" distinguishes a wedged access from never getting here. */
void boox_probe_led(int on);

/*
 * Report a register read. match = long on, brief dark. mismatch = long dark,
 * brief on. Same period either way, so only the ratio means anything.
 */
void boox_step_blink_match(void);
void boox_step_blink_mismatch(void);

/* Caller scope for probes in shared code (uclass, device core, clk core): such
 * a probe blinks for whichever consumer arrives first unless it is scoped, and
 * an unscoped reading proves nothing about the device under investigation. The
 * caller sets this around the call it cares about. */
extern int boox_scope;

#define BOOX_STEP_AT(n)			do { if ((n) == UBOOT_CURRENT_STEP) boox_step_blink(); } while (0)
#define BOOX_STEP_AT_R(n)		do { if ((n) == UBOOT_CURRENT_STEP) boox_step_blink_r(); } while (0)
#define BOOX_STEP_AT_IF(n, cond)	do { if ((n) == UBOOT_CURRENT_STEP && (cond)) boox_step_blink(); } while (0)
#define BOOX_STEP_AT_R_IF(n, cond)	do { if ((n) == UBOOT_CURRENT_STEP && (cond)) boox_step_blink_r(); } while (0)
/* Read a value out through the LED: BOOX_REPORT(n, reg == expected). */
#define BOOX_REPORT(n, cond)						\
	do {								\
		if ((n) == UBOOT_CURRENT_STEP) {			\
			if (cond)					\
				boox_step_blink_match();		\
			else						\
				boox_step_blink_mismatch();		\
		}							\
	} while (0)

#else

#define BOOX_STEP_AT(n)			do { } while (0)
#define BOOX_STEP_AT_R(n)		do { } while (0)
#define BOOX_STEP_AT_IF(n, cond)	do { } while (0)
#define BOOX_STEP_AT_R_IF(n, cond)	do { } while (0)
#define BOOX_REPORT(n, cond)		do { } while (0)

#endif

#endif
