/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * 兼容头：小米 mca 用的高通厂商版 pmic_glink 客户端 API
 *
 * 原版（从 vendor 内核 include/linux/soc/qcom/qti_pmic_glink.h 拷来）在
 * CONFIG_QTI_PMIC_GLINK 未定义时，把三个函数定义成返回 -ENODEV 的内联桩。
 * 主线没这个 Kconfig，于是 mca_adsp_glink_probe 直接拿到 -19 失败。
 * 实测后果：usb_online 读不到、business_charger 无限重试、整机不充电。
 *
 * 这里改成**只声明**，实现由 mca_pmic_glink_shim.c 提供，转发到主线
 * drivers/soc/qcom/pmic_glink.c 的真实实现上。
 *
 * 注意：这里**不能**再定义 struct pmic_glink_hdr —— 主线
 * include/linux/soc/qcom/pmic_glink.h 已经定义了同名结构体，而 shim 那个
 * 编译单元两个头都要包含，重复定义会直接编译报错。需要报文头的地方请直接
 * 用主线的定义（字段布局一致：owner/type/opcode，主线是 __le32，语义更正确）。
 */

#ifndef __QTI_PMIC_GLINK_H
#define __QTI_PMIC_GLINK_H

#include <linux/types.h>

struct pmic_glink_client;
struct device;

enum pmic_glink_state {
	PMIC_GLINK_STATE_DOWN,
	PMIC_GLINK_STATE_UP,
};

/**
 * struct pmic_glink_client_data - pmic_glink client data
 * @name:	Client name
 * @id:		Unique id for client for communication (= 报文 hdr.owner)
 * @priv:	private data for client
 * @msg_cb:	callback function for client to receive the messages that
 *		are intended to be delivered to it over PMIC Glink
 * @state_cb:	callback function to notify pmic glink state in the event of
 *		a subsystem restart (SSR) or a protection domain restart (PDR)
 */
struct pmic_glink_client_data {
	const char	*name;
	u32		id;
	void		*priv;
	int		(*msg_cb)(void *priv, void *data, size_t len);
	void		(*state_cb)(void *priv, enum pmic_glink_state state);
};

struct pmic_glink_client *pmic_glink_register_client(struct device *dev,
			const struct pmic_glink_client_data *client_data);
int pmic_glink_unregister_client(struct pmic_glink_client *client);
int pmic_glink_write(struct pmic_glink_client *client, void *data,
			size_t len);

#endif /* __QTI_PMIC_GLINK_H */
