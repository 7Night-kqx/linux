// SPDX-License-Identifier: GPL-2.0-only
/* OmniVision OVX9100: experimental fixed 4096x3072 RAW10 mode for Xiaomi dada. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "ovx9100-regs.h"
#include "ovx9100-still50.h"
#include "ovx9100-video60.h"

static bool stock_video60;
module_param(stock_video60, bool, 0444);
MODULE_PARM_DESC(stock_video60, "Use the stock 60fps video mode");


#define OVX9100_XCLK_FREQ 19200000
#define OVX9100_WIDTH 4096
#define OVX9100_HEIGHT (stock_video60 ? 2560 : 3072)
#define OVX9100_VTS (stock_video60 ? 1386 : stock_still50 ? 3200 : 2774)
#define OVX9100_EXPOSURE_MAX (stock_still50 && !stock_video60 ? 10000 : OVX9100_VTS - 38)
#define OVX9100_CODE MEDIA_BUS_FMT_SRGGB10_1X10
#define OVX9100_CHIP_ID_REG CCI_REG16(0x300a)
#define OVX9100_CHIP_ID 0x5650
#define OVX9100_STREAM_REG CCI_REG8(0x0100)
/* Bring-up parameters require evidence for the actual phone/module. */
static bool stock_still50;
module_param(stock_still50, bool, 0444);
MODULE_PARM_DESC(stock_still50, "Use the Android Ofilm initialization for full-resolution still probes");
static bool still50_calibration = true;
module_param(still50_calibration, bool, 0444);
MODULE_PARM_DESC(still50_calibration, "Apply checked physical module correction in the 50MP probe");
static bool probe_diagnostics;
module_param(probe_diagnostics, bool, 0444);
MODULE_PARM_DESC(probe_diagnostics, "Log power and probe stages for bring-up");
static bool stream_diagnostics;
module_param(stream_diagnostics, bool, 0444);
MODULE_PARM_DESC(stream_diagnostics, "Read stream and CSI configuration for bring-up");
static bool experimental_stream;
module_param(experimental_stream, bool, 0444);
MODULE_PARM_DESC(
	experimental_stream,
	"Enable fixed-mode experiments after receiver characterization");
static unsigned long symbol_rate;
module_param(symbol_rate, ulong, 0444);
MODULE_PARM_DESC(symbol_rate, "Confirmed C-PHY symbols per second per trio");
static int image_vc = -1;
module_param(image_vc, int, 0444);
MODULE_PARM_DESC(image_vc, "Select stock RAW10 virtual channel 0 or 1 for bring-up");
static char *module_variant;
module_param(module_variant, charp, 0444);
MODULE_PARM_DESC(module_variant, "Confirmed module variant: ofilm or sunny");
static const char *const ovx9100_supply_names[] = {
	"bob", "vaf", "avdd", "dovdd", "dvdd",
};
static const int ovx9100_supply_voltages[] = {
	3296000, 3304000, 2900000, 1800000, 1200000,
};
static const int ovx9100_supply_loads[] = {
	2000000, 150000, 119000, 4000, 913000,
};
static const int ovx9100_power_up_delays[] = { 2000, 1000, 0, 1000, 1000 };

struct ovx9100 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *again;
	s64 link_freqs[1];
	struct regmap *regmap;
	struct clk *xclk;
	struct gpio_desc *reset;
	struct regulator_bulk_data supplies[ARRAY_SIZE(ovx9100_supply_names)];
	bool streaming;
};

static struct ovx9100 *to_ovx9100(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ovx9100, sd);
}

static int ovx9100_write_regs(struct ovx9100 *sensor,
			      const struct ovx9100_reg *regs,
			      unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = cci_write(sensor->regmap, CCI_REG8(regs[i].address),
				regs[i].value, NULL);
		if (ret)
			return ret;
		if (regs[i].delay_us)
			fsleep(regs[i].delay_us);
		/* Inferred conservative reset guard; the stock BIN omits this delay. */
		if (regs[i].address == 0x0103 && regs[i].value == 1)
			fsleep(5000);
	}
	return 0;
}

