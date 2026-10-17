// SPDX-License-Identifier: GPL-2.0+
/*
 * ONYX BOOX Robinson Crusoe 2: SPL board code. No storage, no PMIC
 * access: the power hold and the DDR die geometry, nothing else.
 */

#include <asm/arch-rockchip/sdram_rk3026.h>
#include "boox-rc2.h"

void rk3026_spl_board_init_f(void)
{
	rc2_power_hold();
}

void get_ddr_config(struct rk3026_ddr_config *config)
{
	/* One rank of DDR3, x16, 8 banks, 15 row and 10 column bits: 512 MiB */
	config->ddr_type = 3;		/* DDR3 */
	config->rank = 1;
	config->cs0_row = 15;
	config->cs1_row = 15;
	config->bank = 3;		/* 8 banks */
	config->col = 10;
	config->bw = 1;			/* 16 bit */
}
