// SPDX-License-Identifier: GPL-2.0
/*
 * camss-csid-980.c
 *
 * Qualcomm MSM Camera Subsystem - CSID (CSI Decoder) Module
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include "camss.h"
#include "camss-csid.h"
#include "camss-csid-gen3.h"

static bool rdi_format_measure;
module_param(rdi_format_measure, bool, 0444);
MODULE_PARM_DESC(rdi_format_measure, "Enable full980 HBI and VBI measurements");

static bool rdi_packet_sampling;
module_param(rdi_packet_sampling, bool, 0444);
MODULE_PARM_DESC(rdi_packet_sampling, "Rearm full980 packet captures for 16 bounded samples");

static bool tele_full_vc;
module_param(tele_full_vc, bool, 0444);
MODULE_PARM_DESC(tele_full_vc, "Use full-width VC for SM8750 PHY1 diagnostics");

static bool main_epd;
module_param(main_epd, bool, 0444);
MODULE_PARM_DESC(main_epd, "Enable stock EPD reception for dada PHY3 diagnostics");

/* Reset and Command Registers */
#define CSID_RST_CFG				0xC
#define		RST_MODE				BIT(0)
#define		RST_LOCATION				BIT(4)

/* Reset and Command Registers */
#define CSID_RST_CMD				0x10
#define		SELECT_HW_RST				BIT(0)
#define		SELECT_IRQ_RST				BIT(2)
#define CSID_IRQ_CMD				0x14
#define		IRQ_CMD_CLEAR				BIT(0)

/* Register Update Commands, RUP/AUP */
#define CSID_RUP_CMD				0x18
#define CSID_AUP_CMD				0x1C
#define		CSID_RUP_AUP_RDI(rdi)			(BIT(8) << (rdi))
#define CSID_RUP_AUP_CMD			0x20
#define		RUP_SET					BIT(0)

#define CSID_LITE_RUP_AUP_CMD			0x18
#define		CSID_LITE_RUP_RDI(rdi)		(BIT(4) << (rdi))
#define		CSID_LITE_AUP_RDI(rdi)		(BIT(20) << (rdi))

/* Top level interrupt registers */
#define CSID_TOP_IRQ_STATUS			(csid_is_lite(csid) ? 0x7C : 0x84)
#define CSID_TOP_IRQ_MASK			(csid_is_lite(csid) ? 0x80 : 0x88)
#define CSID_TOP_IRQ_CLEAR			(csid_is_lite(csid) ? 0x84 : 0x8C)
#define		INFO_RST_DONE				BIT(0)
#define		BUF_DONE_IRQ_STATUS			BIT(csid_is_lite(csid) ? 13 : 3)

/* Buffer done interrupt registers */
#define CSID_BUF_DONE_IRQ_STATUS		(csid_is_lite(csid) ? 0x8C : 0xA4)
#define		BUF_DONE_IRQ_STATUS_RDI_OFFSET		(csid_is_lite(csid) ? 1 : 16)
#define CSID_BUF_DONE_IRQ_MASK			(csid_is_lite(csid) ? 0x90 : 0xA8)
#define CSID_BUF_DONE_IRQ_CLEAR			(csid_is_lite(csid) ? 0x94 : 0xAC)

/* CSI2 RX Configuration */
#define CSID_CSI2_RX_CFG0			(csid_is_lite(csid) ? 0x200 : 0x400)
#define		CSI2_RX_CFG0_NUM_ACTIVE_LANES		0
#define		CSI2_RX_CFG0_PHY_SEL_BASE_IDX		1
#define		CSI2_RX_CFG0_DL0_INPUT_SEL		4
#define		CSI2_RX_CFG0_PHY_NUM_SEL		20
#define CSID_CSI2_RX_CFG1			(csid_is_lite(csid) ? 0x204 : 0x404)
#define		CSI2_RX_CFG1_ECC_CORRECTION_EN		BIT(0)
#define		CSI2_RX_CFG1_VC_MODE			BIT(2)
#define		CSI2_RX_CFG1_EPD_MODE_EN			BIT(8)

#define MSM_CSID_MAX_SRC_STREAMS_980		(csid_is_lite(csid) ? 4 : 5)

#define CSID_RDI_CFG0(rdi)                                  \
	({                                                  \
		__typeof__(rdi) _rdi = (rdi);               \
		csid_is_lite(csid) ? 0x500 + 0x100 * _rdi : \
				     0xE00 + 0x200 * _rdi;  \
	})