/* Ofilm module BIN: three signed-byte settings in one checked EEPROM block. */
#define OVX9100_EEPROM_ADDRESS 0x51
#define OVX9100_CORRECTION_FLAG 0x2954
#define OVX9100_CORRECTION_CHECKSUM 0x3756
#define OVX9100_CORRECTION_BYTES \
	(OVX9100_CORRECTION_CHECKSUM - OVX9100_CORRECTION_FLAG + 1)

static int ovx9100_read_eeprom(struct ovx9100 *sensor, u16 start,
			       u8 *data, unsigned int size)
{
	struct i2c_client *client = v4l2_get_subdevdata(&sensor->sd);
	unsigned int offset;

	/* CCI read payload is limited to 12 bytes; writes only set the pointer. */
	for (offset = 0; offset < size; offset += 12) {
		u16 address = start + offset;
		u8 pointer[] = { address >> 8, address & 0xff };
		struct i2c_msg messages[] = {
			{ .addr = OVX9100_EEPROM_ADDRESS,
			  .len = sizeof(pointer), .buf = pointer },
			{ .addr = OVX9100_EEPROM_ADDRESS, .flags = I2C_M_RD,
			  .len = min_t(unsigned int, 12, size - offset),
			  .buf = data + offset },
		};
		int ret;

		ret = i2c_transfer(client->adapter, messages, ARRAY_SIZE(messages));
		if (ret != ARRAY_SIZE(messages))
			return ret < 0 ? ret : -EIO;
	}
	return 0;
}

