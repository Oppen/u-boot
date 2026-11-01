/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * ONYX BOOX Robinson Crusoe 2: register addresses and the power hold,
 * shared by the SPL and U-Boot proper.
 */

#ifndef __BOOX_RC2_H
#define __BOOX_RC2_H

#include <asm/io.h>
#include <asm/arch-rockchip/hardware.h>
#include <linux/bitops.h>

#define CRU_BASE		0x20000000
#define CRU_CLKGATE_CON8	(CRU_BASE + 0xf0)

#define GRF_BASE		0x20008000
#define GRF_GPIO1A_IOMUX	(GRF_BASE + 0xb8)

#define GPIO1_BASE		0x20080000
#define GPIO_SWPORTA_DR		0x00
#define GPIO_SWPORTA_DDR	0x04
#define RC2_POWER_HOLD		BIT(2)		/* GPIO1_A2 */

/*
 * Power hold high, or the PMIC drops the rails when the power key is
 * released: pclk_gpio1 on, pin to GPIO, data high, output.
 */
static inline void rc2_power_hold(void)
{
	writel(RK_CLRBITS(BIT(10)), (void __iomem *)CRU_CLKGATE_CON8);
	writel(RK_CLRBITS(GENMASK(5, 4)), (void __iomem *)GRF_GPIO1A_IOMUX);
	setbits_le32((void __iomem *)(GPIO1_BASE + GPIO_SWPORTA_DR),
		     RC2_POWER_HOLD);
	setbits_le32((void __iomem *)(GPIO1_BASE + GPIO_SWPORTA_DDR),
		     RC2_POWER_HOLD);
}

#endif
