// SPDX-License-Identifier: GPL-2.0-only
/* Xiaomi dada lens bring-up; module-specific calibration is not applied yet. */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

static bool eeprom_diagnostics;
module_param(eeprom_diagnostics, bool, 0644);
MODULE_PARM_DESC(eeprom_diagnostics, "Read only the module and AF EEPROM header after power-up");

static bool eeprom_calibration;
module_param(eeprom_calibration, bool, 0644);
MODULE_PARM_DESC(eeprom_calibration, "Restrict focus to checked module AF endpoints");

struct dada_vcm_chip {
	u16 max_position;
	u16 initial_position;
	u16 park_position;
	u8 position_reg;
	u8 shift;
	bool inverted;
};

static const struct dada_vcm_chip dada_ak7316 = {
	.max_position = 4095,
	.initial_position = 2016,
	.park_position = 100,
	.position_reg = 0x00,
	.shift = 4,
};

static const struct dada_vcm_chip dada_gt9764v = {
	.max_position = 2047,
	.initial_position = 1024,
	.park_position = 1024,
	.position_reg = 0x03,
	.inverted = true,
};

struct dada_vcm {
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *focus;
	const struct dada_vcm_chip *chip;
	u16 position;
	bool eeprom_observed;
	bool calibrated;
	u16 infinity;
	u16 macro;
};

static struct dada_vcm *to_dada_vcm(struct v4l2_subdev *sd)
{
	return container_of(sd, struct dada_vcm, sd);
}

static int dada_vcm_read(struct dada_vcm *vcm, u8 reg, unsigned int size)
{
	struct i2c_client *client = v4l2_get_subdevdata(&vcm->sd);
	u8 value[2];
	struct i2c_msg messages[] = {
		{ .addr = client->addr, .len = 1, .buf = &reg },
		{ .addr = client->addr, .flags = I2C_M_RD,
		  .len = size, .buf = value },
	};
	int ret;

	ret = i2c_transfer(client->adapter, messages, ARRAY_SIZE(messages));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(messages))
		return -EIO;
	return size == 2 ? value[0] << 8 | value[1] : value[0];
}

static int dada_vcm_write(struct dada_vcm *vcm, u8 reg, u16 value,
			  unsigned int size)
{
	struct i2c_client *client = v4l2_get_subdevdata(&vcm->sd);
	u8 buffer[] = { reg, value >> 8, value & 0xff };
	int ret;

	if (size == 1)
		buffer[1] = value;
	ret = i2c_master_send(client, buffer, size + 1);
	if (ret < 0)
		return ret;
	return ret == size + 1 ? 0 : -EIO;
}

static int dada_vcm_set_position(struct dada_vcm *vcm, u16 position)
{
	const struct dada_vcm_chip *chip = vcm->chip;
	u16 value = chip->inverted ? chip->max_position - position : position;
	int ret;

	value <<= chip->shift;
	ret = dada_vcm_write(vcm, chip->position_reg, value, 2);
	if (!ret)
		vcm->position = position;
	return ret;
}

static int dada_vcm_move(struct dada_vcm *vcm, u16 target)
{
	int position = vcm->position;
	int ret;

	/* Bound each movement, including close/restore, and stop on I2C failure. */
	while (position != target) {
		position += clamp_t(int, target - position, -64, 64);
		ret = dada_vcm_set_position(vcm, position);
		if (ret)
			return ret;
		fsleep(10000);
	}
	return 0;
}

static int dada_vcm_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct dada_vcm *vcm = container_of(ctrl->handler, struct dada_vcm, ctrls);
	struct device *dev = vcm->sd.dev;
	int ret;

	if (ctrl->id != V4L2_CID_FOCUS_ABSOLUTE)
		return -EINVAL;
	if (!pm_runtime_get_if_in_use(dev))
		return 0;
	ret = dada_vcm_move(vcm, ctrl->val);
	if (!ret) {
		int value = dada_vcm_read(vcm, vcm->chip->position_reg, 2);
		u16 expected = vcm->chip->inverted ?
			vcm->chip->max_position - ctrl->val : ctrl->val;

		expected <<= vcm->chip->shift;
		dev_info(dev, "focus-readback: logical%d expected%04x read%04x\n",
			 ctrl->val, expected, value);
		if (value < 0)
			ret = value;
		else if (value != expected)
			ret = -EIO;
	}
	pm_runtime_put(dev);
	return ret;
}

static const struct v4l2_ctrl_ops dada_vcm_ctrl_ops = {
	.s_ctrl = dada_vcm_set_ctrl,
};