static int ovx9100_apply_correction(struct ovx9100 *sensor)
{
	static const struct {
		u16 otp;
		u16 reg;
		u16 size;
	} settings[] = {
		{ 0x2956, 0x5a20, 32 },
		{ 0x2976, 0x5ac0, 3536 },
		{ 0x3746, 0x68ae, 16 },
	};
	struct device *dev = sensor->sd.dev;
	u8 header[0x25], *data;
	unsigned int i, j, sum = 0, checked = 0;
	u64 value;
	int ret;

	/* Check the physical module header before accessing its correction block. */
	ret = ovx9100_read_eeprom(sensor, 0, header, sizeof(header));
	if (ret)
		return ret;
	for (i = 1; i < sizeof(header) - 1; i++)
		sum += header[i];
	if (header[0] != 1 || header[sizeof(header) - 1] != sum % 255 + 1)
		return -EBADMSG;
	data = kmalloc(OVX9100_CORRECTION_BYTES, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	ret = ovx9100_read_eeprom(sensor, OVX9100_CORRECTION_FLAG, data,
				  OVX9100_CORRECTION_BYTES);
	if (ret)
		goto free_data;
	sum = 0;
	for (i = 1; i < OVX9100_CORRECTION_BYTES - 1; i++)
		sum += data[i];
	if (data[0] != 1 || data[OVX9100_CORRECTION_BYTES - 1] != sum % 255 + 1) {
		ret = -EBADMSG;
		goto free_data;
	}
	dev_info(dev, "module-correction: checksum checked header=1 block=1 bytes=%u\n",
		 OVX9100_CORRECTION_BYTES);
	if (stream_diagnostics) {
		for (i = 0; i < OVX9100_CORRECTION_BYTES; i += 12) {
			unsigned int size = min_t(unsigned int, 12,
						 OVX9100_CORRECTION_BYTES - i);

			dev_info(dev, "correction-eeprom: offset%04x bytes%u data%*phN\n",
				 OVX9100_CORRECTION_FLAG + i, size, size, data + i);
		}
	}
	/* Use explicit incrementing registers from BIN, independent of burst logs. */
	for (i = 0; i < ARRAY_SIZE(settings); i++) {
		unsigned int offset = settings[i].otp - OVX9100_CORRECTION_FLAG;

		for (j = 0; j < settings[i].size; j++) {
			ret = cci_write(sensor->regmap, CCI_REG8(settings[i].reg + j),
					data[offset + j], NULL);
			if (ret)
				goto free_data;
		}
		/* Verify every stored calibration byte before allowing any frames. */
		for (j = 0; j < settings[i].size; j++) {
			ret = cci_read(sensor->regmap, CCI_REG8(settings[i].reg + j),
				       &value, NULL);
			if (ret)
				goto free_data;
			if (value != data[offset + j]) {
				dev_err(dev, "correction-readback: reg%04x expected%02x read%02llx\n",
					settings[i].reg + j, data[offset + j], value);
				ret = -EIO;
				goto free_data;
			}
			checked++;
		}
		dev_info(dev, "correction-readback: base%04x bytes%u matched=1\n",
			 settings[i].reg, settings[i].size);
	}
	dev_info(dev, "module-correction: applied=1 checked=%u\n", checked);
free_data:
	kfree(data);
	return ret;
}

static bool ovx9100_transport_reg(u16 address)
{
	return (address >= 0x0300 && address <= 0x031f) ||
	       (address >= 0x3800 && address <= 0x38ff) ||
	       (address >= 0x4800 && address <= 0x48ff);
}

static void ovx9100_transport_readback(struct ovx9100 *sensor,
				       const struct ovx9100_reg *regs,
				       unsigned int count, const char *stage)
{
	unsigned int i, j, checked = 0, different = 0, errors = 0;
	u64 value;
	int ret;

	if (!stream_diagnostics)
		return;
	for (i = 0; i < count; i++) {
		if (!ovx9100_transport_reg(regs[i].address))
			continue;
		/* Compare only the last write to each address in this table. */
		for (j = i + 1; j < count; j++)
			if (regs[j].address == regs[i].address)
				break;
		if (j != count)
			continue;
		ret = cci_read(sensor->regmap, CCI_REG8(regs[i].address), &value, NULL);
		checked++;
		if (ret)
			errors++;
		else if (value != regs[i].value)
			different++;
		if (ret || value != regs[i].value)
			dev_info(sensor->sd.dev,
				 "transport-readback: %s reg%04x expected%02x read%02llx rc%d\n",
				 stage, regs[i].address, regs[i].value, ret ? 0 : value, ret);
	}
	/* Differences may include read-only/self-changing fields, not write failures. */
	dev_info(sensor->sd.dev,
		 "transport-readback: %s checked%u different%u errors%u\n",
		 stage, checked, different, errors);
}

static int ovx9100_power_on(struct device *dev)
{
	struct ovx9100 *sensor = to_ovx9100(dev_get_drvdata(dev));
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(sensor->supplies); i++) {
		if (probe_diagnostics)
			dev_info(dev, "probe-diag: rail %s voltage %d begin\n",
				 ovx9100_supply_names[i], ovx9100_supply_voltages[i]);
		ret = regulator_set_voltage(sensor->supplies[i].consumer,
					    ovx9100_supply_voltages[i],
					    ovx9100_supply_voltages[i]);
		if (ret)
			goto disable_supplies;
		if (probe_diagnostics)
			dev_info(dev, "probe-diag: rail %s enable begin\n",
				 ovx9100_supply_names[i]);
		ret = regulator_enable(sensor->supplies[i].consumer);
		if (ret) {
			regulator_set_voltage(sensor->supplies[i].consumer, 0,
					      INT_MAX);
			goto disable_supplies;
		}
		if (probe_diagnostics)
			dev_info(dev, "probe-diag: rail %s enabled\n",
				 ovx9100_supply_names[i]);
		if (ovx9100_power_up_delays[i])
			fsleep(ovx9100_power_up_delays[i]);
	}
	if (probe_diagnostics)
		dev_info(dev, "probe-diag: mclk enable begin\n");
	ret = clk_prepare_enable(sensor->xclk);
	if (probe_diagnostics)
		dev_info(dev, "probe-diag: mclk enable returned %d\n", ret);
	if (ret)
		goto disable_supplies;
	fsleep(1000);
	if (probe_diagnostics)
		dev_info(dev, "probe-diag: reset release begin\n");
	gpiod_set_value_cansleep(sensor->reset, 0);
	fsleep(5000);
	return 0;

disable_supplies:
	while (i) {
		regulator_disable(sensor->supplies[--i].consumer);
		regulator_set_voltage(sensor->supplies[i].consumer, 0, INT_MAX);
	}
	return ret;
}

