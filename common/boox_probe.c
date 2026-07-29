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
/*
 * 200,000, not 5,000,000. A marker can fire while the PLLs are in slow mode --
 * rkclk_set_pll() bypasses them to 24 MHz while reconfiguring -- and at the
 * ~54k iterations/second measured in that state, 5M spins is ~90 s per half
 * cycle, which reads as a SOLID LED. Any "solid" result from a card built with
 * the larger count is suspect for that reason. At 200k the same worst case is a
 * few seconds, while with caches on it is still slow enough to see.
 */
#define SPIN		200000

/*
 * Post-relocation sites run with caches on, where SPIN above is nearly
 * invisible -- common/board_r.c hit exactly that and kept a 5,000,000 spin of
 * its own. Same probe, slower, so cached code can use the shared function
 * instead of copying it.
 */
#define SPIN_CACHED	5000000

int boox_scope;

static noinline void boox_blink(u32 spin)
{
	volatile u32 i;

	writel(BIT(GATE_PCLK_GPIO2 + 16), (void __iomem *)CRU_GATE_CON8);
	setbits_le32((void __iomem *)(GPIO2_BASE + GPIO_DDR), LED_BIT);

	while (1) {
		setbits_le32((void __iomem *)(GPIO2_BASE + GPIO_DR), LED_BIT);
		for (i = 0; i < spin; i++)
			;
		clrbits_le32((void __iomem *)(GPIO2_BASE + GPIO_DR), LED_BIT);
		for (i = 0; i < spin; i++)
			;
	}
}

noinline void boox_step_blink(void)
{
	boox_blink(SPIN);
}

noinline void boox_step_blink_r(void)
{
	boox_blink(SPIN_CACHED);
}
