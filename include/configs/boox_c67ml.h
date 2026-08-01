/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * ONYX BOOX C67ML Magellan board config
 *
 * Copyright (c) 2024, Mario Rugiero
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#include <configs/rk3026_common.h>

/*
 * SPL timebase.
 *
 * RETRACTED 2026-07-29: this said "the SPL is deliberately DM-free, so
 * CONFIG_SPL_TIMER (which needs SPL_DM) is unavailable". Both halves are false.
 * The built .config has CONFIG_SPL_DM=y despite the defconfig negating it, so
 * SPL_TIMER's deps are satisfied and it links at +368 B of a 6144 B window
 * (measured). The SPL is DM-inert, not DM-free. What actually blocks a working
 * SPL DM timer is SPL_OF_CONTROL, which fails to link on device.c's calloc.
 *
 * lib/time.c takes its pre-driver-model path here. Giving it these
 * three values is all that is required: it then supplies timer_read_counter(),
 * get_ticks() with 32->64-bit rollover, and a __udelay() whose usec_to_tick()
 * is 64-bit and whose "tmp + 1" guarantees at least one tick for sub-tick
 * delays. Hand-rolling __udelay() instead, as an earlier revision did, got both
 * of those wrong.
 *
 * The rate is MEASURED, not assumed: timer1's free-running 64-bit counter,
 * sampled 30 s apart on a live device, gives 24,000,665 Hz. The vendor
 * MiniLoader agrees -- it converts ticks with count*5/120 = count/24.
 *
 * The earlier "375 kHz = 24 MHz / 64" here came from pointing at rk3036's
 * timer5 (0x200440a0), which does not exist on RK3026: that window reads all
 * zeros. Timers are 0x20 apart, and only timer0 (+0x00) and timer1 (+0x20)
 * are implemented.
 */
/*
 * SPL only. U-Boot proper has CONFIG_TIMER and the Rockchip DM timer driver, and
 * defining CFG_SYS_TIMER_RATE there would collide with the driver-model
 * get_tbclk() in lib/time.c.
 */
/*
 * SPL timebase: the A9 MPCore global timer, through lib/time.c's
 * pre-driver-model path. Enabled in rockchip_stimer_init() before any delay.
 *
 * Counts UP and is 64-bit, so CONFIG_SYS_TIMER_COUNTS_DOWN must stay OFF -- the
 * counter this replaces (timer0, via the SPL's own __udelay) counted down.
 *
 * The rate is MEASURED in the SPL itself, against timer0's fixed 24 MHz xin24m
 * reference with no human timing: 24 * 505051 / 2020238 = 5.99991 MHz. It is
 * CORE_PERI = APLL / (CLKSEL_CON1[3:0] + 1), and at BROM defaults that is
 * 24 MHz / 4.
 *
 * ⚠️ THIS CONSTANT IS ONLY VALID WHILE THE SPL LEAVES THE PLLS ALONE. Raising
 * APLL in the SPL -- the planned change -- moves CORE_PERI with it and makes
 * every delay here wrong by that ratio. Re-measure and re-derive before doing
 * it, or give the SPL a rate it reads at runtime.
 *
 * Routing the SPL through this path is what stopped it booting on 2026-07-27 --
 * but that pointed CFG_SYS_TIMER_COUNTER at 0x200440a0, a counter that never
 * ticks. The address below is a block that demonstrably counts.
 */
#ifdef CONFIG_XPL_BUILD
#define CFG_SYS_TIMER_COUNTER	0x1013c200
#define CFG_SYS_TIMER_RATE	6000000
#endif	/* SPL only: U-Boot proper has CONFIG_TIMER, and lib/time.c defines
	 * get_tbclk() twice if both paths are configured. */

#endif