static int ovx9100_power_off(struct device *dev)
{
	struct ovx9100 *sensor = to_ovx9100(dev_get_drvdata(dev));
	unsigned int i = ARRAY_SIZE(sensor->supplies);

	if (probe_diagnostics)
		dev_info(dev, "probe-diag: power off begin\n");
	clk_disable_unprepare(sensor->xclk);
	fsleep(1000);
	gpiod_set_value_cansleep(sensor->reset, 1);
	fsleep(1000);
	while (i) {
		regulator_disable(sensor->supplies[--i].consumer);
		regulator_set_voltage(sensor->supplies[i].consumer, 0, INT_MAX);
	}
	fsleep(2000);
	return 0;
}

static void ovx9100_stream_snapshot(struct ovx9100 *sensor, const char *stage)
{
	static const u16 regs[] = {
		0x0100, 0x0304, 0x0305, 0x0306, 0x0307,
		0x3808, 0x3809, 0x380a, 0x380b, 0x380c, 0x380d,
		0x380e, 0x380f, 0x4800, 0x4802, 0x4813, 0x4837, 0x4860,
		0x3500, 0x3501, 0x3502, 0x3508, 0x3509, 0x350a, 0x350b, 0x350c,
		0x3540, 0x3541, 0x3542, 0x3580, 0x3581, 0x3582,
	};
	unsigned int i;

	if (!stream_diagnostics)
		return;
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		u64 value = 0;
		int ret = cci_read(sensor->regmap, CCI_REG8(regs[i]), &value, NULL);

		dev_info(sensor->sd.dev,
			 "stream-diag: %s reg %04x rc %d val %02llx\n",
			 stage, regs[i], ret, value);
	}
}

static const char * const ovx9100_pattern_menu[] = {
	"Disabled",
	"Stock Custom1",
};

static int ovx9100_set_pattern(struct v4l2_ctrl *ctrl)
{
	struct ovx9100 *sensor = container_of(ctrl->handler, struct ovx9100, ctrls);
	struct device *dev = sensor->sd.dev;
	u64 value;
	unsigned int i, count;
	int ret, read_ret;
	const struct ovx9100_reg off[] = {
		{ 0x50c1, 0, 0 },
	};
	const struct ovx9100_reg on[] = {
		{ 0x50c1, 1, 0 }, { 0x50c2, 4, 0 },
		{ 0x53c1, 1, 0 }, { 0x53c2, 4, 0 },
		{ 0x56c1, 1, 0 }, { 0x56c2, 4, 0 },
	};
	const struct ovx9100_reg *regs = ctrl->val ? on : off;

	if (ctrl->id != V4L2_CID_TEST_PATTERN || !experimental_stream ||
	    !module_variant || strcmp(module_variant, "ofilm"))
		return -EINVAL;
	if (!pm_runtime_get_if_in_use(dev))
		return 0;
	/* Exact OFF/Custom1 sequence from the Ofilm module. */
	count = ctrl->val ? ARRAY_SIZE(on) : ARRAY_SIZE(off);
	ret = ovx9100_write_regs(sensor, regs, count);
	if (!ret && stream_diagnostics) {
		for (i = 0; i < count; i++) {
			read_ret = cci_read(sensor->regmap, CCI_REG8(regs[i].address),
					    &value, NULL);
			dev_info(dev, "pattern-readback: reg%04x requested%02x read%02llx rc%d\n",
				 regs[i].address, regs[i].value, read_ret ? 0 : value, read_ret);
		}
	}
	pm_runtime_put_autosuspend(dev);
	return ret;
}

