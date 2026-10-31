// SPDX-License-Identifier: GPL-2.0-only
#include <linux/auxiliary_bus.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>
#include <linux/soc/qcom/qti_pmic_glink.h>
#include <linux/soc/qcom/pmic_glink.h>
#include <mca/common/mca_adsp_glink.h>

#define MCA_ADSP_GLINK_OWNER 0x800A
#define MCA_ADSP_GLINK_QBG_OWNER 0x8009
#define MCA_ADSP_GLINK_MSG_REQ 1
#define MCA_ADSP_GLINK_MSG_NOTIFY 2
#define MCA_ADSP_GLINK_OPCODE_READ 1
#define MCA_ADSP_GLINK_OPCODE_WRITE 2
#define MCA_ADSP_PROP_DATA_LEN 256
#define MCA_ADSP_GLINK_TIMEOUT_MS 250
#define MCA_ADSP_EVENT_MAX 64

struct mca_adsp_glink_req_msg {
	struct pmic_glink_hdr hdr;
	__le32 property_id, seq;
	u8 data[MCA_ADSP_PROP_DATA_LEN];
} __packed;

struct mca_adsp_glink_resp_msg {
	struct pmic_glink_hdr hdr;
	__le32 property_id, retcode, seq;
	u8 data[MCA_ADSP_PROP_DATA_LEN];
} __packed;

struct mca_adsp_glink_notify_msg {
	struct pmic_glink_hdr hdr;
	__le32 property_id;
	u8 data[MCA_ADSP_PROP_DATA_LEN];
} __packed;

struct mca_adsp_event {
	struct list_head node;
	bool state_event;
	enum pmic_glink_state state;
	struct mca_adsp_glink_notify_msg msg;
};

struct mca_adsp_glink_dev {
	struct device *dev;
	struct pmic_glink_client *client, *qbg_client;
	struct mutex rw_lock;
	/* IRQ callbacks only touch the request and the bounded event queue. */
	spinlock_t lock;
	struct completion ack;
	int glink_state;
	bool pending, stopping, ready;
	u32 cur_property_id, owner, opcode, seq;
	int retcode;
	u8 read_buf[MCA_ADSP_PROP_DATA_LEN];
	struct work_struct event_work;
	struct workqueue_struct *wq;
	struct list_head events;
	unsigned int event_count;
};

struct mca_adsp_ops_node {
	struct list_head node;
	void *priv;
	struct mca_adsp_glink_ops *ops;
};

static struct mca_adsp_glink_dev __rcu *g_mca_adsp_glink;
DEFINE_STATIC_SRCU(mca_device_srcu);
DEFINE_STATIC_SRCU(mca_ops_srcu);
static DEFINE_MUTEX(mca_ops_lock);
static DEFINE_MUTEX(mca_device_lock);
static LIST_HEAD(g_mca_adsp_glink_ops_head);
static LIST_HEAD(g_mca_adsp_glink_qbg_ops_head);