#define		RDI_CFG0_RETIME_BS			BIT(5)
#define		RDI_CFG0_TIMESTAMP_EN			BIT(6)
#define		RDI_CFG0_TIMESTAMP_STB_SEL		BIT(8)
#define		RDI_CFG0_DECODE_FORMAT			12
#define		RDI_CFG0_DT				16
#define		RDI_CFG0_VC				22
#define		RDI_CFG0_DT_ID				27
#define		RDI_CFG0_EN				BIT(31)

/* RDI Control and Configuration */
#define CSID_RDI_CTRL(rdi)                                  \
	({                                                  \
		__typeof__(rdi) _rdi = (rdi);               \
		csid_is_lite(csid) ? 0x504 + 0x100 * _rdi : \
				     0xE04 + 0x200 * _rdi;  \
	})
#define		RDI_CTRL_START_CMD			BIT(0)

#define CSID_RDI_CFG1(rdi)                                  \
	({                                                  \
		__typeof__(rdi) _rdi = (rdi);               \
		csid_is_lite(csid) ? 0x510 + 0x100 * _rdi : \
				     0xE10 + 0x200 * _rdi;  \
	})
#define		RDI_CFG1_PACKING_FORMAT_MIPI		BIT(15)

/* RDI Pixel Store Configuration */
#define CSID_RDI_PIX_STORE_CFG0(rdi)		(0xE14 + 0x200 * (rdi))
#define		RDI_PIX_STORE_CFG0_EN			BIT(0)
#define		RDI_PIX_STORE_CFG0_MIN_HBI		1

/* Full980 measurement enable and HBI/VBI counters; size checking stays off. */
#define CSID_RDI_FORMAT_MEASURE_CFG0(rdi)		(0xE88 + 0x200 * (rdi))
#define		RDI_FORMAT_MEASURE_EN			BIT(4)
#define		RDI_FORMAT_MEASURE_HBI_VBI		(BIT(3) | BIT(2))

/* RDI IRQ Status in wrapper */
#define CSID_CSI2_RDIN_IRQ_STATUS(rdi) \
	((csid_is_lite(csid) ? 0xEC : 0x114) + 0x10 * (rdi))
#define CSID_CSI2_RDIN_IRQ_MASK(rdi) \
	((csid_is_lite(csid) ? 0xF0 : 0x118) + 0x10 * (rdi))
#define CSID_CSI2_RDIN_IRQ_CLEAR(rdi) \
	((csid_is_lite(csid) ? 0xF4 : 0x11C) + 0x10 * (rdi))
#define		INFO_RUP_DONE				BIT(23)
#define		INFO_CAMIF_SOF				BIT(4)
#define CSID_RDI_TOP_IRQ(rdi)		BIT((csid_is_lite(csid) ? 8 : 16) + (rdi))
#define CSID_RDI_IRQ_PATTERN(rdi) \
	((csid_is_lite(csid) ? 0x548 : 0xE50) + (csid_is_lite(csid) ? 0x100 : 0x200) * (rdi))
#define CSID_RDI_IRQ_PERIOD(rdi)		(CSID_RDI_IRQ_PATTERN(rdi) + 4)
#define CSID_RDI_SOF_TS(rdi) \
	((csid_is_lite(csid) ? 0x594 : 0xEA0) + (csid_is_lite(csid) ? 0x100 : 0x200) * (rdi))
#define CSID_RDI_EOF_TS(rdi)		(CSID_RDI_SOF_TS(rdi) + 0x10)

static void __csid_full_aup_rup_trigger(struct csid_device *csid)
{
	/* trigger SET in combined register */
	writel(RUP_SET, csid->base + CSID_RUP_AUP_CMD);
}

static void __csid_aup_update(struct csid_device *csid, int port_id)
{
	if (csid_is_lite(csid)) {
		/* CSID Lites in v980 follow the legacy way of a combined RUP
		 * and AUP commands without an explicit SET register.
		 */
		csid->reg_update |= CSID_LITE_AUP_RDI(port_id);
		writel(csid->reg_update, csid->base + CSID_LITE_RUP_AUP_CMD);
	} else {
		csid->aup_update |= CSID_RUP_AUP_RDI(port_id);
		writel(csid->aup_update, csid->base + CSID_AUP_CMD);

		/* CSID Fulls in v980 split AUP and RUP commands, which requires
		 * additional SET operation to make registers modification take
		 * effect.
		 */
		__csid_full_aup_rup_trigger(csid);
	}
}

