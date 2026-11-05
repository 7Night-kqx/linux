// SPDX-License-Identifier: GPL-2.0
/* Root-controlled hostless backend references. Userspace must hold
 * prepared media PCMs until all backend references are released. Never create
 * a backend, reconfigure its media format, or change the DAI driver's state.
 */
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include "q6apm.h"

struct dada_backend_request {
	__u32 op; /* 0 acquire, 1 start, 2 stop, 3 release */
	__u32 graph_id; /* secondary I2S RX=18, TX codec DMA TX3=120 */
	__u32 subgraph_id;
	__u32 start_count;
	__u32 src_iid, src_port, dst_iid, dst_port; /* existing DPCM connection */
};
#define DADA_BACKEND _IOWR('D', 0x58, struct dada_backend_request)

struct backend_slot {
	struct q6apm_graph *graph;
	struct file *owner;
	bool started;
	bool uncertain;
};
static DEFINE_MUTEX(backend_lock);
static struct device *backend_device;
static struct backend_slot slots[2];
static bool detached;

static int stop_slot(struct backend_slot *slot)
{
	int ret;

	if (detached || slot->uncertain)
		return -ENODEV;
	if (!slot->started)
		return 0;
	ret = q6apm_graph_stop(slot->graph);
	if (ret) {
		/* q6apm decrements before sending STOP. Preserve ownership and
		 * quarantine the graph instead of claiming STOP succeeded.
		 */
		slot->graph->ar_graph->start_count++;
		slot->uncertain = true;
		return ret;
	}
	slot->started = false;
	return 0;
}

static long backend_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct dada_backend_request request;
	struct audioreach_graph *cached;
	struct audioreach_graph_info *info;
	struct audioreach_graph_info *connection_info;
	struct audioreach_sub_graph *sg;
	struct q6apm *apm;
	struct backend_slot *slot;
	int ret = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (cmd != DADA_BACKEND)
		return -ENOTTY;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (request.op > 3 || (request.graph_id != 18 && request.graph_id != 120))
		return -EINVAL;
	slot = &slots[request.graph_id == 120];
	mutex_lock(&backend_lock);
	if (detached) {
		ret = -ENODEV;
		goto unlock;
	}
	apm = dev_get_drvdata(backend_device->parent);
	if (detached || !apm || !device_is_registered(backend_device) || slot->uncertain) {
		ret = -ENODEV;
		goto unlock;
	}
	if (!request.op) {
		if (slot->graph) {
			ret = -EBUSY;
			goto unlock;
		}
		/* Prepared PCMs own these graphs. Refuse to instantiate a graph
		 * without the ASoC clocks, codec power and BE media configuration.
		 */
		mutex_lock(&apm->lock);
		cached = idr_find(&apm->graph_idr, request.graph_id);
		if (!cached || cached->start_count)
			ret = -EBUSY;
		mutex_unlock(&apm->lock);
		if (ret)
			goto unlock;
		slot->graph = q6apm_graph_open(backend_device, NULL, NULL,
			request.graph_id, request.graph_id == 120);
		if (IS_ERR(slot->graph)) {
			ret = PTR_ERR(slot->graph);
			slot->graph = NULL;
			goto unlock;
		}
		slot->owner = file;
		__module_get(THIS_MODULE);
	} else if (!slot->graph || slot->owner != file) {
		ret = -ENOENT;
		goto unlock;
	}
	info = slot->graph->info;
	if (info->num_sub_graphs != 1) {
		ret = -EINVAL;
		goto unlock;
	}
	sg = list_first_entry(&info->sg_list, struct audioreach_sub_graph, node);
	request.subgraph_id = sg->sub_graph_id;
	/* Capture DPCM stores its connection on FE1; playback on BE18. */
	mutex_lock(&apm->lock);
	connection_info = request.graph_id == 120 ?
		idr_find(&apm->graph_info_idr, 1) : info;
	if (!connection_info) {
		mutex_unlock(&apm->lock);
		ret = -ENODEV;
		goto unlock;
	}
	request.src_iid = connection_info->src_mod_inst_id;
	request.src_port = connection_info->src_mod_op_port_id;
	request.dst_iid = connection_info->dst_mod_inst_id;
	request.dst_port = connection_info->dst_mod_ip_port_id;
	mutex_unlock(&apm->lock);
	if (request.op == 1 && !slot->started) {
		ret = q6apm_graph_start(slot->graph);
		if (!ret)
			slot->started = true;
		else
			slot->uncertain = true;
	} else if (request.op >= 2) {
		ret = stop_slot(slot);
	}
	request.start_count = slot->graph->ar_graph->start_count;
	if (!ret && request.op == 3) {
		q6apm_graph_close(slot->graph);
		slot->graph = NULL;
		slot->owner = NULL;
		module_put(THIS_MODULE);
	}
	if (!ret && copy_to_user((void __user *)arg, &request, sizeof(request)))
		ret = -EFAULT;
