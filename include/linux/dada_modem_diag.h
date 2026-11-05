/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_DADA_MODEM_DIAG_H
#define _LINUX_DADA_MODEM_DIAG_H

#include <linux/of.h>
#include <linux/remoteproc.h>
#include <linux/string.h>
#include <linux/types.h>

#define DADA_QRTR_PREFIX_SIZE 128
struct dada_qrtr_frame {
	u32 type, src_node, src_port, dst_node, dst_port, size;
	u8 version, qmi_type;
	bool valid, qmi_candidate;
	u16 txn_id, msg_id, msg_len, captured;
	u8 prefix[DADA_QRTR_PREFIX_SIZE];
};

#include <trace/events/dada_modem.h>

/* Exact MPSS device match; other remote processors keep their logging. */
static inline bool dada_modem_diag_target(struct rproc *rproc)
{
	return rproc->dev.parent && rproc->name && !strcmp(rproc->name, "modem") &&
		of_device_is_compatible(dev_of_node(rproc->dev.parent),
					"qcom,sm8750-mpss-pas");
}

static inline void dada_modem_diag_phase(struct rproc *rproc,
					const char *stage, int rc, u64 value)
{
	if (!IS_ENABLED(CONFIG_QCOM_DADA_MODEM_DIAG) ||
	    !dada_modem_diag_target(rproc))
		return;

	trace_dada_rproc_phase(dev_name(rproc->dev.parent), stage, rc, value);
	dev_dbg(&rproc->dev, "DADA_DIAG stage=%s rc=%d value=%llu\n",
		  stage, rc, value);
}
#endif
