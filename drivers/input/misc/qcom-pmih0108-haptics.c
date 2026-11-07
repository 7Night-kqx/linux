// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm PMIH0108 HAP530 DIRECT_PLAY haptics driver.
 *
 * Register sequences derived from Qualcomm's qcom-hv-haptics driver:
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define HAP_REVISION_REG			0x00
#define HAP_PERIPH_TYPE_REG		0x04
#define HAP_PERIPH_TYPE			0x15
#define HAP_CFG_SUBTYPE			0x0a
#define HAP_PTN_SUBTYPE			0x0b
#define HAP530_MAJOR_REV			0x05

#define HAP_CFG_HPWR_INTF_REG		0x0b
#define HAP_HPWR_STATUS_MASK		GENMASK(1, 0)
#define HAP_HPWR_DISABLED		0
#define HAP_HPWR_READY			3
#define HAP_CFG_HW_CONFIG_REG		0x0d
#define HAP_HV_DRIVER_BIT		BIT(1)
#define HAP_CFG_INT_EN_CLR_REG		0x16
#define HAP_FIFO_EMPTY_BIT		BIT(1)
#define HAP_CFG_EN_CTL_REG		0x46
#define HAP_EN_BIT			BIT(7)
#define HAP_CFG_VMAX_REG			0x48
#define HAP_VMAX_STEP_UV			50000
#define HAP_DIRECT_VMAX_MAX_UV		5000000
#define HAP_CFG_DRV_WF_SEL_REG		0x49
#define HAP_DRV_WF_FMT_BIT		BIT(4)
#define HAP_DRV_WF_SEL_MASK		GENMASK(1, 0)
#define HAP_WF_SINE			1
#define HAP_CFG_AUTO_SHUTDOWN_REG		0x4a
#define HAP_WFHOLD_FIFO_BIT		BIT(2)
#define HAP_CFG_SPMI_PLAY_REG		0x4c
#define HAP_PLAY_EN_BIT			BIT(7)
#define HAP_DIRECT_PLAY			1
#define HAP_CFG_BRAKE_MODE_REG		0x50
#define HAP_BRAKE_MODE_MASK		GENMASK(7, 6)
#define HAP_BRAKE_CL			1
#define HAP_BRAKE_SINE_GAIN_MASK		GENMASK(3, 2)
#define HAP_BRAKE_WF_MASK			GENMASK(1, 0)
#define HAP_CFG_TLRA_OL_REG		0x5c
#define HAP_TLRA_STEP_US			5
#define HAP_TLRA_MAX_US			20475
#define HAP_CFG_AUTORES_REG		0x63
#define HAP_AUTORES_EN_BIT		BIT(7)
#define HAP_AUTORES_DELAY_MASK		GENMASK(5, 2)
#define HAP_AUTORES_DELAY_7_CYCLES		14
#define HAP_AUTORES_ERR_MASK		GENMASK(1, 0)
#define HAP_AUTORES_ERR_25_PERCENT	1
#define HAP_CFG_FAULT_CLR_REG		0x66
#define HAP_FAULT_CLR_MASK		(BIT(4) | BIT(2) | BIT(1) | BIT(0))
#define HAP_CFG_VMAX_HDRM_REG		0x67
#define HAP_VMAX_HEADROOM_UV		500000
#define HAP_CFG_ZX_SYNC_REG		0x6f
#define HAP_PDN_ZX_TO_FAULT_BIT		BIT(2)
#define HAP_CFG_CAL_EN_REG		0x72
#define HAP_RC_CLK_CAL_MASK		GENMASK(3, 2)
#define HAP_RC_CLK_CAL_AUTO		1
#define HAP_PTN_DIRECT_PLAY_REG		0x26
#define HAP_PTN_CL_VDRIVE_REG		0x4a
#define HAP_CL_VDRIVE_EN_BIT		BIT(7)
#define HAP_PTN_BRAKE_AMP_REG		0x70
#define HAP_BRAKE_SAMPLES			8

struct qcom_haptics {
	struct device *dev;
	struct regmap *regmap;
	struct input_dev *input;
	struct mutex lock;
	struct work_struct work;
	u32 cfg_base;
	u32 ptn_base;
	u32 vmax_uv;
	u32 period_us;
	u8 brake[HAP_BRAKE_SAMPLES];
	u16 magnitude;
	bool opened;
	bool suspended;
	bool running;
};