static void __csid_rup_update(struct csid_device *csid, int port_id)
{
	if (csid_is_lite(csid)) {
		/* CSID Lites in v980 follow the legacy way of a combined RUP
		 * and AUP commands without an explicit SET register.
		 */
		csid->reg_update |= CSID_LITE_RUP_RDI(port_id);
		writel(csid->reg_update, csid->base + CSID_LITE_RUP_AUP_CMD);
	} else {
		csid->rup_update |= CSID_RUP_AUP_RDI(port_id);
		writel(csid->rup_update, csid->base + CSID_RUP_CMD);

		/* CSID Fulls in v980 split AUP and RUP commands, which requires
		 * additional SET operation to make registers modification take
		 * effect.
		 */
		__csid_full_aup_rup_trigger(csid);
	}
}

static void __csid_aup_rup_clear(struct csid_device *csid, int port_id)
{
	/* Hardware clears the registers upon consuming the settings */
	if (csid_is_lite(csid)) {
		csid->reg_update &= ~CSID_LITE_RUP_RDI(port_id);
		csid->reg_update &= ~CSID_LITE_AUP_RDI(port_id);
	} else {
		csid->aup_update &= ~CSID_RUP_AUP_RDI(port_id);
		csid->rup_update &= ~CSID_RUP_AUP_RDI(port_id);
	}
}

static void __csid_configure_rx(struct csid_device *csid,
				struct csid_phy_config *phy)
{
	int val;

	val = (phy->lane_cnt - 1) << CSI2_RX_CFG0_NUM_ACTIVE_LANES;
	val |= phy->lane_assign << CSI2_RX_CFG0_DL0_INPUT_SEL;
	/* CSID980 RX_CFG0 PHY_TYPE, stock dada-v-oss bit 24. */
	if (phy->is_cphy)
		val |= BIT(24);
	val |= (phy->csiphy_id + CSI2_RX_CFG0_PHY_SEL_BASE_IDX)
	       << CSI2_RX_CFG0_PHY_NUM_SEL;
	writel(val, csid->base + CSID_CSI2_RX_CFG0);

	val = CSI2_RX_CFG1_ECC_CORRECTION_EN;
	if (camera_diagnostics && tele_full_vc && !csid_is_lite(csid) &&
	    csid->camss->res->version == CAMSS_8750 &&
	    !phy->is_cphy && phy->csiphy_id == 1)
		val |= CSI2_RX_CFG1_VC_MODE;
	if (camera_diagnostics && main_epd && !csid_is_lite(csid) &&
	    csid->camss->res->version == CAMSS_8750 &&
	    phy->is_cphy && phy->csiphy_id == 3)
		val |= CSI2_RX_CFG1_EPD_MODE_EN;
	writel(val, csid->base + CSID_CSI2_RX_CFG1);
	/* Stock full980 capture control follows the selected image VC. */
	if (camera_diagnostics && !csid_is_lite(csid)) {
		val = BIT(0) | BIT(1) | (0x2b << 4) |
		      (phy->image_vc << 10) | (phy->image_vc << 15);
		if (phy->is_cphy)
			val |= BIT(2) | (0x2b << 20) | (phy->image_vc << 26);
		writel(val, csid->base + 0x408);
	}
}

static void __csid_configure_rx_vc(struct csid_device *csid, int vc)
{
	int val;

	if (vc > 3) {
		val = readl(csid->base + CSID_CSI2_RX_CFG1);
		val |= CSI2_RX_CFG1_VC_MODE;
		writel(val, csid->base + CSID_CSI2_RX_CFG1);
	}
}

static void __csid_ctrl_rdi(struct csid_device *csid, int enable, u8 rdi)
{
	int val = 0;
	u32 rdi_ctrl_offset = CSID_RDI_CTRL(rdi);

	if (enable)
		val = RDI_CTRL_START_CMD;

	writel(val, csid->base + rdi_ctrl_offset);
}

