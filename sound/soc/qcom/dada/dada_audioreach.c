// SPDX-License-Identifier: GPL-2.0
/* AudioReach control transport using a dedicated dynamic GPR port.
 * This shares the existing ADSP transport without taking over APM/PRM,
 * restarting DSPs or exposing audio samples. Only driver-owned DMA buffers
 * are mapped; userspace cannot supply physical addresses or mapping handles.
 */
#include <linux/capability.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/firmware.h>
#include <linux/hex.h>
#include <crypto/sha2.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/soc/qcom/apr.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include "dada_audioreach.h"

#define APM_GRAPH_OPEN 0x01001000
#define APM_GRAPH_PREPARE 0x01001001
#define APM_GRAPH_START 0x01001002
#define APM_GRAPH_STOP 0x01001003
#define APM_GRAPH_CLOSE 0x01001004
#define APM_SET_CFG 0x01001006
#define APM_GET_CFG 0x01001007
#define APM_REGISTER_CFG 0x01001008
#define APM_DEREGISTER_CFG 0x01001009
#define APM_GET_STATE 0x01001021
#define APM_MAP_MEMORY 0x0100100c
#define APM_UNMAP_MEMORY 0x0100100d

static DEFINE_MUTEX(command_lock);
static DEFINE_SPINLOCK(response_lock);
static DECLARE_COMPLETION(response_ready);
static struct device *apm_device;
static gpr_port_t *control_port;
static void *response_buffer;
static u32 sequence, pending_token, pending_opcode, pending_destination;
static u32 response_size, response_opcode, response_status;
static int response_error;
static bool pending;
static bool detached;
static struct device *dma_device;
static u32 dma_sid;
struct control_mapping {
	struct list_head node;
	void *cpu;
	dma_addr_t dma;
	u32 bytes;
	u32 handle;
	u32 payload_bytes;
	u32 cookie;
	struct file *owner;
	bool persistent;
	bool uncertain;
};
static LIST_HEAD(mappings);
static unsigned int mapping_count;
static char topology_sha256[SHA256_DIGEST_SIZE * 2 + 1];
module_param_string(topology_sha256, topology_sha256, sizeof(topology_sha256), 0444);
static unsigned int oob_threshold = 512;
module_param(oob_threshold, uint, 0600);
MODULE_PARM_DESC(oob_threshold, "Inline payload limit for transport comparison (bytes)");

static bool command_allowed(u32 opcode, u32 destination)
{
	if (destination == GPR_APM_MODULE_IID) {
		switch (opcode) {
		case APM_GRAPH_OPEN:
		case APM_GRAPH_PREPARE:
		case APM_GRAPH_START:
		case APM_GRAPH_STOP:
		case APM_GRAPH_CLOSE:
		case APM_SET_CFG:
		case APM_GET_CFG:
		case APM_GET_STATE:
			return true;
		}
	}
	return destination == GPR_VCPM_MODULE_IID &&
	       (opcode == APM_SET_CFG || opcode == APM_GET_CFG);
}

static int response_callback(const struct gpr_resp_pkt *packet, void *priv, int op)
{
	unsigned long flags;
	const struct gpr_ibasic_rsp_result_t *basic;

	spin_lock_irqsave(&response_lock, flags);
	if (!pending || packet->hdr.token != pending_token ||
	    packet->hdr.src_domain != GPR_DOMAIN_ID_ADSP ||
	    packet->hdr.src_port != pending_destination ||
	    packet->hdr.opcode == GPR_BASIC_EVT_ACCEPTED)
		goto unlock;
	if (packet->hdr.opcode != GPR_BASIC_RSP_RESULT &&
	    !(pending_opcode == APM_GET_STATE && packet->hdr.opcode == 0x02001007) &&
	    !(pending_opcode == APM_MAP_MEMORY && packet->hdr.opcode == 0x02001001) &&
	    !(pending_opcode == APM_GET_CFG && packet->hdr.opcode == 0x02001000))
		goto unlock;

	response_opcode = packet->hdr.opcode;
	response_error = 0;
	response_status = 0;
	response_size = 0;
	if (packet->payload_size < 0 || packet->payload_size > DADA_AR_MAX_PAYLOAD) {
		response_error = -EMSGSIZE;
	} else if (packet->hdr.opcode == GPR_BASIC_RSP_RESULT) {
		if (packet->payload_size < sizeof(*basic)) {
			response_error = -EPROTO;
		} else {
			basic = packet->payload;
			if (basic->opcode != pending_opcode)
				goto unlock;
			response_status = basic->status;
		}
	}
	if (!response_error) {
		response_size = packet->payload_size;
		if (response_size)
			memcpy(response_buffer, packet->payload, response_size);
		if (packet->hdr.opcode == 0x02001000 && response_size >= 4)
			response_status = *(u32 *)response_buffer;
	}
	pending = false;
	complete(&response_ready);
unlock:
	spin_unlock_irqrestore(&response_lock, flags);
	return 0;
}

