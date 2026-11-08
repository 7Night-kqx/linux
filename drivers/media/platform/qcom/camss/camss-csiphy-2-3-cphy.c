// SPDX-License-Identifier: GPL-2.0-only
/* Experimental dada PHY3 C-PHY receiver; no unverified parameter defaults. */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include "camss.h"
#include "camss-csiphy-2-3-cphy-regs.h"

static bool cphy_experimental;
module_param(cphy_experimental, bool, 0444);
MODULE_PARM_DESC(cphy_experimental, "Enable experimental dada PHY3 C-PHY");
static unsigned long cphy_rate;
module_param(cphy_rate, ulong, 0444);
MODULE_PARM_DESC(cphy_rate, "Confirmed bit rate per trio, not symbol rate");
static int cphy_settle_count = -1;
module_param(cphy_settle_count, int, 0444);
MODULE_PARM_DESC(cphy_settle_count,
		 "Confirmed raw settle counter, not nanoseconds");

int csiphy_230_cphy_validate(struct csiphy_device *csiphy)
{
	if (!cphy_experimental || cphy_settle_count < 0 || !cphy_rate)
		return -EOPNOTSUPP;
	if (csiphy->camss->res->version != CAMSS_8750 || csiphy->id != 3 ||
	    !of_machine_is_compatible("xiaomi,dada") ||
	    csiphy->cfg.csi2->lane_cfg.num_data != 3 ||
	    cphy_settle_count > 0xffff || cphy_rate > 9120000000ULL)
		return -EINVAL;
	return 0;
}

static void cphy_write(struct csiphy_device *csiphy,
		       const struct cphy_reg *regs, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		u32 value = regs[i].value;

		switch (regs[i].flags) {
		case BIT(0):
		case BIT(11):
			break;
		case BIT(2):
			value = cphy_settle_count & 0xff;
			break;
		case BIT(3):
			value = (cphy_settle_count >> 8) & 0xff;
			break;
		default:
			continue;
		}
		writel(value, csiphy->base + regs[i].address);
		if (regs[i].delay_us)
			usleep_range(regs[i].delay_us, regs[i].delay_us + 5);
	}
}

int csiphy_230_cphy_enable(struct csiphy_device *csiphy, u8 lane_mask)
{
	const struct cphy_reg *rate_regs;
	int ret = csiphy_230_cphy_validate(csiphy);

	if (ret)
		return ret;
	if (lane_mask != 0x2a)
		return -EINVAL;
	/* Stock o3 first-choice table; automatic EQ alternatives remain untested. */
	if (cphy_rate <= 7524000000ULL)
		rate_regs = datarate_230_3p3Gsps;
	else if (cphy_rate <= 7980000000ULL)
		rate_regs = datarate_230_3p5Gsps;
	else
		rate_regs = datarate_230_4p0Gsps;

	writel(1, csiphy->base + 0x1000);
	usleep_range(1, 6);
	writel(lane_mask, csiphy->base + 0x1014);
	cphy_write(csiphy, csiphy_common_reg_2_3_0,
		   ARRAY_SIZE(csiphy_common_reg_2_3_0));
	cphy_write(csiphy, rate_regs, ARRAY_SIZE(datarate_230_3p3Gsps));
	cphy_write(csiphy, csiphy_3ph_v2_3_0_reg,
		   ARRAY_SIZE(csiphy_3ph_v2_3_0_reg));
	cphy_write(csiphy, csiphy_irq_reg_2_3_0,
		   ARRAY_SIZE(csiphy_irq_reg_2_3_0));
	/* Release only after lane, data-rate and IRQ programming. */
	writel(0x0e, csiphy->base + 0x1000);
	usleep_range(3048, 3053);
	return 0;
}
