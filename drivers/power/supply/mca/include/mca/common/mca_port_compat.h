/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Legacy GPIO compatibility for the MCA port.
 * Lookup takes an exact OF property name (including vendor bare names) and
 * never requests a line. The request wrapper owns a descriptor until devres
 * releases it, and propagates contention and allocation errors.
 */
#ifndef __MCA_PORT_COMPAT_H__
#define __MCA_PORT_COMPAT_H__

#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/gpio/legacy.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/of.h>

/* Query an exact property without taking GPIO ownership. */
static inline int of_get_named_gpio(struct device_node *np, const char *propname,
				    int index)
{
	struct of_phandle_args gpiospec;
	struct gpio_device *gdev;
	struct gpio_chip *chip;
	struct gpio_desc *desc;
	int ret, hwnum;

	if (!np || !propname)
		return -EINVAL;

	ret = of_parse_phandle_with_args_map(np, propname, "gpio", index,
					     &gpiospec);
	if (ret) {
		/* 属性存在却解析不了才报，避免给「本来就没这个可选属性」刷屏 */
		if (of_find_property(np, propname, NULL))
			pr_warn("mca: gpio prop '%s' of %pOF idx %d parse failed, ret=%d\n",
				propname, np, index, ret);
		return ret;
	}

	gdev = gpio_device_find_by_fwnode(of_fwnode_handle(gpiospec.np));
	if (!gdev) {
		pr_warn("mca: gpio prop '%s' of %pOF idx %d: no gpio_device for %pOF (defer)\n",
			propname, np, index, gpiospec.np);
		of_node_put(gpiospec.np);
		return -EPROBE_DEFER;
	}

	chip = gpio_device_get_chip(gdev);
	if (!chip || !chip->of_xlate ||
	    chip->of_gpio_n_cells != gpiospec.args_count) {
		pr_warn("mca: gpio prop '%s' of %pOF idx %d: chip xlate mismatch (cells=%d args=%d)\n",
			propname, np, index,
			chip ? chip->of_gpio_n_cells : -1, gpiospec.args_count);
		gpio_device_put(gdev);
		of_node_put(gpiospec.np);
		return -EINVAL;
	}

	hwnum = chip->of_xlate(chip, &gpiospec, NULL);
	if (hwnum < 0) {
		gpio_device_put(gdev);
		of_node_put(gpiospec.np);
		return hwnum;
	}

	desc = gpio_device_get_desc(gdev, hwnum);
	gpio_device_put(gdev);
	of_node_put(gpiospec.np);
	if (IS_ERR(desc))
		return (int)PTR_ERR(desc);

	return desc_to_gpio(desc);
}

static inline void mca_gpio_release(void *data)
{
	gpiod_put(data);
}

static inline int devm_gpio_request(struct device *dev, unsigned gpio,
				    const char *label)
{
	struct gpio_desc *desc = gpio_to_desc(gpio);
	int ret;

	if (!desc)
		return -EINVAL;
	ret = gpio_request(gpio, label);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, mca_gpio_release, desc);
}

#endif /* __MCA_PORT_COMPAT_H__ */