static int dada_vcm_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct dada_vcm *vcm = to_dada_vcm(sd);
	int ret;

	ret = pm_runtime_resume_and_get(sd->dev);
	if (ret < 0)
		return ret;
	if (vcm->calibrated) {
		ret = v4l2_ctrl_modify_range(vcm->focus, vcm->infinity,
					     vcm->macro, 1,
					     vcm->chip->initial_position);
		if (ret < 0)
			pm_runtime_put_sync_suspend(sd->dev);
	}
	return ret < 0 ? ret : 0;
}

static int dada_vcm_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	int ret = pm_runtime_put_sync_suspend(sd->dev);

	return ret < 0 ? ret : 0;
}

static const struct v4l2_subdev_internal_ops dada_vcm_internal_ops = {
	.open = dada_vcm_open,
	.close = dada_vcm_close,
};

static const struct v4l2_subdev_ops dada_vcm_ops;

static int dada_vcm_observe_eeprom(struct dada_vcm *vcm)
{
	struct i2c_client *client = v4l2_get_subdevdata(&vcm->sd);
	u16 address = vcm->chip == &dada_ak7316 ? 0x51 : 0x54;
	u8 data[0x44];
	unsigned int offset, sum = 0;

	if (!eeprom_diagnostics && !eeprom_calibration)
		return 0;
	if (vcm->eeprom_observed)
		return eeprom_calibration && !vcm->calibrated ? -ENODATA : 0;
	vcm->eeprom_observed = true;
	/* CCI permits at most 12 bytes per read. Only set the address pointer. */
	for (offset = 0; offset < sizeof(data); offset += 12) {
		u8 pointer[] = { offset >> 8, offset & 0xff };
		struct i2c_msg messages[] = {
			{ .addr = address, .len = sizeof(pointer), .buf = pointer },
			{ .addr = address, .flags = I2C_M_RD,
			  .len = min_t(unsigned int, 12, sizeof(data) - offset),
			  .buf = data + offset },
		};
		int ret;

		ret = i2c_transfer(client->adapter, messages, ARRAY_SIZE(messages));
		if (ret != ARRAY_SIZE(messages)) {
			dev_warn(&client->dev, "eeprom-read: addr%02x offset%u error%d\n",
				 address, offset, ret < 0 ? ret : -EIO);
			return eeprom_calibration ? (ret < 0 ? ret : -EIO) : 0;
		}
	}
	/* Keep each hex record below the printk pointer-format length limit. */
	for (offset = 0; offset < sizeof(data); offset += 12) {
		unsigned int count = min_t(unsigned int, 12, sizeof(data) - offset);

		dev_info(&client->dev, "eeprom-header: addr%02x offset%u bytes%u data%*phN\n",
			 address, offset, count, count, data + offset);
	}
	if (!eeprom_calibration)
		return 0;
	for (offset = 1; offset <= 0x23; offset++)
		sum += data[offset];
	if (data[0] != 1 || data[0x24] != sum % 255 + 1)
		return -EBADMSG;
	sum = 0;
	for (offset = 0x28; offset <= 0x42; offset++)
		sum += data[offset];
	if (data[0x27] != 1 || data[0x43] != sum % 255 + 1)
		return -EBADMSG;
	vcm->macro = data[0x2d] << 8 | data[0x2e];
	vcm->infinity = data[0x31] << 8 | data[0x32];
	/* Reject signed-negative, reversed, empty or out-of-domain endpoints. */
	if (vcm->infinity >= vcm->macro || vcm->macro > vcm->chip->max_position ||
	    vcm->chip->initial_position < vcm->infinity ||
	    vcm->chip->initial_position > vcm->macro)
		return -ERANGE;
	vcm->calibrated = true;
	dev_info(&client->dev, "focus-calibration: infinity%u macro%u checked=1\n",
		 vcm->infinity, vcm->macro);
	return 0;
}

static int dada_vcm_resume(struct device *dev)
{
	struct dada_vcm *vcm = to_dada_vcm(dev_get_drvdata(dev));
	int ret, value;

	/* The runtime PM supplier has enabled shared rails/reset/MCLK first. */
	fsleep(10000);
	ret = dada_vcm_write(vcm, 0x02, 0x00, 1);
	if (ret)
		return ret;
	if (vcm->chip == &dada_ak7316) {
		fsleep(1900);
		ret = read_poll_timeout(dada_vcm_read, value,
					value <= 0, 100, 5000, false, vcm, 0x02, 1);
		if (ret)
			return ret;
		if (value < 0)
			return value;
		ret = dada_vcm_set_position(vcm, 2016); /* Stock 0x7e00. */
		if (ret)
			return ret;
	} else {
		fsleep(5000);
		ret = dada_vcm_write(vcm, 0x0b, 0x23, 1);
		if (ret)
			return ret;
		ret = dada_vcm_write(vcm, 0x02, 0x02, 1);
		if (ret)
			return ret;
		fsleep(1000);
		ret = dada_vcm_write(vcm, 0x06, 0x40, 1);
		if (ret)
			return ret;
		ret = dada_vcm_write(vcm, 0x07, 0x59, 1);
		if (ret)
			return ret;
		fsleep(1000);
		value = dada_vcm_read(vcm, 0x03, 2);
		if (value < 0)
			return value;
		if (value > vcm->chip->max_position)
			return -ERANGE;
		vcm->position = vcm->chip->max_position - value;
	}
	ret = dada_vcm_move(vcm, vcm->focus->val);
	if (!ret)
		ret = dada_vcm_observe_eeprom(vcm);
	return ret;
}

