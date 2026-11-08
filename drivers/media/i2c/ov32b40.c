// SPDX-License-Identifier: GPL-2.0-only
/* OmniVision OV32B40: fixed 6528x4896 RAW10 mode for Xiaomi dada. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "ov32b40-regs.h"
#include "ov32b40-preview.h"
#include "ov32b40-video60.h"

static bool stock_video60;
module_param(stock_video60, bool, 0444);
MODULE_PARM_DESC(stock_video60, "Use the stock 60fps video mode");


static bool stock_preview;
module_param(stock_preview, bool, 0444);
MODULE_PARM_DESC(stock_preview, "Use stock 8MP 30fps non-HDR preview mode1");

#define OV32B40_XCLK_FREQ		19200000
#define OV32B40_LINK_FREQ		842400000LL
#define OV32B40_WIDTH		(stock_video60 || stock_preview ? 3264 : 6528)
#define OV32B40_HEIGHT		(stock_video60 ? 1836 : stock_preview ? 2448 : 4896)
#define OV32B40_CODE		MEDIA_BUS_FMT_SBGGR10_1X10
#define OV32B40_CHIP_ID_REG	CCI_REG16(0x300b)
#define OV32B40_CHIP_ID		0x3242
#define OV32B40_STREAM_REG	CCI_REG8(0x0100)
#define OV32B40_GROUP_REG	CCI_REG8(0x3208)
#define OV32B40_EXPOSURE_REG	CCI_REG24(0x3500)
#define OV32B40_ANALOGUE_GAIN_REG CCI_REG16(0x3508)
#define OV32B40_DIGITAL_GAIN_REG CCI_REG24(0x3510)
#define OV32B40_DIGITAL_GAIN_MIN 1024
#define OV32B40_DIGITAL_GAIN_MAX 5120
#define OV32B40_VTS		(stock_video60 ? 2640 : stock_preview ? 5280 : 5332)
#define OV32B40_EXPOSURE_MIN	4
#define OV32B40_EXPOSURE_MARGIN	12
#define OV32B40_EXPOSURE_DEFAULT (stock_video60 ? 2620 : stock_preview ? 5260 : 5300)
#define OV32B40_GAIN_MIN		0x100
#define OV32B40_GAIN_MAX		0xff0
#define OV32B40_GAIN_STEP	16
#define OV32B40_GAIN_DEFAULT	0x800

/* Match the stock order: analogue, I/O, digital core. */
static const char * const ov32b40_supply_names[] = {
	"avdd", "dovdd", "dvdd",
};
static const s64 ov32b40_link_freqs[] = { OV32B40_LINK_FREQ };

struct ov32b40 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	/* Adjacent pointers form the exposure/analogue/digital gain control cluster. */
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *analogue_gain;
	struct v4l2_ctrl *digital_gain;
	struct regmap *regmap;
	struct clk *xclk;
	struct gpio_desc *reset;
	struct regulator_bulk_data supplies[ARRAY_SIZE(ov32b40_supply_names)];
	bool streaming;
};

static struct ov32b40 *to_ov32b40(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov32b40, sd);
}

/*
 * The stock mode-0 helper writes the coarse line count without a bit shift.
 * Gain is encoded in units of 1/256. A 1/16 step is valid in every stock
 * analogue gain range, including its coarsest range above 8x.
 */
static int ov32b40_apply_controls(struct ov32b40 *sensor)
{
	int ret, end_ret;

	ret = cci_write(sensor->regmap, OV32B40_GROUP_REG, 0x03, NULL);
	if (ret)
		return ret;

	cci_write(sensor->regmap, OV32B40_EXPOSURE_REG,
		  sensor->exposure->val, &ret);
	cci_write(sensor->regmap, OV32B40_ANALOGUE_GAIN_REG,
		  sensor->analogue_gain->val, &ret);
	/* Stock FillExposure encodes Q10 digital gain as a 24-bit Q16 value. */
	cci_write(sensor->regmap, OV32B40_DIGITAL_GAIN_REG,
		  sensor->digital_gain->val << 6, &ret);

	/* End the group even on failure, but do not launch partial settings. */
	end_ret = cci_write(sensor->regmap, OV32B40_GROUP_REG, 0x13, NULL);
	if (ret)
		return ret;
	if (end_ret)
		return end_ret;

	return cci_write(sensor->regmap, OV32B40_GROUP_REG, 0xa3, NULL);
}

