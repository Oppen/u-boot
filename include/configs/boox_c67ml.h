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
 * This SPL is deliberately DM-free, so CONFIG_SPL_TIMER (which needs SPL_DM) is
 * unavailable and lib/time.c takes its pre-driver-model path. Giving it these
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
#ifdef CONFIG_XPL_BUILD
#define CFG_SYS_TIMER_COUNTER	(0x20044000 + 0x08)	/* timer0 curr_value0 */
#define CFG_SYS_TIMER_RATE	24000000		/* measured 24,000,665 Hz */
#endif

#endif
