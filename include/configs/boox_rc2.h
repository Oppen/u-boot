/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * ONYX BOOX Robinson Crusoe 2 board config
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#include <configs/rk3026_common.h>

/*
 * SPL timebase: the A9 global timer through lib/time.c's pre-driver-model
 * path, at the mask ROM's clocks, as on the C67ML (boox_c67ml.h).
 */
#ifdef CONFIG_XPL_BUILD
#define CFG_SYS_TIMER_COUNTER	0x1013c200
#define CFG_SYS_TIMER_RATE	6000000
#endif

#endif