static int ovx9100_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ovx9100 *sensor = container_of(ctrl->handler, struct ovx9100, ctrls);
	struct device *dev = sensor->sd.dev;
	u32 exposure = sensor->exposure->val, gain = sensor->again->val;
	u32 frame_length = stock_still50 && !stock_video60 ?
		max_t(u32, OVX9100_VTS, exposure + 38) : OVX9100_VTS;
	u64 value;
	unsigned int i;
	int ret, read_ret;
	/* Ofilm normal mode1: grouped byte writes, no mode switch. */
	const struct ovx9100_reg regs[] = {
		{ 0x3208, 0x00, 0 },
		{ 0x3840, 0, 0 }, { 0x380e, frame_length >> 8, 0 },
		{ 0x380f, frame_length & 0xff, 0 },
		{ 0x5006, 0, 0 },
		{ 0x3500, (exposure >> 16) & 0xff, 0 },
		{ 0x3501, (exposure >> 8) & 0xff, 0 },
		{ 0x3502, exposure & 0xff, 0 },
		{ 0x3508, (gain >> 8) & 0xff, 0 },
		{ 0x3509, gain & 0xfe, 0 },
		/* Digital register gain 1024 (1x), encoded as three bytes. */
		{ 0x350a, 1, 0 }, { 0x350b, 0, 0 }, { 0x350c, 0, 0 },
		{ 0x3208, 0x10, 0 }, { 0x3208, 0xa0, 0 },
	};

	if (ctrl->id == V4L2_CID_TEST_PATTERN)
		return ovx9100_set_pattern(ctrl);
	if ((ctrl->id != V4L2_CID_EXPOSURE &&
	     ctrl->id != V4L2_CID_ANALOGUE_GAIN) || !experimental_stream ||
	    !module_variant || strcmp(module_variant, "ofilm"))
		return -EINVAL;
	if (!pm_runtime_get_if_in_use(dev))
		return 0;
	ret = ovx9100_write_regs(sensor, regs, ARRAY_SIZE(regs));
	if (ret) {
		/* Best effort: never leave group hold active after a failed write. */
		cci_write(sensor->regmap, CCI_REG8(0x3208), 0x10, NULL);
		cci_write(sensor->regmap, CCI_REG8(0x3208), 0xa0, NULL);
	}
	if (!ret && stream_diagnostics) {
		/* Group commands are not persistent register values. */
		for (i = 1; i < ARRAY_SIZE(regs) - 2; i++) {
			read_ret = cci_read(sensor->regmap, CCI_REG8(regs[i].address),
					    &value, NULL);
			dev_info(dev, "exposure-readback: reg%04x requested%02x read%02llx rc%d\n",
				 regs[i].address, regs[i].value, read_ret ? 0 : value, read_ret);
		}
	}
	pm_runtime_put_autosuspend(dev);
	return ret;
}

static const struct v4l2_ctrl_ops ovx9100_ctrl_ops = {
	.s_ctrl = ovx9100_s_ctrl,
};

