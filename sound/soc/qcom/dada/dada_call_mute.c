// SPDX-License-Identifier: GPL-2.0
/* Expose the existing TX DEC0 PGA mute bit without changing routes or clocks.
 * A permanent add-on: controls reference this module's mixer descriptor.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/soc.h>

#define DADA_TX_PATH_CTL 0x0400
#define DADA_TX_PGA_MUTE_SHIFT 4

static const struct snd_kcontrol_new dada_mute_control =
	SOC_SINGLE("Dada Call Mic Mute", DADA_TX_PATH_CTL,
		   DADA_TX_PGA_MUTE_SHIFT, 1, 0);

static int dada_find_tx(struct device *dev, const void *unused)
{
	return of_device_is_compatible(dev->of_node, "qcom,sm8750-lpass-tx-macro");
}

static int __init dada_mute_init(void)
{
	struct snd_soc_component *component;
	struct device *dev;
	int ret;

	dev = bus_find_device(&platform_bus_type, NULL, NULL, dada_find_tx);
	if (!dev)
		return -ENODEV;
	component = snd_soc_lookup_component(dev, "TX-MACRO");
	if (!component || !component->card ||
	    strcmp(component->card->name, "Xiaomi-dada")) {
		ret = -ENODEV;
		goto put;
	}
	ret = snd_soc_add_component_controls(component, &dada_mute_control, 1);
put:
	put_device(dev);
	return ret;
}
module_init(dada_mute_init);
/* No exit: this control stays valid until shutdown, like retained audio. */
MODULE_DESCRIPTION("Xiaomi dada TX DEC0 microphone PGA mute control");
MODULE_AUTHOR("dada Linux port");
MODULE_LICENSE("GPL");
MODULE_VERSION("20261105-1");