/* Caller holds command_lock. Responses remain in the shared response buffer. */
static int exchange(struct gpr_pkt *packet, u32 opcode, u32 destination, u32 bytes)
{
	unsigned long flags;
	int ret;

	if (detached || !control_port)
		return -ENODEV;
	response_status = 0;
	response_opcode = 0;
	response_size = 0;
	packet->hdr.version = GPR_PKT_VER;
	packet->hdr.hdr_size = GPR_PKT_HEADER_WORD_SIZE;
	packet->hdr.pkt_size = GPR_HDR_SIZE + bytes;
	packet->hdr.src_domain = GPR_DOMAIN_ID_APPS;
	packet->hdr.dest_domain = GPR_DOMAIN_ID_ADSP;
	packet->hdr.src_port = control_port->id;
	packet->hdr.dest_port = destination;
	packet->hdr.opcode = opcode;
	packet->hdr.token = ++sequence;
	reinit_completion(&response_ready);
	spin_lock_irqsave(&response_lock, flags);
	pending_token = packet->hdr.token;
	pending_opcode = opcode;
	pending_destination = destination;
	pending = true;
	spin_unlock_irqrestore(&response_lock, flags);
	ret = gpr_send_port_pkt(control_port, packet);
	if (ret < 0)
		goto clear;
	ret = wait_for_completion_interruptible_timeout(&response_ready, 5 * HZ);
	if (!ret)
		ret = -ETIMEDOUT;
	else if (ret > 0)
		ret = response_error;
clear:
	spin_lock_irqsave(&response_lock, flags);
	pending = false;
	spin_unlock_irqrestore(&response_lock, flags);
	return ret;
}

static int map_payload(struct gpr_pkt *packet, u32 bytes,
		       struct control_mapping **mapped)
{
	struct { struct gpr_hdr hdr; u32 data[5]; } map = {};
	struct control_mapping *memory;
	u64 address;
	int ret;

	if (mapping_count >= 16)
		return -ENOSPC;
	memory = kzalloc(sizeof(*memory), GFP_KERNEL);
	if (!memory)
		return -ENOMEM;
	memory->bytes = ALIGN(bytes - 16, 4096);
	memory->payload_bytes = bytes - 16;
	memory->cpu = dma_alloc_coherent(dma_device, memory->bytes, &memory->dma, GFP_KERNEL);
	if (!memory->cpu) {
		ret = -ENOMEM;
		goto free;
	}
	memcpy(memory->cpu, &packet->payload[4], bytes - 16);
	address = (u64)memory->dma | ((u64)dma_sid << 32);
	map.data[0] = 3 | (1 << 16); /* SHMEM8_4K_POOL, one region */
	map.data[1] = 0; /* physical address (includes the AudioReach IOMMU SID) */
	map.data[2] = lower_32_bits(address);
	map.data[3] = upper_32_bits(address);
	map.data[4] = memory->bytes;
	ret = exchange((struct gpr_pkt *)&map, APM_MAP_MEMORY, 1, sizeof(map.data));
	if (ret || response_status || response_opcode != 0x02001001 || response_size != 4) {
		/* A lost/malformed response does not prove MAP was rejected. The
		 * handle is unknown, so neither UNMAP nor freeing DMA is safe.
		 * Retain the allocation until reboot, including interrupted waits.
		 */
		if (ret || !response_status) {
			memory->uncertain = true;
			__module_get(THIS_MODULE);
			list_add_tail(&memory->node, &mappings);
			mapping_count++;
			pr_err("dada-audioreach: MAP result uncertain; retaining DMA until reboot\n");
			return ret ?: -EPROTO;
		}
		if (!ret)
			ret = -EREMOTEIO;
		goto dma;
	}
	memory->handle = *(u32 *)response_buffer;
	__module_get(THIS_MODULE);
	list_add_tail(&memory->node, &mappings);
	mapping_count++;
	*mapped = memory;
	pr_info("dada-audioreach: mapped sid=%u address=%#llx bytes=%u handle=%#x\n",
		dma_sid, address, memory->bytes, memory->handle);
	packet->payload[0] = lower_32_bits(address);
	packet->payload[1] = upper_32_bits(address);
	packet->payload[2] = memory->handle;
	/* Normal command payloads are copied; persistent calibration is retained. */
	return 0;
dma:
	dma_free_coherent(dma_device, memory->bytes, memory->cpu, memory->dma);
free:
	kfree(memory);
	return ret;
}

