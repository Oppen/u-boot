// SPDX-License-Identifier: GPL-2.0+
/*
 * BOOX bring-up probe: blink the blue LED forever off a raw spin.
 *
 * No timer, no udelay(), no console. Deliberately noinline and non-static so
 * that a built image can be checked for the marker before it is flashed.
 */
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/compiler.h>
#include <boox_probe.h>

#define GPIO2_BASE	0x20084000
#define GPIO_DR		0x00
#define GPIO_DDR	0x04
#define LED_BIT		BIT(15)
#define CRU_GATE_CON8	0x200000f0
#define GATE_PCLK_GPIO2	11
#define SPIN		5000000

int boox_scope;

noinline void boox_step_blink(void)
{
	volatile u32 i;

	writel(BIT(GATE_PCLK_GPIO2 + 16), (void __iomem *)CRU_GATE_CON8);
	setbits_le32((void __iomem *)(GPIO2_BASE + GPIO_DDR), LED_BIT);

	while (1) {
		setbits_le32((void __iomem *)(GPIO2_BASE + GPIO_DR), LED_BIT);
		for (i = 0; i < SPIN; i++)
			;
		clrbits_le32((void __iomem *)(GPIO2_BASE + GPIO_DR), LED_BIT);
		for (i = 0; i < SPIN; i++)
			;
	}
}