static int qcom_haptics_set_vmax(struct qcom_haptics *haptics)
{
	unsigned int value;
	int error, restore_error;

	/* HAP530 requires closed-loop drive to be paused while changing VMAX. */
	error = regmap_set_bits(haptics->regmap,
				haptics->cfg_base + HAP_CFG_ZX_SYNC_REG,
				HAP_PDN_ZX_TO_FAULT_BIT);
	if (error)
		goto restore;

	error = regmap_clear_bits(haptics->regmap,
				  haptics->ptn_base + HAP_PTN_CL_VDRIVE_REG,
				  HAP_CL_VDRIVE_EN_BIT);
	if (error)
		goto restore;

	error = regmap_write(haptics->regmap,
			     haptics->cfg_base + HAP_CFG_FAULT_CLR_REG,
			     HAP_FAULT_CLR_MASK);
	if (error)
		goto restore;

	error = regmap_write(haptics->regmap,
			     haptics->cfg_base + HAP_CFG_VMAX_HDRM_REG,
			     HAP_VMAX_HEADROOM_UV / HAP_VMAX_STEP_UV);
	if (error)
		goto restore;

	error = regmap_write(haptics->regmap,
			     haptics->cfg_base + HAP_CFG_VMAX_REG,
			     haptics->vmax_uv / HAP_VMAX_STEP_UV);
	if (error)
		goto restore;

	error = regmap_read_poll_timeout(haptics->regmap,
			haptics->cfg_base + HAP_CFG_HPWR_INTF_REG, value,
			(value & HAP_HPWR_STATUS_MASK) == HAP_HPWR_DISABLED ||
			(value & HAP_HPWR_STATUS_MASK) == HAP_HPWR_READY,
			1000, 10000);
restore:
	restore_error = regmap_clear_bits(haptics->regmap,
				haptics->cfg_base + HAP_CFG_ZX_SYNC_REG,
				HAP_PDN_ZX_TO_FAULT_BIT);
	if (!error)
		error = restore_error;

	restore_error = regmap_set_bits(haptics->regmap,
				haptics->ptn_base + HAP_PTN_CL_VDRIVE_REG,
				HAP_CL_VDRIVE_EN_BIT);
	if (!error)
		error = restore_error;

	return error;
}

static int qcom_haptics_stop(struct qcom_haptics *haptics)
{
	unsigned int value;
	int error, next_error;

	error = regmap_write(haptics->regmap,
			     haptics->cfg_base + HAP_CFG_SPMI_PLAY_REG,
			     HAP_DIRECT_PLAY);
	if (!error)
		error = regmap_read_poll_timeout(haptics->regmap,
				haptics->cfg_base + HAP_CFG_HPWR_INTF_REG, value,
				(value & HAP_HPWR_STATUS_MASK) == HAP_HPWR_DISABLED,
				1000, 30000);

	/* Always attempt every shutdown step, even after a transfer failure. */
	next_error = regmap_write(haptics->regmap,
				  haptics->ptn_base + HAP_PTN_DIRECT_PLAY_REG, 0);
	if (!error)
		error = next_error;

	next_error = regmap_clear_bits(haptics->regmap,
				       haptics->cfg_base + HAP_CFG_EN_CTL_REG,
				       HAP_EN_BIT);
	if (!error)
		error = next_error;

	haptics->running = false;
	return error;
}

static int qcom_haptics_start(struct qcom_haptics *haptics, u16 magnitude)
{
	u8 amplitude = DIV_ROUND_CLOSEST((u32)magnitude * 255, 65535);
	int error;

	if (!amplitude)
		return qcom_haptics_stop(haptics);

	if (!haptics->running) {
		error = regmap_write(haptics->regmap,
				     haptics->cfg_base + HAP_CFG_FAULT_CLR_REG,
				     HAP_FAULT_CLR_MASK);
		if (error)
			return error;
	}

	error = regmap_write(haptics->regmap,
			     haptics->ptn_base + HAP_PTN_DIRECT_PLAY_REG, amplitude);
	if (error || haptics->running)
		return error;

	error = regmap_set_bits(haptics->regmap,
				haptics->cfg_base + HAP_CFG_EN_CTL_REG, HAP_EN_BIT);
	if (error)
		return error;

	error = regmap_write(haptics->regmap,
			     haptics->cfg_base + HAP_CFG_SPMI_PLAY_REG,
			     HAP_DIRECT_PLAY | HAP_PLAY_EN_BIT);
	if (!error)
		haptics->running = true;

	return error;
}

