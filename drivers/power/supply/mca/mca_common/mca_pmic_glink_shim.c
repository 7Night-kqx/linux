// SPDX-License-Identifier: GPL-2.0-only
/* MCA owns the actual pmic_glink.power-supply auxiliary device.
 * Callbacks are atomic: MCA copies notifications to its sleepable event work.
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/soc/qcom/pdr.h>
#include <linux/soc/qcom/pmic_glink.h>
#include <linux/soc/qcom/qti_pmic_glink.h>

struct mca_glink_shim {
	int (*msg_cb)(void *priv, void *data, size_t len);
	void (*state_cb)(void *priv, enum pmic_glink_state state);
	void *priv;
};

static void mca_shim_msg(const void *data, size_t len, void *priv)
{
	struct mca_glink_shim *s = priv;

	if (s->msg_cb)
		s->msg_cb(s->priv, (void *)data, len);
}

static void mca_shim_pdr(void *priv, int state)
{
	struct mca_glink_shim *s = priv;

	if (s->state_cb)
		s->state_cb(s->priv, state == SERVREG_SERVICE_STATE_UP ?
			PMIC_GLINK_STATE_UP : PMIC_GLINK_STATE_DOWN);
}

struct pmic_glink_client *pmic_glink_register_client(struct device *dev,
		const struct pmic_glink_client_data *data)
{
	struct mca_glink_shim *s;
	struct pmic_glink_client *client;

	if (!data || !data->msg_cb)
		return ERR_PTR(-EINVAL);
	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return ERR_PTR(-ENOMEM);
	s->msg_cb = data->msg_cb;
	s->state_cb = data->state_cb;
	s->priv = data->priv;
	client = devm_pmic_glink_client_alloc(dev, data->id,
			mca_shim_msg, mca_shim_pdr, s);
	if (!IS_ERR(client))
		pmic_glink_client_register(client);
	return client;
}
EXPORT_SYMBOL_GPL(pmic_glink_register_client);

int pmic_glink_unregister_client(struct pmic_glink_client *client)
{
	pmic_glink_client_unregister(client);
	return 0;
}
EXPORT_SYMBOL_GPL(pmic_glink_unregister_client);

int pmic_glink_write(struct pmic_glink_client *client, void *data, size_t len)
{
	return pmic_glink_send(client, data, len);
}
EXPORT_SYMBOL_GPL(pmic_glink_write);

MODULE_DESCRIPTION("MCA pmic glink shim");
MODULE_LICENSE("GPL");