static int mca_adsp_glink_transfer(u32 owner, u32 opcode, int prop_id,
				 void *value, size_t size)
{
	struct mca_adsp_glink_req_msg msg = { 0 };
	struct mca_adsp_glink_dev *mca;
	unsigned long flags;
	int idx, ret;

	if (!value || !size || size > MCA_ADSP_PROP_DATA_LEN || prop_id < 0)
		return -EINVAL;
	idx = srcu_read_lock(&mca_device_srcu);
	mca = srcu_dereference(g_mca_adsp_glink, &mca_device_srcu);
	if (!mca) {
		ret = -ENODEV;
		goto out_srcu;
	}
	mutex_lock(&mca->rw_lock);
	spin_lock_irqsave(&mca->lock, flags);
	if (mca->stopping || mca->glink_state != PMIC_GLINK_STATE_UP) {
		ret = -ENOTCONN;
		spin_unlock_irqrestore(&mca->lock, flags);
		goto out_mutex;
	}
	mca->seq++;
	msg.hdr.owner = cpu_to_le32(owner);
	msg.hdr.type = cpu_to_le32(MCA_ADSP_GLINK_MSG_REQ);
	msg.hdr.opcode = cpu_to_le32(opcode);
	msg.property_id = cpu_to_le32(prop_id);
	msg.seq = cpu_to_le32(mca->seq);
	if (opcode == MCA_ADSP_GLINK_OPCODE_WRITE)
		memcpy(msg.data, value, size);
	reinit_completion(&mca->ack);
	mca->pending = true;
	mca->retcode = -ETIMEDOUT;
	mca->cur_property_id = prop_id;
	mca->owner = owner;
	mca->opcode = opcode;
	spin_unlock_irqrestore(&mca->lock, flags);

	ret = pmic_glink_write(owner == MCA_ADSP_GLINK_OWNER ?
			       mca->client : mca->qbg_client, &msg, sizeof(msg));
	if (!ret && !wait_for_completion_timeout(&mca->ack,
				msecs_to_jiffies(MCA_ADSP_GLINK_TIMEOUT_MS)))
		ret = -ETIMEDOUT;
	spin_lock_irqsave(&mca->lock, flags);
	if (!ret) {
		ret = mca->retcode;
		if (!ret && opcode == MCA_ADSP_GLINK_OPCODE_READ)
			memcpy(value, mca->read_buf, size);
	}
	mca->pending = false;
	spin_unlock_irqrestore(&mca->lock, flags);
out_mutex:
	mutex_unlock(&mca->rw_lock);
out_srcu:
	srcu_read_unlock(&mca_device_srcu, idx);
	return ret;
}

int mca_adsp_glink_write_prop(int prop_id, void *value, size_t size)
{
	return mca_adsp_glink_transfer(MCA_ADSP_GLINK_OWNER,
			MCA_ADSP_GLINK_OPCODE_WRITE, prop_id, value, size);
}
EXPORT_SYMBOL(mca_adsp_glink_write_prop);
int mca_adsp_glink_read_prop(int prop_id, void *value, size_t size)
{
	return mca_adsp_glink_transfer(MCA_ADSP_GLINK_OWNER,
			MCA_ADSP_GLINK_OPCODE_READ, prop_id, value, size);
}
EXPORT_SYMBOL(mca_adsp_glink_read_prop);
int mca_adsp_glink_qbg_write_prop(int prop_id, void *value, size_t size)
{
	return mca_adsp_glink_transfer(MCA_ADSP_GLINK_QBG_OWNER,
			MCA_ADSP_GLINK_OPCODE_WRITE, prop_id, value, size);
}
EXPORT_SYMBOL(mca_adsp_glink_qbg_write_prop);
int mca_adsp_glink_qbg_read_prop(int prop_id, void *value, size_t size)
{
	return mca_adsp_glink_transfer(MCA_ADSP_GLINK_QBG_OWNER,
			MCA_ADSP_GLINK_OPCODE_READ, prop_id, value, size);
}
EXPORT_SYMBOL(mca_adsp_glink_qbg_read_prop);

static int mca_register_ops(struct list_head *head,
			    struct mca_adsp_glink_ops *ops, void *priv)
{
	struct mca_adsp_ops_node *n;

	if (!ops)
		return -EINVAL;
	n = kzalloc(sizeof(*n), GFP_KERNEL);
	if (!n)
		return -ENOMEM;
	n->ops = ops;
	n->priv = priv;
	mutex_lock(&mca_ops_lock);
	list_add_tail_rcu(&n->node, head);
	mutex_unlock(&mca_ops_lock);
	return 0;
}

int mca_adsp_glink_resister_ops(struct mca_adsp_glink_ops *ops, void *priv)
{
	return mca_register_ops(&g_mca_adsp_glink_ops_head, ops, priv);
}
EXPORT_SYMBOL(mca_adsp_glink_resister_ops);
int mca_adsp_glink_qbg_resister_ops(struct mca_adsp_glink_ops *ops, void *priv)
{
	return mca_register_ops(&g_mca_adsp_glink_qbg_ops_head, ops, priv);
}
EXPORT_SYMBOL(mca_adsp_glink_qbg_resister_ops);