static int qcom_haptics_hw_init(struct qcom_haptics *haptics)
{
	u32 period = haptics->period_us / HAP_TLRA_STEP_US;
	u8 period_data[] = { period >> 8, period & 0xff };
	int error;

	error = qcom_haptics_stop(haptics);
	if (error)
		return error;

	/* DIRECT_PLAY does not consume the FIFO interrupt or waveform memory. */
	error = regmap_write(haptics->regmap,
			     haptics->cfg_base + HAP_CFG_INT_EN_CLR_REG,
			     HAP_FIFO_EMPTY_BIT);
	if (error)
		return error;

	error = regmap_clear_bits(haptics->regmap,
				  haptics->cfg_base + HAP_CFG_AUTO_SHUTDOWN_REG,
				  HAP_WFHOLD_FIFO_BIT);
	if (error)
		return error;

	error = qcom_haptics_set_vmax(haptics);
	if (error)
		return error;

	error = regmap_update_bits(haptics->regmap,
				haptics->cfg_base + HAP_CFG_DRV_WF_SEL_REG,
				HAP_DRV_WF_SEL_MASK | HAP_DRV_WF_FMT_BIT, HAP_WF_SINE);
	if (error)
		return error;

	error = regmap_update_bits(haptics->regmap,
			haptics->cfg_base + HAP_CFG_BRAKE_MODE_REG,
			HAP_BRAKE_MODE_MASK | HAP_BRAKE_SINE_GAIN_MASK | HAP_BRAKE_WF_MASK,
			FIELD_PREP(HAP_BRAKE_MODE_MASK, HAP_BRAKE_CL) | HAP_WF_SINE);
	if (error)
		return error;

	error = regmap_bulk_write(haptics->regmap,
				  haptics->ptn_base + HAP_PTN_BRAKE_AMP_REG,
				  haptics->brake, sizeof(haptics->brake));
	if (error)
		return error;

	error = regmap_update_bits(haptics->regmap,
				  haptics->cfg_base + HAP_CFG_CAL_EN_REG,
				  HAP_RC_CLK_CAL_MASK,
				  FIELD_PREP(HAP_RC_CLK_CAL_MASK, HAP_RC_CLK_CAL_AUTO));
	if (error)
		return error;

	error = regmap_bulk_write(haptics->regmap,
				  haptics->cfg_base + HAP_CFG_TLRA_OL_REG,
				  period_data, sizeof(period_data));
	if (error)
		return error;

	/* Hardware tracks the LRA resonance during sinusoidal direct playback. */
	return regmap_update_bits(haptics->regmap,
			haptics->cfg_base + HAP_CFG_AUTORES_REG,
			HAP_AUTORES_EN_BIT | HAP_AUTORES_DELAY_MASK | HAP_AUTORES_ERR_MASK,
			HAP_AUTORES_EN_BIT |
			FIELD_PREP(HAP_AUTORES_DELAY_MASK, HAP_AUTORES_DELAY_7_CYCLES) |
			HAP_AUTORES_ERR_25_PERCENT);
}

static void qcom_haptics_work(struct work_struct *work)
{
	struct qcom_haptics *haptics = container_of(work, struct qcom_haptics, work);
	int error;

	guard(mutex)(&haptics->lock);

	if (READ_ONCE(haptics->suspended) || !READ_ONCE(haptics->opened))
		error = qcom_haptics_stop(haptics);
	else
		error = qcom_haptics_start(haptics, READ_ONCE(haptics->magnitude));

	if (error) {
		qcom_haptics_stop(haptics);
		dev_err_ratelimited(haptics->dev, "Haptics playback failed: %d\n", error);
	}
}

static int qcom_haptics_play_effect(struct input_dev *input, void *data,
				    struct ff_effect *effect)
{
	struct qcom_haptics *haptics = input_get_drvdata(input);

	/* Called with input->event_lock held; SPMI transfers run in work context. */
	if (haptics->suspended || !haptics->opened)
		return 0;