static void __csid_configure_rdi_pix_store(struct csid_device *csid, u8 rdi)
{
	u32 val;

	/*
	 * Configure pixel store to allow absorption of hblanking or idle time.
	 * This helps with horizontal crop and prevents line buffer conflicts.
	 * Reset state is 0x8 which has MIN_HBI=4, we keep the default MIN_HBI
	 * and just enable the pixel store functionality.
	 */
	val = (4 << RDI_PIX_STORE_CFG0_MIN_HBI) | RDI_PIX_STORE_CFG0_EN;
	writel(val, csid->base + CSID_RDI_PIX_STORE_CFG0(rdi));
}

static void __csid_configure_rdi_stream(struct csid_device *csid, u8 enable, u8 port, u8 vc)
{
	u32 val;

	/* Source pads matching RDI channels on hardware.
	 * E.g. Pad 1 -> RDI0, Pad 2 -> RDI1, etc.
	 */
	struct v4l2_mbus_framefmt *input_format = &csid->fmt[MSM_CSID_PAD_FIRST_SRC + port];
	const struct csid_format_info *format = csid_get_fmt_entry(csid->res->formats->formats,
								   csid->res->formats->nformats,
								   input_format->code);

	/*
	 * DT_ID is a two bit bitfield that is concatenated with
	 * the four least significant bits of the five bit VC
	 * bitfield to generate an internal CID value.
	 *
	 * CSID_RDI_CFG0(vc)
	 * DT_ID : 28:27
	 * VC    : 26:22
	 * DT    : 21:16
	 *
	 * CID   : VC 3:0 << 2 | DT_ID 1:0
	 */
	u8 dt_id = port & 0x03;
	u32 rdi_cfg0_offset = CSID_RDI_CFG0(port);
	u32 rdi_cfg1_offset = CSID_RDI_CFG1(port);
	u32 rdi_ctrl_offset = CSID_RDI_CTRL(port);

	val = RDI_CFG0_TIMESTAMP_EN;
	val |= RDI_CFG0_TIMESTAMP_STB_SEL;
	val |= RDI_CFG0_RETIME_BS;

	/* SM8750 decodes pixels before its MIPI packing stage. */
	if (csid->camss->res->version == CAMSS_8750)
		val |= format->decode_format << RDI_CFG0_DECODE_FORMAT;
	else
		val |= DECODE_FORMAT_PAYLOAD_ONLY << RDI_CFG0_DECODE_FORMAT;
	val |= vc << RDI_CFG0_VC;
	val |= format->data_type << RDI_CFG0_DT;
	val |= dt_id << RDI_CFG0_DT_ID;
	writel(val, csid->base + rdi_cfg0_offset);

	val = RDI_CFG1_PACKING_FORMAT_MIPI;
	writel(val, csid->base + rdi_cfg1_offset);
	if (camera_diagnostics)
		dev_info(csid->camss->dev,
			 "rdi-write: csid%u rdi%u enable%u version%u hw%08x cfg1@%04x requested%08x read%08x measure%u lite%u\n",
			 csid->id, port, enable, csid->camss->res->version,
			 readl(csid->base), rdi_cfg1_offset, val,
			 readl(csid->base + rdi_cfg1_offset), rdi_format_measure,
			 csid_is_lite(csid));
	if (rdi_format_measure && !csid_is_lite(csid)) {
		u32 offset = CSID_RDI_FORMAT_MEASURE_CFG0(port);

		/* Full980 has a dedicated measurement enable, outside CFG1. */
		val = enable ? RDI_FORMAT_MEASURE_EN | RDI_FORMAT_MEASURE_HBI_VBI : 0;
		writel(val, csid->base + offset);
		if (camera_diagnostics)
			dev_info(csid->camss->dev,
				 "rdi-measure-write: csid%u rdi%u enable%u cfg0@%04x requested%08x read%08x\n",
				 csid->id, port, enable, offset, val,
				 readl(csid->base + offset));
	}
	writel(1, csid->base + CSID_RDI_IRQ_PATTERN(port));
	writel(0, csid->base + CSID_RDI_IRQ_PERIOD(port));

	/* Configure pixel store using dedicated register in 980 */
	if (!csid_is_lite(csid))
		__csid_configure_rdi_pix_store(csid, port);

	val = 0;
	writel(val, csid->base + rdi_ctrl_offset);

	val = readl(csid->base + rdi_cfg0_offset);

	if (enable)
		val |= RDI_CFG0_EN;

	writel(val, csid->base + rdi_cfg0_offset);
}

