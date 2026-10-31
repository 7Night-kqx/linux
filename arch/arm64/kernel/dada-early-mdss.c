// SPDX-License-Identifier: GPL-2.0
/*
 * dada early MDSS register snapshot (round BB).
 *
 * ABL hands over a fully-programmed, correctly-decoding display
 * pipeline (the Mu-Silicium SM8750 simpledrm console proves the frame
 * ABL scanned out is displayed fine). But every dump so far read the
 * hardware state only AFTER it had been rewritten or wedged: AV/AW/AY
 * read our own driver's values, and round BA (mdss disabled) read the
 * wedged state -- the whole register file returned the bus-dead value
 * 0x040e8000 because nothing preserved the ABL splash mapping through
 * the arm-smmu handover.
 *
 * This arch_initcall runs BEFORE the arm-smmu and msm_dpu probes
 * (device_initcall level), while the ABL configuration is still live
 * in the registers, and prints it with the dada-early-mdss: prefix.
 * Together with the post-boot live rd console (which reads the same
 * addresses after our driver has programmed them) one boot yields the
 * vendor ground truth AND our values for a direct diff.
 *
 * Gated on the dada-early-mdss bootarg so only the BB probe tree
 * dumps; the token rides in the DT chosen bootargs.
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/printk.h>
#include <linux/string.h>

struct dada_dump_reg {
	phys_addr_t addr;
	const char *name;
};

static const struct dada_dump_reg dada_bb_regs[] = {
	/* DSI0 ctrl @ 0x0ae94000, RAW offsets (xml + DSI_6G_REG_SHIFT 4) */
	{ 0x0ae94000, "HW_VERSION" },
	{ 0x0ae94004, "DSI_CTRL" },
	{ 0x0ae94008, "STATUS0" },
	{ 0x0ae9400c, "FIFO_STATUS" },
	{ 0x0ae94040, "CMD_CFG0" },
	{ 0x0ae94044, "CMD_CFG1" },
	{ 0x0ae94058, "STREAM0_CTRL" },
	{ 0x0ae9405c, "STREAM0_TOTAL" },
	{ 0x0ae94060, "STREAM1_CTRL" },
	{ 0x0ae94064, "STREAM1_TOTAL" },
	{ 0x0ae940a8, "LANE_STATUS" },
	{ 0x0ae940ac, "LANE_CTRL" },
	{ 0x0ae940b4, "LP_TIMER_CTRL" },
	{ 0x0ae940b8, "HS_TIMER_CTRL" },
	{ 0x0ae940cc, "EOT_PACKET_CTRL" },
	{ 0x0ae941b8, "CMD_MODE_MDP_CTRL2" },
	{ 0x0ae942a8, "CMD_COMPRESSION_CTRL" },
	{ 0x0ae942ac, "CMD_COMPRESSION_CTRL2" },
	/* DSC encoder/ctrl pair dce_0 */
	{ 0x0ae81100, "DCE0_ENC_DF_CTRL" },
	{ 0x0ae81130, "DCE0_DSC_MAIN_CONF" },
	{ 0x0ae81200, "DCE0_CTRL_DF_CTRL" },
	{ 0x0ae81230, "DCE0_CTRL_MAIN_CONF" },
	/* DSC dce_1 (idle contrast) */
	{ 0x0ae82100, "DCE1_ENC_DF_CTRL" },
	{ 0x0ae82130, "DCE1_ENC_MAIN_CONF" },
	{ 0x0ae82200, "DCE1_CTRL_DF_CTRL" },
	{ 0x0ae82230, "DCE1_CTRL_MAIN_CONF" },
	/* INTF_0 (base 0x0ae35000) */
	{ 0x0ae35000, "INTF_0_000" },
	{ 0x0ae35004, "INTF_0_004" },
	{ 0x0ae35008, "INTF_0_008" },
	{ 0x0ae35060, "INTF_CONFIG2" },
	/* CTL_0 (base 0x0ae16000) */
	{ 0x0ae16000, "CTL_0_000" },
	{ 0x0ae16004, "CTL_0_004" },
	{ 0x0ae16008, "CTL_0_008" },
	{ 0x0ae1600c, "CTL_0_00c" },
	{ 0x0ae16014, "CTL_0_014" },
	/* pingpong 0/1 */
	{ 0x0ae6a000, "PP0_000" },
	{ 0x0ae6a004, "PP0_004" },
	{ 0x0ae6a008, "PP0_008" },
	{ 0x0ae6a00c, "PP0_00c" },
	{ 0x0ae6b000, "PP1_000" },
	{ 0x0ae6b004, "PP1_004" },
	/* DSI0 PHY @ 0x0ae95000, first 16 words */
	{ 0x0ae95000, "PHY_00" },  { 0x0ae95004, "PHY_04" },
	{ 0x0ae95008, "PHY_08" },  { 0x0ae9500c, "PHY_0c" },
	{ 0x0ae95010, "PHY_10" },  { 0x0ae95014, "PHY_14" },
	{ 0x0ae95018, "PHY_18" },  { 0x0ae9501c, "PHY_1c" },
	{ 0x0ae95020, "PHY_20" },  { 0x0ae95024, "PHY_24" },
	{ 0x0ae95028, "PHY_28" },  { 0x0ae9502c, "PHY_2c" },
	{ 0x0ae95030, "PHY_30" },  { 0x0ae95034, "PHY_34" },
	{ 0x0ae95038, "PHY_38" },  { 0x0ae9503c, "PHY_3c" },
};

static int __init dada_early_mdss_dump(void)
{
	int i;

	if (!strstr(saved_command_line, "dada-early-mdss"))
		return 0;

	pr_info("dada-early-mdss: dump begin (round BB, pre-SMMU ABL state)\n");
	for (i = 0; i < ARRAY_SIZE(dada_bb_regs); i++) {
		void __iomem *base = ioremap(dada_bb_regs[i].addr & PAGE_MASK,
					     PAGE_SIZE);
		u32 val;

		if (!base) {
			pr_info("dada-early-mdss: 0x%08llX %s ioremap FAILED\n",
				dada_bb_regs[i].addr, dada_bb_regs[i].name);
			continue;
		}
		val = readl(base + (dada_bb_regs[i].addr & ~PAGE_MASK));
		pr_info("dada-early-mdss: 0x%08llX %s = 0x%08x\n",
			dada_bb_regs[i].addr, dada_bb_regs[i].name, val);
		iounmap(base);
	}
	pr_info("dada-early-mdss: dump end (%d registers)\n", i);
	return 0;
}
arch_initcall(dada_early_mdss_dump);