static void release_payload(struct control_mapping *memory, bool unmap)
{
	if (memory->uncertain || detached)
		return;
	if (unmap) {
		struct { struct gpr_hdr hdr; u32 handle; } command = {};
		command.handle = memory->handle;
		if (exchange((struct gpr_pkt *)&command, APM_UNMAP_MEMORY, 1, 4) || response_status)
			return; /* Keep the module pinned until DSP releases this memory. */
	}
	list_del(&memory->node);
	dma_free_coherent(dma_device, memory->bytes, memory->cpu, memory->dma);
	kfree(memory);
	mapping_count--;
	module_put(THIS_MODULE);
}

static void release_payloads(bool unmap)
{
	struct control_mapping *memory, *next;

	list_for_each_entry_safe(memory, next, &mappings, node)
		release_payload(memory, unmap);
}

static int deregister_payload(struct control_mapping *memory)
{
	struct { struct gpr_hdr hdr; u32 data[4]; } packet = {};
	u64 address = (u64)memory->dma | ((u64)dma_sid << 32);
	int ret;

	packet.data[0] = lower_32_bits(address);
	packet.data[1] = upper_32_bits(address);
	packet.data[2] = memory->handle;
	packet.data[3] = memory->payload_bytes;
	ret = exchange((struct gpr_pkt *)&packet, APM_DEREGISTER_CFG, 1, 16);
	if (!ret && !response_status)
		memory->persistent = false;
	else if (ret)
		memory->uncertain = true;
	return ret;
}

static long persistent_ioctl(struct file *file, unsigned long arg)
{
	struct dada_ar_persistent request;
	struct control_mapping *memory = NULL, *candidate;
	struct gpr_pkt *packet = NULL;
	int ret;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (request.op > 1 || request.reserved ||
	    request.payload_bytes > DADA_AR_MAX_PAYLOAD - 16 ||
	    (!request.op && (!request.payload_bytes || request.cookie)) ||
	    (request.op && (request.payload_bytes || request.payload || !request.cookie)))
		return -EINVAL;
	if (!request.op) {
		packet = kvzalloc(GPR_HDR_SIZE + 16 + request.payload_bytes, GFP_KERNEL);
		if (!packet)
			return -ENOMEM;
		packet->payload[3] = request.payload_bytes;
		if (copy_from_user(&packet->payload[4], u64_to_user_ptr(request.payload),
				   request.payload_bytes)) {
			ret = -EFAULT;
			goto free;
		}
	}
	ret = mutex_lock_interruptible(&command_lock);
	if (ret)
		goto free;
	if (detached || !control_port || !device_is_registered(apm_device)) {
		ret = -ENODEV;
		goto unlock;
	}
	if (!request.op) {
		ret = map_payload(packet, request.payload_bytes + 16, &memory);
		if (ret)
			goto unlock;
		memory->owner = file;
		memory->persistent = true;
		memory->cookie = ++sequence;
		ret = exchange(packet, APM_REGISTER_CFG, 1, 16);
		/* Even a failure may have partially registered calibration. */
		if (ret)
			memory->uncertain = true;
		request.cookie = memory->cookie;
	} else {
		list_for_each_entry(candidate, &mappings, node)
			if (candidate->cookie == request.cookie && candidate->owner == file &&
			    candidate->persistent && !candidate->uncertain)
				memory = candidate;
		if (!memory) {
			ret = -ENOENT;
			goto unlock;
		}
		ret = deregister_payload(memory);
	}
	if (ret)
		goto unlock;
	request.dsp_status = response_status;
	request.response_opcode = response_opcode;
	if (copy_to_user((void __user *)arg, &request, sizeof(request)))
		ret = -EFAULT;
	if (request.op && !memory->persistent)
		release_payload(memory, true);
unlock:
	mutex_unlock(&command_lock);
free:
	kvfree(packet);
	return ret;
}