/* Call outside callbacks; return only when no dispatch can reference priv. */
static void mca_unregister_ops(struct list_head *head,
			       struct mca_adsp_glink_ops *ops, void *priv)
{
	struct mca_adsp_ops_node *n, *found = NULL;

	mutex_lock(&mca_ops_lock);
	list_for_each_entry(n, head, node) {
		if (n->ops == ops && n->priv == priv) {
			list_del_rcu(&n->node);
			found = n;
			break;
		}
	}
	mutex_unlock(&mca_ops_lock);
	if (found) {
		synchronize_srcu(&mca_ops_srcu);
		kfree(found);
	}
}
void mca_adsp_glink_unregister_ops(struct mca_adsp_glink_ops *ops, void *priv)
{
	mca_unregister_ops(&g_mca_adsp_glink_ops_head, ops, priv);
}
EXPORT_SYMBOL(mca_adsp_glink_unregister_ops);
void mca_adsp_glink_qbg_unregister_ops(struct mca_adsp_glink_ops *ops, void *priv)
{
	mca_unregister_ops(&g_mca_adsp_glink_qbg_ops_head, ops, priv);
}
EXPORT_SYMBOL(mca_adsp_glink_qbg_unregister_ops);

static void mca_adsp_dispatch(struct list_head *head, struct mca_adsp_event *e)
{
	struct mca_adsp_ops_node *n;
	int idx = srcu_read_lock(&mca_ops_srcu);

	list_for_each_entry_rcu(n, head, node, srcu_read_lock_held(&mca_ops_srcu)) {
		if (!e->state_event) {
			if (n->ops->notification)
				n->ops->notification(le32_to_cpu(e->msg.property_id),
					e->msg.data, sizeof(e->msg.data), n->priv);
		} else if (e->state == PMIC_GLINK_STATE_UP) {
			if (n->ops->glink_state_up)
				n->ops->glink_state_up(n->priv);
		} else if (n->ops->glink_state_down) {
			n->ops->glink_state_down(n->priv);
		}
	}
	srcu_read_unlock(&mca_ops_srcu, idx);
}

static void mca_adsp_event_work(struct work_struct *work)
{
	struct mca_adsp_glink_dev *mca = container_of(work,
			struct mca_adsp_glink_dev, event_work);
	struct mca_adsp_event *e;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&mca->lock, flags);
		if (list_empty(&mca->events)) {
			spin_unlock_irqrestore(&mca->lock, flags);
			return;
		}
		e = list_first_entry(&mca->events, struct mca_adsp_event, node);
		list_del(&e->node);
		mca->event_count--;
		spin_unlock_irqrestore(&mca->lock, flags);
		if (READ_ONCE(mca->stopping)) {
			kfree(e);
			continue;
		}
		if (e->state_event) {
			mca_adsp_dispatch(&g_mca_adsp_glink_ops_head, e);
			mca_adsp_dispatch(&g_mca_adsp_glink_qbg_ops_head, e);
		} else {
			mca_adsp_dispatch(le32_to_cpu(e->msg.hdr.owner) ==
				MCA_ADSP_GLINK_QBG_OWNER ?
				&g_mca_adsp_glink_qbg_ops_head :
				&g_mca_adsp_glink_ops_head, e);
		}
		kfree(e);
	}
}

/* Caller holds lock. Notifications leave capacity for service transitions. */
static void mca_adsp_queue_event(struct mca_adsp_glink_dev *mca,
				struct mca_adsp_event *e)
{
	if (!e)
		return;
	if (mca->stopping || mca->event_count >= MCA_ADSP_EVENT_MAX) {
		kfree(e);
		return;
	}
	list_add_tail(&e->node, &mca->events);
	mca->event_count++;
	if (mca->ready)
		queue_work(mca->wq, &mca->event_work);
}