static int ov32b40_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov32b40 *sensor = container_of(ctrl->handler, struct ov32b40,
					    ctrls);
	struct device *dev = sensor->sd.dev;
	int ret;

	/* Gain is clustered with exposure and is committed in the same group. */
	if (ctrl->id != V4L2_CID_EXPOSURE)
		return -EINVAL;

	ret = pm_runtime_get_if_in_use(dev);
	if (ret <= 0)
		return ret;

	ret = ov32b40_apply_controls(sensor);
	pm_runtime_put(dev);
	return ret;
}

static const struct v4l2_ctrl_ops ov32b40_ctrl_ops = {
	.s_ctrl = ov32b40_set_ctrl,
};

static int ov32b40_power_on(struct device *dev)
{
	struct ov32b40 *sensor = to_ov32b40(dev_get_drvdata(dev));
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(sensor->supplies); i++) {
		ret = regulator_enable(sensor->supplies[i].consumer);
		if (ret)
			goto disable_supplies;
		fsleep(1000);
	}
	ret = clk_prepare_enable(sensor->xclk);
	if (ret)
		goto disable_supplies;
	fsleep(1000);
	gpiod_set_value_cansleep(sensor->reset, 0);
	fsleep(5000);
	return 0;

disable_supplies:
	while (i)
		regulator_disable(sensor->supplies[--i].consumer);
	return ret;
}

static int ov32b40_power_off(struct device *dev)
{
	struct ov32b40 *sensor = to_ov32b40(dev_get_drvdata(dev));
	unsigned int i = ARRAY_SIZE(sensor->supplies);

	gpiod_set_value_cansleep(sensor->reset, 1);
	clk_disable_unprepare(sensor->xclk);
	fsleep(1000);
	while (i)
		regulator_disable(sensor->supplies[--i].consumer);
	return 0;
}

static int ov32b40_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov32b40 *sensor = to_ov32b40(sd);
	struct device *dev = sd->dev;
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	if (sensor->streaming == !!enable)
		goto unlock;
	if (!enable) {
		ret = cci_write(sensor->regmap, OV32B40_STREAM_REG, 0, NULL);
		sensor->streaming = false;
		/* Power down even if the sensor no longer acknowledges I2C. */
		pm_runtime_put_sync_suspend(dev);
		goto unlock;
	}

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto unlock;
	/* The first stock init write resets the chip. Allow reset to finish. */
	ret = cci_write(sensor->regmap, ov32b40_init_regs[0].reg,
			ov32b40_init_regs[0].val, NULL);
	if (ret)
		goto power_down;
	fsleep(5000);
	ret = cci_multi_reg_write(sensor->regmap, ov32b40_init_regs + 1,
				  ARRAY_SIZE(ov32b40_init_regs) - 1, NULL);
	if (ret)
		goto power_down;
	ret = cci_multi_reg_write(sensor->regmap,
		stock_video60 ? ov32b40_video60_regs :
		stock_preview ? ov32b40_preview_regs : ov32b40_6528x4896_regs,
		stock_video60 ? ARRAY_SIZE(ov32b40_video60_regs) :
		stock_preview ? ARRAY_SIZE(ov32b40_preview_regs) : ARRAY_SIZE(ov32b40_6528x4896_regs), NULL);
	if (ret)
		goto power_down;
	/* Reapply cached values after reset and the mode register table. */
	ret = __v4l2_ctrl_handler_setup(&sensor->ctrls);
	if (ret)
		goto power_down;
	ret = cci_write(sensor->regmap, OV32B40_STREAM_REG, 1, NULL);
	if (ret)
		goto power_down;
	sensor->streaming = true;
	goto unlock;

power_down:
	cci_write(sensor->regmap, OV32B40_STREAM_REG, 0, NULL);
	pm_runtime_put_sync_suspend(dev);
unlock:
	v4l2_subdev_unlock_state(state);
	return ret;
}

static void ov32b40_fill_format(struct v4l2_mbus_framefmt *fmt)
{
	*fmt = (struct v4l2_mbus_framefmt) {
		.width = OV32B40_WIDTH,
		.height = OV32B40_HEIGHT,
		.code = OV32B40_CODE,
		.field = V4L2_FIELD_NONE,
		.colorspace = V4L2_COLORSPACE_RAW,
		.quantization = V4L2_QUANTIZATION_FULL_RANGE,
		.xfer_func = V4L2_XFER_FUNC_NONE,
	};
}

