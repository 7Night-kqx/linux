// SPDX-License-Identifier: GPL-2.0-only
/* ST54L raw NCI over I2C. The older ST NCI driver's NDLC is not used. */
#include <linux/bitmap.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nfc.h>
#include <linux/of.h>
#include <linux/skbuff.h>
#include <net/nfc/nci_core.h>

#define ST54L_FRAME_MAX		258
#define ST54L_PROP_CONFIG	0x02
#define ST54L_MIFARE		0x90
#define ST54L_PLL_ERROR		0xe6

static bool trace;
module_param(trace, bool, 0600);
MODULE_PARM_DESC(trace, "Log raw NCI frames for transport debugging");

struct st54l_phy {
	struct i2c_client *client;
	struct nci_dev *ndev;
	struct gpio_desc *reset, *irq_gpio, *clkreq;
	struct mutex io_lock;
	struct completion mode_reset;
	atomic_t irq_enabled;
	int irq, clkreq_irq, hard_fault;
	bool running, switching_mode, classic_active;
	DECLARE_BITMAP(mifare_targets, 256);
};

static int st54l_read(struct st54l_phy *phy, u8 *data, size_t len)
{
	int ret, attempt;

	for (attempt = 0; attempt < 5; attempt++) {
		ret = i2c_master_recv(phy->client, data, len);
		if (ret == len)
			return 0;
		if (ret != -ENXIO)
			return ret < 0 ? ret : -EIO;
		/* Address NACK has consumed no bytes. Never retry a short read. */
		if (attempt != 4)
			usleep_range(10000, 11000);
	}
	return ret;
}

/* Caller holds io_lock; an asserted IRQ means the complete frame is ready. */
static int st54l_read_frame(struct st54l_phy *phy, u8 *frame)
{
	int ret, idle = 0;

	ret = st54l_read(phy, frame, NCI_CTRL_HDR_SIZE);
	if (ret)
		return ret;
	while (frame[0] == 0x7e) {
		if (++idle > 64)
			return -EPROTO;
		frame[0] = frame[1];
		frame[1] = frame[2];
		ret = st54l_read(phy, frame + 2, 1);
		if (ret)
			return ret;
	}
	if ((frame[0] >> 5) > NCI_MT_NTF_PKT || (frame[1] & 0xc0))
		return -EPROTO;
	if (frame[2]) {
		ret = st54l_read(phy, frame + 3, frame[2]);
		if (ret)
			return ret;
	}
	return NCI_CTRL_HDR_SIZE + frame[2];
}

static void st54l_disable_irq(struct st54l_phy *phy)
{
	if (atomic_xchg(&phy->irq_enabled, 0))
		disable_irq_nosync(phy->irq);
	/* Also wait if the receive thread itself disabled the IRQ on failure. */
	synchronize_irq(phy->irq);
}