static int control_release(struct inode *inode, struct file *file)
{
	struct control_mapping *memory, *next;

	mutex_lock(&command_lock);
	list_for_each_entry_safe(memory, next, &mappings, node) {
		if (memory->owner != file)
			continue;
		memory->owner = NULL;
		if (detached || !control_port || memory->uncertain)
			continue;
		if (memory->persistent &&
		    (deregister_payload(memory) || memory->persistent))
			continue;
		release_payload(memory, true);
	}
	mutex_unlock(&command_lock);
	return 0;
}

static long control_ioctl(struct file *file, unsigned int ioctl, unsigned long arg)
{
	struct dada_ar_command request;
	struct gpr_pkt *packet;
	struct control_mapping *mapped = NULL;
	u32 capacity;
	u32 transmit_bytes;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (ioctl == DADA_AR_PERSISTENT)
		return persistent_ioctl(file, arg);
	if (ioctl != DADA_AR_COMMAND)
		return -ENOTTY;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!command_allowed(request.opcode, request.destination) ||
	    request.payload_bytes > DADA_AR_MAX_PAYLOAD ||
	    request.response_bytes > DADA_AR_MAX_PAYLOAD)
		return -EINVAL;
	capacity = request.response_bytes;
	packet = kvzalloc(GPR_HDR_SIZE + request.payload_bytes, GFP_KERNEL);
	if (!packet)
		return -ENOMEM;
	if (request.payload_bytes && copy_from_user(packet->payload,
			u64_to_user_ptr(request.payload), request.payload_bytes)) {
		ret = -EFAULT;
		goto free;
	}
	/* Accept inline data only; the driver supplies any OOB DMA addresses. */
	if (request.opcode != APM_GET_STATE &&
	    (request.payload_bytes < 16 || packet->payload[0] ||
	     packet->payload[1] || packet->payload[2] ||
	     packet->payload[3] != request.payload_bytes - 16)) {
		ret = -EINVAL;
		goto free;
	}
	if (request.opcode == APM_GET_STATE && request.payload_bytes) {
		ret = -EINVAL;
		goto free;
	}
	ret = mutex_lock_interruptible(&command_lock);
	if (ret)
		goto free;
	if (detached || !control_port || !device_is_registered(apm_device)) {
		ret = -ENODEV;
		goto unlock;
	}
	transmit_bytes = request.payload_bytes;
	if (transmit_bytes > min(oob_threshold, 512U) && transmit_bytes > 16) {
		ret = map_payload(packet, transmit_bytes, &mapped);
		if (ret)
			goto unlock;
		transmit_bytes = 16;
	}
	ret = exchange(packet, request.opcode, request.destination, transmit_bytes);
	if (ret < 0) {
		if (mapped)
			mapped->uncertain = true;
		goto unlock;
	}
	request.response_opcode = response_opcode;
	request.dsp_status = response_status;
	request.response_bytes = response_size;
	if (mapped && request.opcode == APM_GET_CFG &&
	    response_opcode == 0x02001000 && response_size == 4) {
		request.response_bytes = request.payload_bytes - 16;
		memcpy(response_buffer, mapped->cpu, request.response_bytes);
		response_size = request.response_bytes;
	}
	/* On OPEN failure, return SPF-written per-parameter errors after the
	 * basic response. This avoids issuing separate module-loader queries.
	 */
	if (mapped && request.opcode == APM_GRAPH_OPEN && response_status &&
	    response_opcode == GPR_BASIC_RSP_RESULT && response_size == 8) {
		memcpy(response_buffer + 8, mapped->cpu, request.payload_bytes - 16);
		response_size += request.payload_bytes - 16;
		request.response_bytes = response_size;
	}
	if (response_size > capacity)
		ret = -ENOSPC;
	else if (response_size && copy_to_user(u64_to_user_ptr(request.response),
					    response_buffer, response_size))
		ret = -EFAULT;
	if (copy_to_user((void __user *)arg, &request, sizeof(request)))
		ret = -EFAULT;
	/* A received response ends SPF access to nonpersistent command payloads.
	 * On timeout, keep memory pinned: the command may still be executing.
	 */
	if (mapped)
		release_payload(mapped, true);
unlock:
	mutex_unlock(&command_lock);
free:
	kvfree(packet);
	return ret;
}

static const struct file_operations control_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = control_ioctl,
	.release = control_release,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
	.llseek = noop_llseek,
};