static int ov32b40_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	struct v4l2_fract *interval;

	ov32b40_fill_format(v4l2_subdev_state_get_format(state, 0));
	interval = v4l2_subdev_state_get_interval(state, 0);
	*interval = (struct v4l2_fract) { .numerator = 1, .denominator = stock_video60 ? 60 : stock_preview ? 30 : 15 };
	return 0;
}

static int ov32b40_enum_code(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index)
		return -EINVAL;
	code->code = OV32B40_CODE;
	return 0;
}

static int ov32b40_enum_size(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_frame_size_enum *size)
{
	if (size->pad || size->index || size->code != OV32B40_CODE)
		return -EINVAL;
	size->min_width = size->max_width = OV32B40_WIDTH;
	size->min_height = size->max_height = OV32B40_HEIGHT;
	return 0;
}

static int ov32b40_set_format(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_format *format)
{
	if (format->pad)
		return -EINVAL;
	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE && to_ov32b40(sd)->streaming)
		return -EBUSY;
	ov32b40_fill_format(&format->format);
	*v4l2_subdev_state_get_format(state, 0) = format->format;
	return 0;
}

static int ov32b40_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
				 struct v4l2_mbus_frame_desc *fd)
{
	if (pad)
		return -EINVAL;
	memset(fd, 0, sizeof(*fd));
	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;
	fd->num_entries = 1;
	fd->entry[0].pixelcode = OV32B40_CODE;
	fd->entry[0].bus.csi2.vc = stock_video60 || stock_preview ? 2 : 0;
	fd->entry[0].bus.csi2.dt = 0x2b;
	return 0;
}

static const struct v4l2_subdev_video_ops ov32b40_video_ops = {
	.s_stream = ov32b40_set_stream,
};
static const struct v4l2_subdev_pad_ops ov32b40_pad_ops = {
	.enum_mbus_code = ov32b40_enum_code,
	.enum_frame_size = ov32b40_enum_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = ov32b40_set_format,
	.get_frame_interval = v4l2_subdev_get_frame_interval,
	.get_frame_desc = ov32b40_get_frame_desc,
};
static const struct v4l2_subdev_ops ov32b40_ops = {
	.video = &ov32b40_video_ops,
	.pad = &ov32b40_pad_ops,
};
static const struct v4l2_subdev_internal_ops ov32b40_internal_ops = {
	.init_state = ov32b40_init_state,
};

static int ov32b40_check_endpoint(struct device *dev)
{
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *fwnode;
	int ret;

	fwnode = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!fwnode)
		return -EINVAL;
	ret = v4l2_fwnode_endpoint_alloc_parse(fwnode, &ep);
	fwnode_handle_put(fwnode);
	if (ret)
		goto free_endpoint;
	if (ep.bus_type != V4L2_MBUS_CSI2_DPHY || ep.bus.mipi_csi2.num_data_lanes != 4 ||
	    ep.nr_of_link_frequencies != 1 || ep.link_frequencies[0] != OV32B40_LINK_FREQ)
		ret = -EINVAL;
free_endpoint:
	v4l2_fwnode_endpoint_free(&ep);
	return ret;
}

