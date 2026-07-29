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
 * No CFG_SYS_TIMER_* here. The SPL defines its own __udelay() in
 * rk3026-board-spl.c; routing it through lib/time.c's pre-driver-model path
 * instead is what stopped the SPL booting on 2026-07-27 (it had reached "DRAM
 * trained" and handed off to U-Boot proper immediately before that change).
 */

#endif