static int st54l_open(struct nci_dev *ndev)
{
	struct st54l_phy *phy = nci_get_drvdata(ndev);
	unsigned long quiet, deadline;
	u8 frame[ST54L_FRAME_MAX], dummy;
	bool identified = false;
	int ret = 0, level, count = 0;

	mutex_lock(&phy->io_lock);
	phy->hard_fault = 0;
	phy->switching_mode = false;
	bitmap_zero(phy->mifare_targets, 256);
	phy->classic_active = false;
	/* Prime lazy I2C masters; reset below discards this previous session. */
	i2c_master_recv(phy->client, &dummy, 1);
	gpiod_set_value_cansleep(phy->reset, 0);
	msleep(20);
	gpiod_set_value_cansleep(phy->reset, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(phy->reset, 0);
	msleep(20);
	gpiod_set_value_cansleep(phy->reset, 1);
	msleep(80);

	/* Consume unsolicited boot packets before the core's RESET request.
	 * Otherwise a boot notification could complete the wrong request.
	 */
	deadline = jiffies + msecs_to_jiffies(1500);
	quiet = jiffies + msecs_to_jiffies(300);
	while (time_before(jiffies, quiet) && time_before(jiffies, deadline)) {
		level = gpiod_get_value_cansleep(phy->irq_gpio);
		if (level < 0) {
			ret = level;
			goto out;
		}
		if (!level) {
			usleep_range(1000, 2000);
			continue;
		}
		ret = st54l_read_frame(phy, frame);
		if (ret < 0)
			goto out;
		if (ret >= 14 && frame[0] == 0x60 && frame[1] == 0x00 &&
		    frame[5] == 0x20 && frame[6] == 0x02 && frame[7] >= 6 &&
		    frame[7] + 8 <= ret && frame[8] == 0x06) {
			identified = true;
			dev_info(&phy->client->dev,
				 "ST54L rev %02x firmware %02x%02x%02x%02x\n",
				 frame[9], frame[10], frame[11], frame[12], frame[13]);
		}
		if (++count == 128) {
			ret = -EOVERFLOW;
			goto out;
		}
		quiet = jiffies + msecs_to_jiffies(300);
	}
	if (!identified) {
		ret = -ENODEV;
		goto out;
	}
	WRITE_ONCE(phy->running, true);
	ret = 0;
out:
	mutex_unlock(&phy->io_lock);
	if (!ret) {
		atomic_set(&phy->irq_enabled, 1);
		enable_irq(phy->irq);
	}
	return ret;
}

static int st54l_close(struct nci_dev *ndev)
{
	struct st54l_phy *phy = nci_get_drvdata(ndev);

	WRITE_ONCE(phy->running, false);
	st54l_disable_irq(phy);
	/* CORE_RESET performed by NCI closes the RF session. Keep reset
	 * deasserted because ST54L shares the package with a secure element.
	 */
	return 0;
}

static int st54l_send(struct nci_dev *ndev, struct sk_buff *skb)
{
	struct st54l_phy *phy = nci_get_drvdata(ndev);
	u8 *data = skb->data;
	int ret = -ENETDOWN, attempt;

	mutex_lock(&phy->io_lock);
	if (!READ_ONCE(phy->running))
		goto out;
	if (phy->hard_fault) {
		ret = phy->hard_fault;
		goto out;
	}
	if (skb->len < 3 || skb->len != 3 + data[2]) {
		ret = -EINVAL;
		goto out;
	}
	if (data[0] == 0x21 && data[1] == 0x03) {
		bitmap_zero(phy->mifare_targets, 256);
		phy->classic_active = false;
	}
	/* Linux uses the MIFARE/T2T protocol for both Classic and Ultralight.
	 * Restore ST's Classic protocol/interface for that discovered ID only.
	 */
	if (skb->len == 6 && data[0] == 0x21 && data[1] == 0x04 &&
	    data[4] == NCI_RF_PROTOCOL_T2T &&
	    test_bit(data[3], phy->mifare_targets)) {
		data[4] = ST54L_MIFARE;
		data[5] = ST54L_MIFARE;
	}
	if (trace)
		dev_info(&phy->client->dev, "TX %*ph\n", skb->len, data);
	for (attempt = 0; attempt < 5; attempt++) {
		ret = i2c_master_send(phy->client, data, skb->len);
		if (ret == skb->len) {
			ret = 0;
			break;
		}
		if (ret >= 0) {
			ret = -EIO;
			break;
		}
		if (ret != -ENXIO && ret != -EREMOTEIO)
			break;
		if (attempt != 4)
			msleep(20);
	}
out:
	mutex_unlock(&phy->io_lock);
	kfree_skb(skb);
	return ret;
}

static int st54l_prop_response(struct nci_dev *ndev, struct sk_buff *skb)
{
	nci_req_complete(ndev, skb->len ? skb->data[0] : NCI_STATUS_SYNTAX_ERROR);
	return 0;
}

static int st54l_prop_notification(struct nci_dev *ndev, struct sk_buff *skb)
{
	/* ST diagnostics on OID02 are not NFC target or data notifications. */
	return 0;
}

static int st54l_setup(struct nci_dev *ndev)
{
	struct st54l_phy *phy = nci_get_drvdata(ndev);
	static const u8 init[] = { 0x00, 0x00 };
	static const u8 mode[] = { 0x02, 0x01 };
	int ret;

	if (ndev->nci_ver != 0x20)
		return -EPROTONOSUPPORT;
	/* RF mode needs CORE_INIT first; the core calls its final INIT after
	 * setup, so initialize once here before the ST mode-reset sequence.
	 */
	ret = nci_core_cmd(ndev, NCI_OP_CORE_INIT_CMD, sizeof(init), init);
	if (ret)
		return ret;
	reinit_completion(&phy->mode_reset);
	mutex_lock(&phy->io_lock);
	phy->switching_mode = true;
	mutex_unlock(&phy->io_lock);
	ret = nci_prop_cmd(ndev, ST54L_PROP_CONFIG, sizeof(mode), mode);
	if (!ret && !wait_for_completion_timeout(&phy->mode_reset,
						 msecs_to_jiffies(2000)))
		ret = -ETIMEDOUT;
	mutex_lock(&phy->io_lock);
	phy->switching_mode = false;
	if (!ret && phy->hard_fault)
		ret = phy->hard_fault;
	mutex_unlock(&phy->io_lock);
	return ret;
}

static const struct nci_driver_ops st54l_prop_ops[] = {
	{ .opcode = nci_opcode_pack(NCI_GID_PROPRIETARY, ST54L_PROP_CONFIG),
	  .rsp = st54l_prop_response,
	  .ntf = st54l_prop_notification },
};

static const struct nci_ops st54l_ops = {
	.open = st54l_open,
	.close = st54l_close,
	.send = st54l_send,
	.setup = st54l_setup,
	.prop_ops = st54l_prop_ops,
	.n_prop_ops = ARRAY_SIZE(st54l_prop_ops),
};

static irqreturn_t st54l_clkreq_irq(int irq, void *data)
{
	/* Requesting this IRQ keeps TLMM's PDC/MPM forwarding bit enabled.
	 * The board's clock-request path needs no software clock voting.
	 */
	return IRQ_HANDLED;
}

static irqreturn_t st54l_irq_thread(int irq, void *data)
{
	struct st54l_phy *phy = data;
	struct sk_buff *skb;
	u8 frame[ST54L_FRAME_MAX];
	int ret = 0, packets, level;

	for (packets = 0; packets < 32 && READ_ONCE(phy->running); packets++) {
		mutex_lock(&phy->io_lock);
		level = gpiod_get_value_cansleep(phy->irq_gpio);
		if (level <= 0) {
			mutex_unlock(&phy->io_lock);
			ret = level;
			break;
		}
		ret = st54l_read_frame(phy, frame);
		if (ret < 0) {
			mutex_unlock(&phy->io_lock);
			break;
		}
		if (trace)
			dev_info(&phy->client->dev, "RX %*ph\n", ret, frame);
		if (phy->switching_mode && ret >= 8 && frame[0] == 0x60 &&
		    frame[1] == 0 && frame[3] == 0xa0) {
			/* This is the expected ST RF-mode reset, not the core's
			 * pending RESET request. Its next INIT is sent by NCI.
			 */
			complete(&phy->mode_reset);
			mutex_unlock(&phy->io_lock);
			continue;
		}
		if (ret == 4 && frame[0] == 0x60 && frame[1] == 0x07 &&
		    frame[3] == ST54L_PLL_ERROR)
			dev_err_ratelimited(&phy->client->dev,
					    "RF PLL failed; check CLKREQ forwarding\n");
		if (ret >= 8 && frame[0] == 0x61 && frame[1] == 0x03 &&
		    frame[4] == ST54L_MIFARE) {
			set_bit(frame[3], phy->mifare_targets);
			frame[4] = NCI_RF_PROTOCOL_T2T;
		}
		if (ret >= 10 && frame[0] == 0x61 && frame[1] == 0x05)
			phy->classic_active = false;
		if (ret >= 10 && frame[0] == 0x61 && frame[1] == 0x05 &&
		    frame[4] == ST54L_MIFARE && frame[5] == ST54L_MIFARE) {
			phy->classic_active = true;
			set_bit(frame[3], phy->mifare_targets);
			frame[4] = NCI_RF_INTERFACE_FRAME;
			frame[5] = NCI_RF_PROTOCOL_T2T;
		}
		if (ret >= 4 && frame[0] == 0x61 && frame[1] == 0x06)
			phy->classic_active = false;
		/* ST's 0x90 interface returns a Classic READ as 16 bare bytes;
		 * auth/write replies are one status byte. Linux FRAME expects a
		 * trailing NCI status on every data reply, so append OK only to
		 * the bare READ response. The original trace remains unmodified.
		 */
		if (phy->classic_active && ret == 19 && frame[0] == 0 &&
		    frame[1] == 0 && frame[2] == 16) {
			frame[ret++] = NCI_STATUS_OK;
			frame[2]++;
		}
		mutex_unlock(&phy->io_lock);
		skb = nci_skb_alloc(phy->ndev, ret, GFP_KERNEL);
		if (!skb) {
			ret = -ENOMEM;
			break;
		}
		skb_put_data(skb, frame, ret);
		nci_recv_frame(phy->ndev, skb);
	}
	if (ret < 0) {
		mutex_lock(&phy->io_lock);
		phy->hard_fault = ret;
		mutex_unlock(&phy->io_lock);
		dev_err_ratelimited(&phy->client->dev, "NCI receive failed: %d\n", ret);
		if (atomic_xchg(&phy->irq_enabled, 0))
			disable_irq_nosync(phy->irq);
		complete(&phy->mode_reset);
		nci_req_complete(phy->ndev, NCI_STATUS_FAILED);
	}
	return IRQ_HANDLED;
}

static int st54l_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct st54l_phy *phy;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;
	phy->client = client;
	mutex_init(&phy->io_lock);
	init_completion(&phy->mode_reset);
	atomic_set(&phy->irq_enabled, 0);
	phy->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(phy->reset))
		return dev_err_probe(dev, PTR_ERR(phy->reset), "reset GPIO\n");
	phy->irq_gpio = devm_gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(phy->irq_gpio))
		return dev_err_probe(dev, PTR_ERR(phy->irq_gpio), "IRQ GPIO\n");
	phy->clkreq = devm_gpiod_get(dev, "clkreq", GPIOD_IN);
	if (IS_ERR(phy->clkreq))
		return dev_err_probe(dev, PTR_ERR(phy->clkreq), "CLKREQ GPIO\n");
	phy->irq = gpiod_to_irq(phy->irq_gpio);
	if (phy->irq < 0)
		return phy->irq;
	phy->clkreq_irq = gpiod_to_irq(phy->clkreq);
	if (phy->clkreq_irq < 0)
		return phy->clkreq_irq;
	phy->ndev = nci_allocate_device(&st54l_ops,
			NFC_PROTO_MIFARE_MASK | NFC_PROTO_ISO14443_MASK, 0, 0);
	if (!phy->ndev)
		return -ENOMEM;
	nci_set_drvdata(phy->ndev, phy);
	nci_set_parent_dev(phy->ndev, dev);
	i2c_set_clientdata(client, phy);
	ret = devm_request_threaded_irq(dev, phy->irq, NULL, st54l_irq_thread,
			IRQF_TRIGGER_HIGH | IRQF_ONESHOT | IRQF_NO_AUTOEN,
			"st54l-nci", phy);
	if (ret)
		goto free_ndev;
	ret = devm_request_irq(dev, phy->clkreq_irq, st54l_clkreq_irq,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, "st54l-clkreq", phy);
	if (ret)
		goto free_irq;
	ret = nci_register_device(phy->ndev);
	if (ret)
		goto free_clkreq_irq;
	dev_info(dev, "ST54L NCI reader registered with CLKREQ forwarding\n");
	return 0;