static int mca_adsp_glink_handle_message(struct mca_adsp_glink_dev *mca,
			struct mca_adsp_glink_resp_msg *msg, size_t len)
{
	unsigned long flags;
	u32 retcode;

	if (len != sizeof(*msg))
		return -EMSGSIZE;
	spin_lock_irqsave(&mca->lock, flags);
	if (!mca->pending || mca->stopping ||
	    le32_to_cpu(msg->hdr.type) != MCA_ADSP_GLINK_MSG_REQ ||
	    le32_to_cpu(msg->hdr.owner) != mca->owner ||
	    le32_to_cpu(msg->hdr.opcode) != mca->opcode ||
	    le32_to_cpu(msg->property_id) != mca->cur_property_id ||
	    le32_to_cpu(msg->seq) != mca->seq) {
		spin_unlock_irqrestore(&mca->lock, flags);
		return -EINVAL;
	}
	retcode = le32_to_cpu(msg->retcode);
	/* Never turn an ADSP failure into a positive kernel success code. */
	mca->retcode = retcode ? -EREMOTEIO : 0;
	if (!retcode && mca->opcode == MCA_ADSP_GLINK_OPCODE_READ)
		memcpy(mca->read_buf, msg->data, sizeof(mca->read_buf));
	mca->pending = false;
	complete(&mca->ack);
	spin_unlock_irqrestore(&mca->lock, flags);
	return 0;
}

static int mca_adsp_glink_callback(void *priv, void *data, size_t len)
{
	struct mca_adsp_glink_dev *mca = priv;
	struct pmic_glink_hdr *hdr = data;
	struct mca_adsp_event *e;
	unsigned long flags;
	u32 owner, type;

	if (!data || len < sizeof(*hdr))
		return -EMSGSIZE;
	owner = le32_to_cpu(hdr->owner);
	type = le32_to_cpu(hdr->type);
	if (owner != MCA_ADSP_GLINK_OWNER && owner != MCA_ADSP_GLINK_QBG_OWNER)
		return -EINVAL;
	if (type == MCA_ADSP_GLINK_MSG_REQ)
		return mca_adsp_glink_handle_message(mca, data, len);
	if (type != MCA_ADSP_GLINK_MSG_NOTIFY ||
	    len != sizeof(struct mca_adsp_glink_notify_msg))
		return -EMSGSIZE;
	/* Vendor notifications use property_id as their event discriminator. */
	e = kzalloc(sizeof(*e), GFP_ATOMIC);
	if (!e)
		return -ENOMEM;
	memcpy(&e->msg, data, len);
	spin_lock_irqsave(&mca->lock, flags);
	/* Keep two slots available for PDR transitions. */
	if (mca->event_count >= MCA_ADSP_EVENT_MAX - 2) {
		kfree(e);
	} else {
		mca_adsp_queue_event(mca, e);
	}
	spin_unlock_irqrestore(&mca->lock, flags);
	return 0;
}

static void mca_adsp_glink_state_cb(void *priv, enum pmic_glink_state state)
{
	struct mca_adsp_glink_dev *mca = priv;
	struct mca_adsp_event *e;
	unsigned long flags;

	e = kzalloc(sizeof(*e), GFP_ATOMIC);
	if (!e)
		state = PMIC_GLINK_STATE_DOWN;
	if (e) {
		e->state_event = true;
		e->state = state;
	}
	spin_lock_irqsave(&mca->lock, flags);
	if (state == mca->glink_state) {
		kfree(e);
		goto out;
	}
	mca->glink_state = state;
	if (state != PMIC_GLINK_STATE_UP && mca->pending) {
		mca->retcode = -ENOTCONN;
		mca->pending = false;
		complete(&mca->ack);
	}
	mca_adsp_queue_event(mca, e);
out:
	spin_unlock_irqrestore(&mca->lock, flags);
}

