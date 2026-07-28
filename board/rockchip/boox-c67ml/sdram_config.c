// SPDX-License-Identifier: GPL-2.0+
/*
 * ONYX BOOX C67ML Magellan DDR die configuration (used by the SPL's
 * sdram_init(); kept in its own object so the SPL does not pull in the
 * env/fdtdec code from boox-c67ml.c).
 *
 * Copyright (c) 2024, Mario Rugiero
 */

#include <asm/arch-rockchip/sdram_rk3026.h>

void get_ddr_config(struct rk3026_ddr_config *config)
{
	/*
	 * Samsung K4B4G1646Q-HYK0 (part marking read off the board): DDR3,
	 * 4 Gbit, x16 organisation = 256M x 16 -> 8 banks, 15 row bits,
	 * 10 col bits, one rank.
	 *   1 << (row + col + bank + 1) = 1 << 29 = 512 MB.
	 *
	 * Same K4B4G1646 family the in-tree rk3036 driver's 396 MHz timing
	 * table was written against (see board/rockchip/evb_rk3036).
	 */
	config->ddr_type = 3;		/* DDR3 */
	config->rank = 1;
	config->cs0_row = 15;
	config->cs1_row = 15;
	config->bank = 3;		/* 8 banks */
	config->col = 10;
	config->bw = 1;			/* 16 bit */
}