void csid_980_diag_snapshot(struct csid_device *csid, const char *stage)
{
	if (!camera_diagnostics)
		return;
	dev_info(csid->camss->dev,
		 "rx-diag: %s csid%u lite%u phy%u vc-mask %02x update%u cfg %08x/%08x top %08x mask %08x rdi0 %08x ctrl %08x irq %08x mask %08x\n",
		 stage, csid->id, csid_is_lite(csid), csid->phy.csiphy_id,
		 csid->phy.en_vc, csid->phy.need_vc_update,
		 readl(csid->base + CSID_CSI2_RX_CFG0),
		 readl(csid->base + CSID_CSI2_RX_CFG1),
		 readl(csid->base + CSID_TOP_IRQ_STATUS),
		 readl(csid->base + CSID_TOP_IRQ_MASK),
		 readl(csid->base + CSID_RDI_CFG0(0)),
		 readl(csid->base + CSID_RDI_CTRL(0)),
		 readl(csid->base + CSID_CSI2_RDIN_IRQ_STATUS(0)),
		 readl(csid->base + CSID_CSI2_RDIN_IRQ_MASK(0)));
	/* Packet counters are documented by the full CSID980 stock table. */
	if (!csid_is_lite(csid))
		dev_info(csid->camss->dev,
			 "rx-diag: %s csid%u rx-irq %08x/%08x pkts %u ecc %08x crc %u headers %08x/%08x captured %08x/%08x\n",
			 stage, csid->id,
			 readl(csid->base + 0xb4), readl(csid->base + 0xc4),
			 readl(csid->base + 0x440), readl(csid->base + 0x444),
			 readl(csid->base + 0x448), readl(csid->base + 0x410),
			 readl(csid->base + 0x414), readl(csid->base + 0x418),
			 readl(csid->base + 0x41c));
	if (!csid_is_lite(csid))
		dev_info(csid->camss->dev,
			 "rx-diag: %s csid%u capctrl %08x long %08x/%08x footer %08x rup %08x aup %08x\n",
			 stage, csid->id, readl(csid->base + 0x408),
			 readl(csid->base + 0x420), readl(csid->base + 0x424),
			 readl(csid->base + 0x428), readl(csid->base + CSID_RUP_CMD),
			 readl(csid->base + CSID_AUP_CMD));
	if (!csid_is_lite(csid) && csid->phy.is_cphy)
		dev_info(csid->camss->dev,
			 "rx-diag: %s csid%u cphy-header %08x\n",
			 stage, csid->id, readl(csid->base + 0x42c));
	/* Full980 RDI0 offsets from cam_ife_csid_980_rdi_0_reg_info. */
	if (!csid_is_lite(csid)) {
		dev_info(csid->camss->dev,
			 "rx-diag: %s csid%u rdi0 cfg1 %08x multi %08x store %08x bytes %u/%u frame %08x camif %08x/%08x drop %08x/%08x halt %08x\n",
			 stage, csid->id, readl(csid->base + 0xe10),
			 readl(csid->base + 0xe0c), readl(csid->base + 0xe14),
			 readl(csid->base + 0xe24), readl(csid->base + 0xe28),
			 readl(csid->base + 0xe2c), readl(csid->base + 0xe40),
			 readl(csid->base + 0xe44), readl(csid->base + 0xe48),
			 readl(csid->base + 0xe4c), readl(csid->base + 0xe74));
		dev_info(csid->camss->dev,
			 "rx-diag: %s csid%u rdi0 measure-cfg %08x/%08x/%08x measure %08x/%08x/%08x sof %08x%08x eof %08x%08x frame-id %08x\n",
			 stage, csid->id, readl(csid->base + 0xe88),
			 readl(csid->base + 0xe8c), readl(csid->base + 0xe90),
			 readl(csid->base + 0xe94), readl(csid->base + 0xe98),
			 readl(csid->base + 0xe9c), readl(csid->base + 0xea4),
			 readl(csid->base + 0xea0), readl(csid->base + 0xeb4),
			 readl(csid->base + 0xeb0), readl(csid->base + 0xf04));
	}
}