static int mca_adsp_glink_probe(struct auxiliary_device *adev,
			       const struct auxiliary_device_id *id)
{
	struct mca_adsp_glink_dev *mca;
	struct pmic_glink_client_data data = {
		.name = "mca_adsp_glink", .id = MCA_ADSP_GLINK_OWNER,
		.msg_cb = mca_adsp_glink_callback, .state_cb = mca_adsp_glink_state_cb,
	};
	int ret;

	mca = devm_kzalloc(&adev->dev, sizeof(*mca), GFP_KERNEL);
	if (!mca)
		return -ENOMEM;
	mca->dev = &adev->dev;
	mutex_init(&mca->rw_lock);
	spin_lock_init(&mca->lock);
	init_completion(&mca->ack);
	INIT_LIST_HEAD(&mca->events);
	INIT_WORK(&mca->event_work, mca_adsp_event_work);
	mca->wq = alloc_ordered_workqueue("mca_glink", WQ_MEM_RECLAIM);
	if (!mca->wq)
		return -ENOMEM;
	data.priv = mca;
	mca->client = pmic_glink_register_client(&adev->dev, &data);
	if (IS_ERR(mca->client)) {
		ret = PTR_ERR(mca->client);
		goto out_wq;
	}
	data.id = MCA_ADSP_GLINK_QBG_OWNER;
	/* Both clients share service state; only the main owner queues PDR events. */
	data.state_cb = NULL;
	mca->qbg_client = pmic_glink_register_client(&adev->dev, &data);
	if (IS_ERR(mca->qbg_client)) {
		ret = PTR_ERR(mca->qbg_client);
		goto out_client;
	}
	dev_set_drvdata(&adev->dev, mca);
	mutex_lock(&mca_device_lock);
	if (rcu_access_pointer(g_mca_adsp_glink)) {
		mutex_unlock(&mca_device_lock);
		ret = -EBUSY;
		pmic_glink_unregister_client(mca->qbg_client);
		goto out_client;
	}
	rcu_assign_pointer(g_mca_adsp_glink, mca);
	mutex_unlock(&mca_device_lock);
	spin_lock_irq(&mca->lock);
	mca->ready = true;
	queue_work(mca->wq, &mca->event_work);
	spin_unlock_irq(&mca->lock);
	dev_notice(&adev->dev, "MCA ADSP GLINK transport registered\n");
	return 0;
out_client:
	pmic_glink_unregister_client(mca->client);
out_wq:
	WRITE_ONCE(mca->stopping, true);
	queue_work(mca->wq, &mca->event_work);
	/* Already queued initial state callbacks cannot outlive a failed probe. */
	destroy_workqueue(mca->wq);
	return ret;
}

static void mca_adsp_glink_remove(struct auxiliary_device *adev)
{
	struct mca_adsp_glink_dev *mca = dev_get_drvdata(&adev->dev);
	unsigned long flags;

	mutex_lock(&mca_device_lock);
	RCU_INIT_POINTER(g_mca_adsp_glink, NULL);
	mutex_unlock(&mca_device_lock);
	spin_lock_irqsave(&mca->lock, flags);
	mca->stopping = true;
	mca->glink_state = PMIC_GLINK_STATE_DOWN;
	if (mca->pending) {
		mca->pending = false;
		mca->retcode = -ENOTCONN;
		complete(&mca->ack);
	}
	spin_unlock_irqrestore(&mca->lock, flags);
	pmic_glink_unregister_client(mca->qbg_client);
	pmic_glink_unregister_client(mca->client);
	synchronize_srcu(&mca_device_srcu);
	destroy_workqueue(mca->wq);
}

static const struct auxiliary_device_id mca_adsp_ids[] = {
	{ .name = "pmic_glink.power-supply" },
	{ }
};
MODULE_DEVICE_TABLE(auxiliary, mca_adsp_ids);
static struct auxiliary_driver mca_adsp_driver = {
	.name = "mca_adsp_glink",
	.probe = mca_adsp_glink_probe,
	.remove = mca_adsp_glink_remove,
	.id_table = mca_adsp_ids,
};
module_auxiliary_driver(mca_adsp_driver);
MODULE_DESCRIPTION("MCA ADSP GLINK transport");
MODULE_LICENSE("GPL");