static int ovx9100_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct ovx9100 *sensor = to_ovx9100(sd);
	struct device *dev = sd->dev;
	struct v4l2_subdev_state *state;
	const struct ovx9100_reg *init;
	unsigned int count;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	if (sensor->streaming == !!enable)
		goto unlock;
	if (!enable) {
		ovx9100_stream_snapshot(sensor, "before-off");
		ret = ovx9100_write_regs(sensor, ovx9100_stream_off,
					 ARRAY_SIZE(ovx9100_stream_off));
		sensor->streaming = false;
		pm_runtime_put_sync_suspend(dev);
		goto unlock;
	}
	if (!experimental_stream || !symbol_rate || image_vc < 0 || image_vc > 1 ||
	    !module_variant) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}
	if ((stock_still50 || stock_video60) && strcmp(module_variant, "ofilm")) {
		ret = -EINVAL;
		goto unlock;
	}
	if (!strcmp(module_variant, "ofilm")) {
		init = ovx9100_init_ofilm;
		count = ARRAY_SIZE(ovx9100_init_ofilm);
	} else if (!strcmp(module_variant, "sunny")) {
		init = ovx9100_init_sunny;
		count = ARRAY_SIZE(ovx9100_init_sunny);
	} else {
		ret = -EINVAL;
		goto unlock;
	}
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto unlock;
	ret = ovx9100_write_regs(sensor, init, count);
	if (ret)
		goto power_down;
	ovx9100_transport_readback(sensor, init, count, "init");
	if (stock_still50 && !stock_video60 && still50_calibration) {
		ret = ovx9100_apply_correction(sensor);
		if (ret) {
			dev_err(dev, "module-correction: failed rc%d\n", ret);
			goto power_down;
		}
	}
	/* Android high-resolution starts use the initialization alone. */
	if (!stock_still50 || stock_video60) {
		const struct ovx9100_reg *mode = stock_video60 ?
			ovx9100_mode25 : ovx9100_mode1;
		unsigned int mode_count = stock_video60 ?
			ARRAY_SIZE(ovx9100_mode25) : ARRAY_SIZE(ovx9100_mode1);

		ret = ovx9100_write_regs(sensor, mode, mode_count);
		if (ret)
			goto power_down;
		ovx9100_transport_readback(sensor, mode, mode_count, "mode");
	}
	ret = __v4l2_ctrl_handler_setup(&sensor->ctrls);
	if (ret)
		goto power_down;
	ret = ovx9100_write_regs(sensor, ovx9100_stream_on,
				 ARRAY_SIZE(ovx9100_stream_on));
	if (ret)
		goto power_down;
	sensor->streaming = true;
	ovx9100_stream_snapshot(sensor, "after-on");
	goto unlock;
power_down:
	ovx9100_write_regs(sensor, ovx9100_stream_off,
			   ARRAY_SIZE(ovx9100_stream_off));
	pm_runtime_put_sync_suspend(dev);
unlock:
	v4l2_subdev_unlock_state(state);
	return ret;
}

static void ovx9100_fill_format(struct v4l2_mbus_framefmt *fmt)
{
	*fmt = (struct v4l2_mbus_framefmt){
		.width = stock_still50 ? 8192 : OVX9100_WIDTH,
		.height = stock_still50 ? 6144 : OVX9100_HEIGHT,
		.code = OVX9100_CODE,
		.field = V4L2_FIELD_NONE,
		.colorspace = V4L2_COLORSPACE_RAW,
		.quantization = V4L2_QUANTIZATION_FULL_RANGE,
		.xfer_func = V4L2_XFER_FUNC_NONE,
	};
}

static int ovx9100_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	struct v4l2_fract *interval;

	ovx9100_fill_format(v4l2_subdev_state_get_format(state, 0));
	interval = v4l2_subdev_state_get_interval(state, 0);
	*interval = (struct v4l2_fract){ .numerator = 100000,
					 .denominator = stock_video60 ? 6000000 : 3004086 };
	return 0;
}

static int ovx9100_enum_code(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index)
		return -EINVAL;
	code->code = OVX9100_CODE;
	return 0;
}

static int ovx9100_enum_size(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_frame_size_enum *size)
{
	if (size->pad || size->index || size->code != OVX9100_CODE)
		return -EINVAL;
	size->min_width = size->max_width = stock_still50 ? 8192 : OVX9100_WIDTH;
	size->min_height = size->max_height = stock_still50 ? 6144 : OVX9100_HEIGHT;
	return 0;
}