static int ov32b40_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct v4l2_fwnode_device_properties props;
	struct ov32b40 *sensor;
	struct v4l2_ctrl *ctrl;
	u64 id;
	unsigned int i;
	int ret;

	ret = ov32b40_check_endpoint(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Expected four D-PHY lanes at 842.4 MHz\n");
	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	v4l2_i2c_subdev_init(&sensor->sd, client, &ov32b40_ops);
	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return PTR_ERR(sensor->regmap);
	sensor->xclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(sensor->xclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xclk), "Cannot get xclk\n");
	if (clk_get_rate(sensor->xclk) != OV32B40_XCLK_FREQ)
		return dev_err_probe(dev, -EINVAL, "Expected 19.2 MHz xclk\n");
	for (i = 0; i < ARRAY_SIZE(sensor->supplies); i++) {
		sensor->supplies[i].supply = ov32b40_supply_names[i];
		sensor->supplies[i].init_load_uA = 150000;
	}
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(sensor->supplies), sensor->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot get supplies\n");
	sensor->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset))
		return dev_err_probe(dev, PTR_ERR(sensor->reset), "Cannot get reset\n");

	ret = ov32b40_power_on(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot power on\n");
	ret = cci_read(sensor->regmap, OV32B40_CHIP_ID_REG, &id, NULL);
	ov32b40_power_off(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot read sensor ID\n");
	if (id != OV32B40_CHIP_ID)
		return dev_err_probe(dev, -ENODEV, "Unexpected sensor ID 0x%04llx\n", id);

	ret = v4l2_ctrl_handler_init(&sensor->ctrls, 6);
	if (ret)
		return ret;
	ctrl = v4l2_ctrl_new_int_menu(&sensor->ctrls, NULL, V4L2_CID_LINK_FREQ,
				     0, 0, ov32b40_link_freqs);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	sensor->exposure = v4l2_ctrl_new_std(&sensor->ctrls, &ov32b40_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     OV32B40_EXPOSURE_MIN,
					     OV32B40_VTS - OV32B40_EXPOSURE_MARGIN,
					     2, OV32B40_EXPOSURE_DEFAULT);
	sensor->analogue_gain = v4l2_ctrl_new_std(&sensor->ctrls,
						 &ov32b40_ctrl_ops,
						 V4L2_CID_ANALOGUE_GAIN,
						 OV32B40_GAIN_MIN, OV32B40_GAIN_MAX,
						 OV32B40_GAIN_STEP,
						 OV32B40_GAIN_DEFAULT);
	sensor->digital_gain = v4l2_ctrl_new_std(&sensor->ctrls,
						&ov32b40_ctrl_ops,
						V4L2_CID_DIGITAL_GAIN,
						OV32B40_DIGITAL_GAIN_MIN,
						OV32B40_DIGITAL_GAIN_MAX,
						1, OV32B40_DIGITAL_GAIN_MIN);
	if (sensor->ctrls.error) {
		ret = sensor->ctrls.error;
		goto free_ctrls;
	}
	v4l2_ctrl_cluster(3, &sensor->exposure);
	/* Pixel-array clock/blanking units still need characterization. */
	ret = v4l2_fwnode_device_parse(dev, &props);
	if (!ret)
		ret = v4l2_ctrl_new_fwnode_properties(&sensor->ctrls, NULL, &props);
	if (!ret)
		ret = sensor->ctrls.error;
	if (ret)
		goto free_ctrls;
	sensor->sd.ctrl_handler = &sensor->ctrls;
	sensor->sd.internal_ops = &ov32b40_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		goto free_ctrls;
	sensor->sd.state_lock = sensor->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto cleanup_entity;
	pm_runtime_set_suspended(dev);
	pm_runtime_enable(dev);
	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto cleanup_subdev;
	return 0;

cleanup_subdev:
	pm_runtime_disable(dev);
	v4l2_subdev_cleanup(&sensor->sd);
cleanup_entity:
	media_entity_cleanup(&sensor->sd.entity);
free_ctrls:
	v4l2_ctrl_handler_free(&sensor->ctrls);
	return ret;
}

static void ov32b40_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov32b40 *sensor = to_ov32b40(sd);

	v4l2_async_unregister_subdev(sd);
	if (sensor->streaming)
		ov32b40_set_stream(sd, 0);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		ov32b40_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrls);
}

static const struct dev_pm_ops ov32b40_pm_ops = {
	SET_RUNTIME_PM_OPS(ov32b40_power_off, ov32b40_power_on, NULL)
};
static const struct of_device_id ov32b40_of_match[] = {
	{ .compatible = "ovti,ov32b40" },
	{ }
};
MODULE_DEVICE_TABLE(of, ov32b40_of_match);

static struct i2c_driver ov32b40_driver = {
	.driver = {
		.name = "ov32b40",
		.of_match_table = ov32b40_of_match,
		.pm = &ov32b40_pm_ops,
	},
	.probe = ov32b40_probe,
	.remove = ov32b40_remove,
};
module_i2c_driver(ov32b40_driver);

MODULE_DESCRIPTION("OmniVision OV32B40 fixed RAW10 sensor driver");
MODULE_LICENSE("GPL");