/* Capture slots latch only the first match until their reset strobes. */
static void csid_packet_diag_work(struct work_struct *work)
{
	struct csid_device *csid = container_of(to_delayed_work(work),
					      struct csid_device, packet_diag_work);
	u32 irq = readl(csid->base + 0xb4);
	u32 irq2 = readl(csid->base + 0xc4);
	u8 sample = csid->packet_diag_samples++;

	dev_info(csid->camss->dev,
		 "packet-sample: csid%u phy%u vc%u n%u irq %08x/%08x pkts %u short %08x/%08x long %08x/%08x footer %08x cphy %08x unmapped %08x/%08x\n",
		 csid->id, csid->phy.csiphy_id, csid->phy.image_vc, sample,
		 irq, irq2, readl(csid->base + 0x440),
		 readl(csid->base + 0x418), readl(csid->base + 0x41c),
		 readl(csid->base + 0x420), readl(csid->base + 0x424),
		 readl(csid->base + 0x428), readl(csid->base + 0x42c),
		 readl(csid->base + 0x410), readl(csid->base + 0x414));
	if (csid->packet_diag_samples >= 16)
		return;

	/* Clear only capture events, then rearm slots; preserve path errors. */
	writel(irq & BIT(28), csid->base + 0xbc);
	writel(irq2 & 7, csid->base + 0xcc);
	writel(IRQ_CMD_CLEAR, csid->base + CSID_IRQ_CMD);
	writel(0xf, csid->base + 0x40c);
	/* Vary the interval to avoid sampling only one phase of 30 fps. */
	schedule_delayed_work(&csid->packet_diag_work,
			      msecs_to_jiffies(17 + sample % 13));
}

static void csid_configure_stream(struct csid_device *csid, u8 enable)
{
	u8 i, k;

	if (!enable)
		cancel_delayed_work_sync(&csid->packet_diag_work);

	__csid_configure_rx(csid, &csid->phy);

	for (i = 0; i < MSM_CSID_MAX_SRC_STREAMS_980; i++) {
		if (csid->phy.en_vc & BIT(i)) {
			u32 mask = readl(csid->base + CSID_CSI2_RDIN_IRQ_MASK(i));

			writel(INFO_CAMIF_SOF | INFO_RUP_DONE,
			       csid->base + CSID_CSI2_RDIN_IRQ_CLEAR(i));
			mask = enable ? mask | INFO_CAMIF_SOF | INFO_RUP_DONE
				      : mask & ~(INFO_CAMIF_SOF | INFO_RUP_DONE);
			writel(mask, csid->base + CSID_CSI2_RDIN_IRQ_MASK(i));
			mask = readl(csid->base + CSID_TOP_IRQ_MASK);
			mask = enable ? mask | CSID_RDI_TOP_IRQ(i) : mask & ~CSID_RDI_TOP_IRQ(i);
			writel(mask, csid->base + CSID_TOP_IRQ_MASK);
			writel(IRQ_CMD_CLEAR, csid->base + CSID_IRQ_CMD);
			__csid_configure_rdi_stream(csid, enable, i, csid->phy.image_vc);
			__csid_configure_rx_vc(csid, csid->phy.image_vc);

			for (k = 0; enable && k < CAMSS_INIT_BUF_COUNT; k++) {
				__csid_aup_update(csid, i);
				__csid_rup_update(csid, i);
			}

			__csid_ctrl_rdi(csid, enable, i);
		}
	}
	if (enable && camera_diagnostics && rdi_packet_sampling && !csid_is_lite(csid)) {
		csid->packet_diag_samples = 0;
		schedule_delayed_work(&csid->packet_diag_work, msecs_to_jiffies(23));
	}
}

static int csid_configure_testgen_pattern(struct csid_device *csid, s32 val)
{
	return 0;
}

static void csid_subdev_reg_update(struct csid_device *csid, int port_id,
				   bool clear)
{
	if (clear)
		__csid_aup_rup_clear(csid, port_id);
	else
		__csid_aup_update(csid, port_id);
}

static u64 csid_read_timestamp(struct csid_device *csid, u32 offset)
{
	u32 hi, lo;

	do {
		hi = readl(csid->base + offset + 4);
		lo = readl(csid->base + offset);
	} while (hi != readl(csid->base + offset + 4));

	return (u64)hi << 32 | lo;
}

/**
 * csid_isr - CSID module interrupt service routine
 * @irq: Interrupt line
 * @dev: CSID device
 *
 * Return IRQ_HANDLED on success
 */
