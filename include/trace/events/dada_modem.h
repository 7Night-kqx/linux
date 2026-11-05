/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM dada_modem

#if !defined(_TRACE_DADA_MODEM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_DADA_MODEM_H

#include <linux/tracepoint.h>

TRACE_EVENT(dada_qrtr_frame,
	TP_PROTO(bool tx, int rc, const struct dada_qrtr_frame *frame),
	TP_ARGS(tx, rc, frame),
	TP_STRUCT__entry(
		__field(bool, tx)
		__field(int, rc)
		__field(u32, type)
		__field(u32, src_node)
		__field(u32, src_port)
		__field(u32, dst_node)
		__field(u32, dst_port)
		__field(u32, size)
		__field(u8, version)
		__field(bool, valid)
		__field(bool, qmi_candidate)
		__field(u8, qmi_type)
		__field(u16, txn_id)
		__field(u16, msg_id)
		__field(u16, msg_len)
		__field(u16, captured)
		__array(u8, prefix, DADA_QRTR_PREFIX_SIZE)
	),
	TP_fast_assign(
		__entry->tx = tx;
		__entry->rc = rc;
		__entry->type = frame->type;
		__entry->src_node = frame->src_node;
		__entry->src_port = frame->src_port;
		__entry->dst_node = frame->dst_node;
		__entry->dst_port = frame->dst_port;
		__entry->size = frame->size;
		__entry->version = frame->version;
		__entry->valid = frame->valid;
		__entry->qmi_candidate = frame->qmi_candidate;
		__entry->qmi_type = frame->qmi_type;
		__entry->txn_id = frame->txn_id;
		__entry->msg_id = frame->msg_id;
		__entry->msg_len = frame->msg_len;
		__entry->captured = frame->captured;
		memcpy(__entry->prefix, frame->prefix, DADA_QRTR_PREFIX_SIZE);
	),
	TP_printk("%s rc=%d valid=%u v=%u type=%u %u:%u->%u:%u size=%u qmi_candidate=%u qmi_type=%u txn=%u msg=%#x msg_len=%u captured=%u prefix=%s",
		  __entry->tx ? "tx" : "rx", __entry->rc, __entry->valid,
		  __entry->version, __entry->type, __entry->src_node,
		  __entry->src_port, __entry->dst_node, __entry->dst_port,
		  __entry->size, __entry->qmi_candidate, __entry->qmi_type,
		  __entry->txn_id, __entry->msg_id, __entry->msg_len,
		  __entry->captured,
		  __print_hex(__entry->prefix, __entry->captured))
);

TRACE_EVENT(dada_pdm_result,
	TP_PROTO(u32 node, u32 port, u16 txn, const char *service,
		 u32 offset, u32 total, u32 returned, const char *domain,
		 u32 instance, int rc),
	TP_ARGS(node, port, txn, service, offset, total, returned, domain, instance, rc),
	TP_STRUCT__entry(
		__field(u32, node)
		__field(u32, port)
		__field(u16, txn)
		__string(service, service)
		__field(u32, offset)
		__field(u32, total)
		__field(u32, returned)
		__string(domain, domain)
		__field(u32, instance)
		__field(int, rc)
	),
	TP_fast_assign(
		__entry->node = node;
		__entry->port = port;
		__entry->txn = txn;
		__assign_str(service);
		__entry->offset = offset;
		__entry->total = total;
		__entry->returned = returned;
		__assign_str(domain);
		__entry->instance = instance;
		__entry->rc = rc;
	),
	TP_printk("%u:%u txn=%u service=%s offset=%u total=%u returned=%u domain=%s instance=%u rc=%d",
		  __entry->node, __entry->port, __entry->txn,
		  __get_str(service), __entry->offset, __entry->total,
		  __entry->returned, __get_str(domain), __entry->instance,
		  __entry->rc)
);

TRACE_EVENT(dada_rproc_phase,
	TP_PROTO(const char *device, const char *stage, int rc, u64 value),
	TP_ARGS(device, stage, rc, value),
	TP_STRUCT__entry(
		__string(device, device)
		__string(stage, stage)
		__field(int, rc)
		__field(u64, value)
	),
	TP_fast_assign(
		__assign_str(device);
		__assign_str(stage);
		__entry->rc = rc;
		__entry->value = value;
	),
	TP_printk("device=%s stage=%s rc=%d value=%llu",
		  __get_str(device), __get_str(stage), __entry->rc,
		  __entry->value)
);
#endif

#include <trace/define_trace.h>