static int ovx9100_set_format(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_format *format)
{
	if (format->pad)
		return -EINVAL;
	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    to_ovx9100(sd)->streaming)
		return -EBUSY;
	ovx9100_fill_format(&format->format);
	*v4l2_subdev_state_get_format(state, 0) = format->format;
	return 0;
}

static int ovx9100_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
				  struct v4l2_mbus_frame_desc *fd)
{
	if (pad)
		return -EINVAL;
	memset(fd, 0, sizeof(*fd));
	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;
	fd->num_entries = 1;
	fd->entry[0].pixelcode = OVX9100_CODE;
	if (image_vc < 0 || image_vc > 1)
		return -EOPNOTSUPP;
	fd->entry[0].bus.csi2.vc = image_vc;
	fd->entry[0].bus.csi2.dt = 0x2b;
	return 0;
}

static const struct v4l2_subdev_video_ops ovx9100_video_ops = {
	.s_stream = ovx9100_set_stream,
};
static const struct v4l2_subdev_pad_ops ovx9100_pad_ops = {
	.enum_mbus_code = ovx9100_enum_code,
	.enum_frame_size = ovx9100_enum_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = ovx9100_set_format,
	.get_frame_interval = v4l2_subdev_get_frame_interval,
	.get_frame_desc = ovx9100_get_frame_desc,
};
static const struct v4l2_subdev_ops ovx9100_ops = {
	.video = &ovx9100_video_ops,
	.pad = &ovx9100_pad_ops,
};
static const struct v4l2_subdev_internal_ops ovx9100_internal_ops = {
	.init_state = ovx9100_init_state,
};

static int ovx9100_check_endpoint(struct device *dev)
{
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_CSI2_CPHY };
	struct fwnode_handle *fwnode;
	int ret;

	fwnode = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!fwnode)
		return -EINVAL;
	ret = v4l2_fwnode_endpoint_alloc_parse(fwnode, &ep);
	fwnode_handle_put(fwnode);
	if (!ret && (ep.bus_type != V4L2_MBUS_CSI2_CPHY ||
		     ep.bus.mipi_csi2.num_data_lanes != 3))
		ret = -EINVAL;
	v4l2_fwnode_endpoint_free(&ep);
	return ret;
}