static irqreturn_t csid_isr(int irq, void *dev)
{
	struct csid_device *csid = dev;
	u32 val, buf_done_val;
	u8 reset_done;
	int i;

	val = readl(csid->base + CSID_TOP_IRQ_STATUS);
	writel(val, csid->base + CSID_TOP_IRQ_CLEAR);

	reset_done = val & INFO_RST_DONE;

	buf_done_val = readl(csid->base + CSID_BUF_DONE_IRQ_STATUS);
	writel(buf_done_val, csid->base + CSID_BUF_DONE_IRQ_CLEAR);

	for (i = 0; i < MSM_CSID_MAX_SRC_STREAMS_980; i++) {
		if (csid->phy.en_vc & BIT(i)) {
			bool done = buf_done_val & BIT(BUF_DONE_IRQ_STATUS_RDI_OFFSET + i);

			val = readl(csid->base + CSID_CSI2_RDIN_IRQ_STATUS(i));
			writel(val, csid->base + CSID_CSI2_RDIN_IRQ_CLEAR(i));

			if (val & INFO_RUP_DONE)
				csid_subdev_reg_update(csid, i, true);

			if (val & INFO_CAMIF_SOF) {
				/* A new SOF may follow an as-yet unhandled completion. */
				if (done && csid_read_timestamp(csid, CSID_RDI_EOF_TS(i)) <
				    csid_read_timestamp(csid, CSID_RDI_SOF_TS(i))) {
					camss_buf_done(csid->camss, csid->id, i);
					done = false;
				}
				camss_frame_start(csid->camss, csid->id, i);
			}

			if (done)
				camss_buf_done(csid->camss, csid->id, i);
		}
	}

	val = IRQ_CMD_CLEAR;
	writel(val, csid->base + CSID_IRQ_CMD);

	if (reset_done)
		complete(&csid->reset_complete);

	return IRQ_HANDLED;
}

/**
 * csid_reset - Trigger reset on CSID module and wait to complete
 * @csid: CSID device
 *
 * Return 0 on success or a negative error code otherwise
 */
static int csid_reset(struct csid_device *csid)
{
	unsigned long time;
	u32 val;
	int i;

	reinit_completion(&csid->reset_complete);

	val = INFO_RST_DONE | BUF_DONE_IRQ_STATUS;
	writel(val, csid->base + CSID_TOP_IRQ_CLEAR);
	writel(val, csid->base + CSID_TOP_IRQ_MASK);

	val = 0;
	for (i = 0; i < MSM_CSID_MAX_SRC_STREAMS_980; i++) {
		if (csid->phy.en_vc & BIT(i)) {
			/*
			 * Only need to clear buf done IRQ status here,
			 * RUP done IRQ status will be cleared once isr
			 * strobe generated by CSID_RST_CMD
			 */
			val |= BIT(BUF_DONE_IRQ_STATUS_RDI_OFFSET + i);
		}
	}
	writel(val, csid->base + CSID_BUF_DONE_IRQ_CLEAR);
	writel(val, csid->base + CSID_BUF_DONE_IRQ_MASK);

	/* Clear all IRQ status with CLEAR bits set */
	val = IRQ_CMD_CLEAR;
	writel(val, csid->base + CSID_IRQ_CMD);

	val = RST_LOCATION | RST_MODE;
	writel(val, csid->base + CSID_RST_CFG);

	val = SELECT_HW_RST | SELECT_IRQ_RST;
	writel(val, csid->base + CSID_RST_CMD);

	time = wait_for_completion_timeout(&csid->reset_complete,
					   msecs_to_jiffies(CSID_RESET_TIMEOUT_MS));

	if (!time) {
		dev_err(csid->camss->dev, "CSID reset timeout\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static void csid_subdev_init(struct csid_device *csid)
{
	INIT_DELAYED_WORK(&csid->packet_diag_work, csid_packet_diag_work);
	csid->testgen.nmodes = CSID_PAYLOAD_MODE_DISABLED;
}

const struct csid_hw_ops csid_ops_980 = {
	.configure_stream = csid_configure_stream,
	.configure_testgen_pattern = csid_configure_testgen_pattern,
	.hw_version = csid_hw_version,
	.isr = csid_isr,
	.reset = csid_reset,
	.src_pad_code = csid_src_pad_code,
	.subdev_init = csid_subdev_init,
	.reg_update = csid_subdev_reg_update,
};