	WRITE_ONCE(haptics->magnitude, max(effect->u.rumble.strong_magnitude,
					 effect->u.rumble.weak_magnitude));
	schedule_work(&haptics->work);
	return 0;
}

static int qcom_haptics_open(struct input_dev *input)
{
	struct qcom_haptics *haptics = input_get_drvdata(input);

	guard(spinlock_irq)(&input->event_lock);
	WRITE_ONCE(haptics->opened, true);
	return 0;
}

static void qcom_haptics_close(struct input_dev *input)
{
	struct qcom_haptics *haptics = input_get_drvdata(input);
	int error;

	scoped_guard(spinlock_irq, &input->event_lock) {
		WRITE_ONCE(haptics->opened, false);
		WRITE_ONCE(haptics->magnitude, 0);
	}
	cancel_work_sync(&haptics->work);

	guard(mutex)(&haptics->lock);
	error = qcom_haptics_stop(haptics);
	if (error)
		dev_err(haptics->dev, "Failed to stop haptics on close: %d\n", error);
}

static int qcom_haptics_quiesce(struct qcom_haptics *haptics)
{
	scoped_guard(spinlock_irq, &haptics->input->event_lock) {
		WRITE_ONCE(haptics->suspended, true);
		WRITE_ONCE(haptics->magnitude, 0);
	}
	cancel_work_sync(&haptics->work);

	guard(mutex)(&haptics->lock);
	return qcom_haptics_stop(haptics);
}

static void qcom_haptics_cleanup(void *data)
{
	struct qcom_haptics *haptics = data;
	int error;

	error = qcom_haptics_quiesce(haptics);
	if (error)
		dev_err(haptics->dev, "Failed to disable haptics: %d\n", error);
}

static int qcom_haptics_check_revision(struct qcom_haptics *haptics, u32 base,
				      u8 subtype)
{
	u8 revision[2], type[2];
	int error;

	error = regmap_bulk_read(haptics->regmap, base + HAP_REVISION_REG,
				 revision, sizeof(revision));
	if (error)
		return error;

	error = regmap_bulk_read(haptics->regmap, base + HAP_PERIPH_TYPE_REG,
				 type, sizeof(type));
	if (error)
		return error;

	if (revision[1] != HAP530_MAJOR_REV ||
	    type[0] != HAP_PERIPH_TYPE || type[1] != subtype)
		return dev_err_probe(haptics->dev, -ENODEV,
			"Unsupported haptics block at %#x: revision %u.%u, type %#x/%#x\n",
			base, revision[1], revision[0], type[0], type[1]);

	return 0;
}

static int qcom_haptics_parse_properties(struct qcom_haptics *haptics)
{
	struct device *dev = haptics->dev;
	int error, count;

	if (of_property_count_u32_elems(dev->of_node, "reg") != 2)
		return -EINVAL;

	error = of_property_read_u32_index(dev->of_node, "reg", 0, &haptics->cfg_base);
	if (error)
		return error;

	error = of_property_read_u32_index(dev->of_node, "reg", 1, &haptics->ptn_base);
	if (error)
		return error;

	if (haptics->cfg_base > 0xff00 || haptics->ptn_base > 0xff00 ||
	    (haptics->cfg_base & 0xff) || (haptics->ptn_base & 0xff) ||
	    haptics->cfg_base == haptics->ptn_base)
		return -EINVAL;

	error = device_property_read_u32(dev, "qcom,vmax-microvolt", &haptics->vmax_uv);
	if (error)
		return error;

	/* Stay below the charger firmware's 5 V clamp without a vendor notifier. */
	if (haptics->vmax_uv < HAP_VMAX_STEP_UV ||
	    haptics->vmax_uv > HAP_DIRECT_VMAX_MAX_UV ||
	    haptics->vmax_uv % HAP_VMAX_STEP_UV)
		return dev_err_probe(dev, -EINVAL, "Invalid direct-play voltage\n");

	error = device_property_read_u32(dev, "qcom,lra-period-us", &haptics->period_us);
	if (error)
		return error;

	if (haptics->period_us < HAP_TLRA_STEP_US ||
	    haptics->period_us > HAP_TLRA_MAX_US ||
	    haptics->period_us % HAP_TLRA_STEP_US)
		return dev_err_probe(dev, -EINVAL, "Invalid LRA period\n");

	if (!device_property_present(dev, "qcom,brake-pattern"))
		return 0;

	count = device_property_count_u8(dev, "qcom,brake-pattern");
	if (count < 1 || count > HAP_BRAKE_SAMPLES)
		return -EINVAL;

	return device_property_read_u8_array(dev, "qcom,brake-pattern",
					     haptics->brake, count);
}