static int dada_vcm_suspend(struct device *dev)
{
	struct dada_vcm *vcm = to_dada_vcm(dev_get_drvdata(dev));
	int ret;

	/* AK7316 stock parking has gravity branches; use its downward endpoint. */
	ret = dada_vcm_move(vcm, vcm->chip->park_position);
	if (ret)
		return ret;
	dev_info(dev, "focus-park: logical%u; supplier may suspend\n", vcm->position);
	return 0;
}

static int dada_vcm_link_sensor(struct device *dev)
{
	struct device_node *node;
	struct i2c_client *sensor;
	struct device_link *link = NULL;

	node = of_parse_phandle(dev->of_node, "xiaomi,power-sensor", 0);
	if (!node)
		return -EINVAL;
	sensor = of_find_i2c_device_by_node(node);
	of_node_put(node);
	if (!sensor)
		return -EPROBE_DEFER;
	device_lock(&sensor->dev);
	if (device_is_bound(&sensor->dev))
		link = device_link_add(dev, &sensor->dev,
				       DL_FLAG_PM_RUNTIME | DL_FLAG_AUTOREMOVE_CONSUMER);
	device_unlock(&sensor->dev);
	put_device(&sensor->dev);
	return link ? 0 : -EPROBE_DEFER;
}

static int dada_vcm_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct dada_vcm *vcm;
	int ret;

	vcm = devm_kzalloc(dev, sizeof(*vcm), GFP_KERNEL);
	if (!vcm)
		return -ENOMEM;
	vcm->chip = device_get_match_data(dev);
	v4l2_i2c_subdev_init(&vcm->sd, client, &dada_vcm_ops);
	vcm->sd.internal_ops = &dada_vcm_internal_ops;
	vcm->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	vcm->sd.entity.function = MEDIA_ENT_F_LENS;
	v4l2_ctrl_handler_init(&vcm->ctrls, 1);
	vcm->focus = v4l2_ctrl_new_std(&vcm->ctrls, &dada_vcm_ctrl_ops,
				       V4L2_CID_FOCUS_ABSOLUTE, 0,
				       vcm->chip->max_position, 1,
				       vcm->chip->initial_position);
	ret = vcm->ctrls.error;
	if (ret)
		goto free_ctrls;
	vcm->sd.ctrl_handler = &vcm->ctrls;
	ret = media_entity_pads_init(&vcm->sd.entity, 0, NULL);
	if (ret)
		goto free_ctrls;
	ret = dada_vcm_link_sensor(dev);
	if (ret)
		goto cleanup_entity;
	pm_runtime_set_suspended(dev);
	pm_runtime_enable(dev);
	ret = v4l2_async_register_subdev(&vcm->sd);
	if (ret) {
		pm_runtime_disable(dev);
		goto cleanup_entity;
	}
	return 0;

cleanup_entity:
	media_entity_cleanup(&vcm->sd.entity);
free_ctrls:
	v4l2_ctrl_handler_free(&vcm->ctrls);
	return ret;
}

static void dada_vcm_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct dada_vcm *vcm = to_dada_vcm(sd);

	v4l2_async_unregister_subdev(sd);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		dada_vcm_suspend(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&vcm->ctrls);
}

static const struct of_device_id dada_vcm_of_match[] = {
	{ .compatible = "xiaomi,dada-ak7316", .data = &dada_ak7316 },
	{ .compatible = "xiaomi,dada-gt9764v", .data = &dada_gt9764v },
	{}
};
MODULE_DEVICE_TABLE(of, dada_vcm_of_match);

static const struct dev_pm_ops dada_vcm_pm_ops = {
	SET_RUNTIME_PM_OPS(dada_vcm_suspend, dada_vcm_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
};

static struct i2c_driver dada_vcm_driver = {
	.driver = {
		.name = "dada-vcm",
		.of_match_table = dada_vcm_of_match,
		.pm = &dada_vcm_pm_ops,
	},
	.probe = dada_vcm_probe,
	.remove = dada_vcm_remove,
};
module_i2c_driver(dada_vcm_driver);

MODULE_DESCRIPTION("Xiaomi dada AK7316 and GT9764V experimental lens driver");
MODULE_AUTHOR("7Night-kqx");
MODULE_LICENSE("GPL");