unlock:
	mutex_unlock(&backend_lock);
	return ret;
}

static int backend_release(struct inode *inode, struct file *file)
{
	int i;

	mutex_lock(&backend_lock);
	for (i = 0; i < ARRAY_SIZE(slots); i++) {
		struct backend_slot *slot = &slots[i];

		if (slot->owner != file)
			continue;
		slot->owner = NULL;
		if (stop_slot(slot)) {
			pr_err("dada-voice-backend: STOP failed; retaining graph and module reference\n");
			continue;
		}
		q6apm_graph_close(slot->graph);
		slot->graph = NULL;
		module_put(THIS_MODULE);
	}
	mutex_unlock(&backend_lock);
	return 0;
}

static const struct file_operations backend_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = backend_ioctl,
	.release = backend_release,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
	.llseek = noop_llseek,
};
static struct miscdevice backend_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "dada-voice-backend",
	.fops = &backend_fops,
	.mode = 0600,
};

/* Serialize the final ioctl with driver removal before devm frees APM or
 * topology state. A borrowed graph is retained after detach, never reused.
 */
static int backend_bus_event(struct notifier_block *nb, unsigned long event, void *data)
{
	if ((event != BUS_NOTIFY_DEL_DEVICE && event != BUS_NOTIFY_UNBIND_DRIVER) ||
	    (data != backend_device && data != backend_device->parent))
		return NOTIFY_DONE;
	mutex_lock(&backend_lock);
	detached = true;
	mutex_unlock(&backend_lock);
	return NOTIFY_OK;
}
static struct notifier_block backend_notifier = { .notifier_call = backend_bus_event };
static struct notifier_block backend_platform_notifier = { .notifier_call = backend_bus_event };

static int __init backend_init(void)
{
	int ret;

	backend_device = bus_find_device_by_name(&platform_bus_type, NULL,
		"6800000.remoteproc:glink-edge:gpr:service@1:bedais");
	if (!backend_device)
		return -ENODEV;
	ret = bus_register_notifier(&aprbus, &backend_notifier);
	if (ret)
		goto put;
	ret = bus_register_notifier(&platform_bus_type, &backend_platform_notifier);
	if (ret)
		goto apr_notifier;
	ret = misc_register(&backend_misc);
	if (!ret)
		return 0;
	bus_unregister_notifier(&platform_bus_type, &backend_platform_notifier);
apr_notifier:
	bus_unregister_notifier(&aprbus, &backend_notifier);
put:
	put_device(backend_device);
	return ret;
}
static void __exit backend_exit(void)
{
	misc_deregister(&backend_misc);
	bus_unregister_notifier(&platform_bus_type, &backend_platform_notifier);
	bus_unregister_notifier(&aprbus, &backend_notifier);
	put_device(backend_device);
}
module_init(backend_init);
module_exit(backend_exit);
MODULE_DESCRIPTION("Dada hostless AudioReach backend references");
MODULE_LICENSE("GPL");
MODULE_VERSION("20261104-lifecycle-1");