static int qcom_haptics_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qcom_haptics *haptics;
	struct input_dev *input;
	unsigned int value;
	int error;

	haptics = devm_kzalloc(dev, sizeof(*haptics), GFP_KERNEL);
	if (!haptics)
		return -ENOMEM;

	haptics->dev = dev;
	haptics->regmap = dev_get_regmap(dev->parent, NULL);
	if (!haptics->regmap)
		return dev_err_probe(dev, -ENXIO, "Missing PMIC regmap\n");

	error = qcom_haptics_parse_properties(haptics);
	if (error)
		return dev_err_probe(dev, error, "Invalid haptics properties\n");

	error = qcom_haptics_check_revision(haptics, haptics->cfg_base, HAP_CFG_SUBTYPE);
	if (error)
		return error;

	error = qcom_haptics_check_revision(haptics, haptics->ptn_base, HAP_PTN_SUBTYPE);
	if (error)
		return error;

	error = regmap_read(haptics->regmap,
			    haptics->cfg_base + HAP_CFG_HW_CONFIG_REG, &value);
	if (error)
		return error;
	if (!(value & HAP_HV_DRIVER_BIT))
		return dev_err_probe(dev, -ENODEV, "Haptics block is not high-voltage\n");

	error = devm_mutex_init(dev, &haptics->lock);
	if (error)
		return error;
	INIT_WORK(&haptics->work, qcom_haptics_work);

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	haptics->input = input;
	input->name = "qcom-pmih0108-haptics";
	input->id.bustype = BUS_HOST;
	input->open = qcom_haptics_open;
	input->close = qcom_haptics_close;
	input_set_drvdata(input, haptics);
	platform_set_drvdata(pdev, haptics);

	/* Registered before input_register_device(), so FF teardown runs first. */
	error = devm_add_action_or_reset(dev, qcom_haptics_cleanup, haptics);
	if (error)
		return error;

	error = qcom_haptics_hw_init(haptics);
	if (error)
		return dev_err_probe(dev, error, "Failed to initialize haptics\n");

	input_set_capability(input, EV_FF, FF_RUMBLE);
	error = input_ff_create_memless(input, NULL, qcom_haptics_play_effect);
	if (error)
		return error;

	return input_register_device(input);
}

static int qcom_haptics_suspend(struct device *dev)
{
	struct qcom_haptics *haptics = dev_get_drvdata(dev);

	guard(mutex)(&haptics->input->mutex);
	return qcom_haptics_quiesce(haptics);
}

static int qcom_haptics_resume(struct device *dev)
{
	struct qcom_haptics *haptics = dev_get_drvdata(dev);
	int error;

	guard(mutex)(&haptics->input->mutex);

	scoped_guard(mutex, &haptics->lock) {
		error = qcom_haptics_hw_init(haptics);
		if (error) {
			qcom_haptics_stop(haptics);
			return error;
		}
	}

	guard(spinlock_irq)(&haptics->input->event_lock);
	WRITE_ONCE(haptics->suspended, false);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(qcom_haptics_pm_ops,
				qcom_haptics_suspend, qcom_haptics_resume);

static void qcom_haptics_shutdown(struct platform_device *pdev)
{
	struct qcom_haptics *haptics = platform_get_drvdata(pdev);

	qcom_haptics_cleanup(haptics);
}

static const struct of_device_id qcom_haptics_of_match[] = {
	{ .compatible = "qcom,pmih0108-haptics" },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_haptics_of_match);

static struct platform_driver qcom_haptics_driver = {
	.probe = qcom_haptics_probe,
	.shutdown = qcom_haptics_shutdown,
	.driver = {
		.name = "qcom-pmih0108-haptics",
		.of_match_table = qcom_haptics_of_match,
		.pm = pm_sleep_ptr(&qcom_haptics_pm_ops),
	},
};
module_platform_driver(qcom_haptics_driver);

MODULE_DESCRIPTION("Qualcomm PMIH0108 HAP530 haptics driver");
MODULE_LICENSE("GPL");