static struct miscdevice control_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "dada-audioreach",
	.fops = &control_fops,
	.mode = 0600,
};

/* Device removal is not proof that DSP DMA has stopped. Never free mappings
 * here, nor dereference a stale packet router to unregister the dynamic port.
 */
static int bus_event(struct notifier_block *nb, unsigned long event, void *data)
{
	if ((event != BUS_NOTIFY_DEL_DEVICE && event != BUS_NOTIFY_UNBIND_DRIVER) ||
	    data != apm_device)
		return NOTIFY_DONE;
	mutex_lock(&command_lock);
	detached = true;
	pr_err("dada-audioreach: transport detached; unavailable until reboot\n");
	mutex_unlock(&command_lock);
	return NOTIFY_OK;
}

static struct notifier_block bus_notifier = { .notifier_call = bus_event };

static int __init control_init(void)
{
	struct device_node *node;
	struct platform_device *pdev;
	struct of_phandle_args args;
	int ret;
	const struct firmware *fw;
	u8 digest[SHA256_DIGEST_SIZE];

	apm_device = bus_find_device_by_name(&aprbus, NULL, "gprsvc:service:2:1");
	if (!apm_device)
		return -ENODEV;
	/* This image embeds the topology. request_firmware() follows the same
	 * lookup as q6apm, so checking /lib/firmware alone is insufficient.
	 */
	ret = request_firmware(&fw, "qcom/sm8750/Xiaomi-dada-tplg.bin", apm_device);
	if (ret)
		goto put;
	sha256(fw->data, fw->size, digest);
	bin2hex(topology_sha256, digest, sizeof(digest));
	release_firmware(fw);
	if (strcmp(topology_sha256,
		   "d6dcd361d4cd6784fc20cdf02e37278c532cf3dbd127630fa3bd72e3093cce92")) {
		ret = -EINVAL;
		pr_err("dada-audioreach: unsupported topology; control port not created\n");
		goto put;
	}
	node = of_find_compatible_node(NULL, NULL, "qcom,q6apm-dais");
	if (!node) {
		ret = -ENODEV;
		goto put;
	}
	pdev = of_find_device_by_node(node);
	ret = of_parse_phandle_with_fixed_args(node, "iommus", 1, 0, &args);
	of_node_put(node);
	if (!pdev || ret) {
		if (pdev)
			put_device(&pdev->dev);
		ret = -ENODEV;
		goto put;
	}
	dma_device = &pdev->dev;
	dma_sid = args.args[0] & 0xf;
	of_node_put(args.np);
	response_buffer = kvzalloc(DADA_AR_MAX_PAYLOAD, GFP_KERNEL);
	if (!response_buffer) {
		ret = -ENOMEM;
		goto put_dma;
	}
	control_port = gpr_alloc_port(to_apr_device(apm_device), apm_device,
				      response_callback, NULL);
	if (IS_ERR(control_port)) {
		ret = PTR_ERR(control_port);
		control_port = NULL;
		goto buffer;
	}
	ret = bus_register_notifier(&aprbus, &bus_notifier);
	if (ret)
		goto port;
	ret = misc_register(&control_device);
	if (ret)
		goto notifier;
	pr_info("dada-audioreach: control transport ready\n");
	/* This kernel's gpr_free_port() does not synchronize in-flight RX
	 * callbacks. Keep this port/callback/module alive until reboot rather
	 * than pretending hot-unload is safe. Stopping the userspace service
	 * still releases all acknowledged mappings and leaves the port idle.
	 */
	__module_get(THIS_MODULE);
	return 0;
notifier:
	bus_unregister_notifier(&aprbus, &bus_notifier);
port:
	gpr_free_port(control_port);
buffer:
	kvfree(response_buffer);
put_dma:
	put_device(dma_device);
put:
	put_device(apm_device);
	return ret;
}

static void __exit control_exit(void)
{
	misc_deregister(&control_device);
	bus_unregister_notifier(&aprbus, &bus_notifier);
	mutex_lock(&command_lock);
	if (control_port)
		release_payloads(true);
	if (control_port)
		gpr_free_port(control_port);
	control_port = NULL;
	mutex_unlock(&command_lock);
	kvfree(response_buffer);
	put_device(dma_device);
	put_device(apm_device);
}
module_init(control_init);
module_exit(control_exit);
MODULE_DESCRIPTION("Dada AudioReach voice graph control transport");
MODULE_LICENSE("GPL");
MODULE_VERSION("20261104-lifecycle-1");