free_clkreq_irq:
	devm_free_irq(dev, phy->clkreq_irq, phy);
free_irq:
	devm_free_irq(dev, phy->irq, phy);
free_ndev:
	nci_free_device(phy->ndev);
	return ret;
}

static void st54l_remove(struct i2c_client *client)
{
	struct st54l_phy *phy = i2c_get_clientdata(client);

	nci_unregister_device(phy->ndev);
	WRITE_ONCE(phy->running, false);
	st54l_disable_irq(phy);
	devm_free_irq(&client->dev, phy->irq, phy);
	devm_free_irq(&client->dev, phy->clkreq_irq, phy);
	nci_free_device(phy->ndev);
}

static const struct of_device_id st54l_of_match[] = {
	{ .compatible = "st,st54l" },
	{ }
};
MODULE_DEVICE_TABLE(of, st54l_of_match);

static const struct i2c_device_id st54l_ids[] = {
	{ "st54l" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, st54l_ids);

static struct i2c_driver st54l_driver = {
	.driver = { .name = "st54l_i2c", .of_match_table = st54l_of_match },
	.probe = st54l_probe,
	.remove = st54l_remove,
	.id_table = st54l_ids,
};
module_i2c_driver(st54l_driver);

MODULE_DESCRIPTION("ST54L raw NCI I2C reader");
MODULE_LICENSE("GPL");