static int ovx9100_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct v4l2_fwnode_device_properties props;
	struct ovx9100 *sensor;
	struct v4l2_ctrl *ctrl;
	u64 id;
	unsigned int i;
	int ret;

	if (probe_diagnostics)
		dev_info(dev, "probe-diag: endpoint begin\n");
	ret = ovx9100_check_endpoint(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Expected three C-PHY trios\n");
	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	v4l2_i2c_subdev_init(&sensor->sd, client, &ovx9100_ops);
	if (probe_diagnostics)
		dev_info(dev, "probe-diag: regmap begin\n");
	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return PTR_ERR(sensor->regmap);
	if (probe_diagnostics)
		dev_info(dev, "probe-diag: xclk get begin\n");
	sensor->xclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(sensor->xclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xclk),
				     "Cannot get xclk\n");
	if (clk_get_rate(sensor->xclk) != OVX9100_XCLK_FREQ)
		return dev_err_probe(dev, -EINVAL, "Expected 19.2 MHz xclk\n");
	for (i = 0; i < ARRAY_SIZE(sensor->supplies); i++) {
		sensor->supplies[i].supply = ovx9100_supply_names[i];
		sensor->supplies[i].init_load_uA = ovx9100_supply_loads[i];
	}
	if (probe_diagnostics)
		dev_info(dev, "probe-diag: supplies get begin\n");
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(sensor->supplies),
				      sensor->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot get supplies\n");
	if (probe_diagnostics)
		dev_info(dev, "probe-diag: reset get begin\n");
	sensor->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset))
		return dev_err_probe(dev, PTR_ERR(sensor->reset),
				     "Cannot get reset\n");

	ret = ovx9100_power_on(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot power on\n");
	if (probe_diagnostics)
		dev_info(dev, "probe-diag: id read begin\n");
	ret = cci_read(sensor->regmap, OVX9100_CHIP_ID_REG, &id, NULL);
	if (probe_diagnostics)
		dev_info(dev, "probe-diag: id read rc %d id %04llx\n",
			 ret, ret ? 0 : id);
	ovx9100_power_off(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot read sensor ID\n");
	if (id != OVX9100_CHIP_ID)
		return dev_err_probe(dev, -ENODEV,
				     "Unexpected sensor ID 0x%04llx\n", id);

	if (probe_diagnostics)
		dev_info(dev, "probe-diag: validated sensor ID %04llx\n", id);
	ret = v4l2_ctrl_handler_init(&sensor->ctrls, 6);
	if (ret)
		return ret;
	if (symbol_rate) {
		sensor->link_freqs[0] = symbol_rate;
		ctrl = v4l2_ctrl_new_int_menu(&sensor->ctrls, NULL,
					      V4L2_CID_LINK_FREQ, 0, 0,
					      sensor->link_freqs);
		if (ctrl)
			ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	}
	/* Pixel-array clock/blanking units still need characterization. */
	ret = v4l2_fwnode_device_parse(dev, &props);
	if (!ret)
		ret = v4l2_ctrl_new_fwnode_properties(&sensor->ctrls, NULL,
						      &props);
	if (!ret)
		ret = sensor->ctrls.error;
	if (ret)
		goto free_ctrls;
	if (experimental_stream && module_variant &&
	    !strcmp(module_variant, "ofilm")) {
		/* Ofilm BIN verticalOffset38; fixed mode1 VTS2774. */
		sensor->exposure = v4l2_ctrl_new_std(&sensor->ctrls, &ovx9100_ctrl_ops,
						     V4L2_CID_EXPOSURE, 4, OVX9100_EXPOSURE_MAX, 1,
						     stock_video60 ? 1280 : 2560);
		sensor->again = v4l2_ctrl_new_std(&sensor->ctrls, &ovx9100_ctrl_ops,
						  V4L2_CID_ANALOGUE_GAIN, 256, 16320, 2, 256);
		v4l2_ctrl_new_std_menu_items(&sensor->ctrls, &ovx9100_ctrl_ops,
					     V4L2_CID_TEST_PATTERN, 1, 0, 0,
					     ovx9100_pattern_menu);
		if (sensor->ctrls.error) {
			ret = sensor->ctrls.error;
			goto free_ctrls;
		}
	}
	sensor->sd.ctrl_handler = &sensor->ctrls;
	sensor->sd.internal_ops = &ovx9100_internal_ops;
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

static void ovx9100_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ovx9100 *sensor = to_ovx9100(sd);

	v4l2_async_unregister_subdev(sd);
	if (sensor->streaming)
		ovx9100_set_stream(sd, 0);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		ovx9100_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrls);
}

static const struct dev_pm_ops ovx9100_pm_ops = { SET_RUNTIME_PM_OPS(
	ovx9100_power_off, ovx9100_power_on, NULL) };
static const struct of_device_id ovx9100_of_match[] = {
	{ .compatible = "ovti,ovx9100" },
	{}
};
MODULE_DEVICE_TABLE(of, ovx9100_of_match);

static struct i2c_driver ovx9100_driver = {
	.driver = {
		.name = "ovx9100",
		.of_match_table = ovx9100_of_match,
		.pm = &ovx9100_pm_ops,
	},
	.probe = ovx9100_probe,
	.remove = ovx9100_remove,
};
module_i2c_driver(ovx9100_driver);

MODULE_DESCRIPTION("OmniVision OVX9100 experimental probe and fixed RAW10 mode");
MODULE_LICENSE("GPL");
