/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM qrtr

#if !defined(_TRACE_QRTR_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_QRTR_H

#include <linux/qrtr.h>
#include <linux/tracepoint.h>

TRACE_EVENT(qrtr_ns_service_announce_new,

	TP_PROTO(unsigned int service, unsigned int instance,
		 unsigned int node, unsigned int port),

	TP_ARGS(service, instance, node, port),

	TP_STRUCT__entry(
		__field(unsigned int, service)
		__field(unsigned int, instance)
		__field(unsigned int, node)
		__field(unsigned int, port)
	),

	TP_fast_assign(
		__entry->service = service;
		__entry->instance = instance;
		__entry->node = node;
		__entry->port = port;
	),

	TP_printk("advertising new server [%d:%x]@[%d:%d]",
		  __entry->service, __entry->instance, __entry->node,
		  __entry->port
	)
);

TRACE_EVENT(qrtr_ns_service_announce_del,

	TP_PROTO(unsigned int service, unsigned int instance,
		 unsigned int node, unsigned int port),

	TP_ARGS(service, instance, node, port),

	TP_STRUCT__entry(
		__field(unsigned int, service)
		__field(unsigned int, instance)
		__field(unsigned int, node)
		__field(unsigned int, port)
	),

	TP_fast_assign(
		__entry->service = service;
		__entry->instance = instance;
		__entry->node = node;
		__entry->port = port;
	),

	TP_printk("advertising removal of server [%d:%x]@[%d:%d]",
		  __entry->service, __entry->instance, __entry->node,
		  __entry->port
	)
);

TRACE_EVENT(qrtr_ns_server_add,

	TP_PROTO(unsigned int service, unsigned int instance,
		 unsigned int node, unsigned int port),

	TP_ARGS(service, instance, node, port),

	TP_STRUCT__entry(
		__field(unsigned int, service)
		__field(unsigned int, instance)
		__field(unsigned int, node)
		__field(unsigned int, port)
	),

	TP_fast_assign(
		__entry->service = service;
		__entry->instance = instance;
		__entry->node = node;
		__entry->port = port;
	),

	TP_printk("add server [%d:%x]@[%d:%d]",
		  __entry->service, __entry->instance, __entry->node,
		  __entry->port
	)
);

TRACE_EVENT(qrtr_ns_message,

	TP_PROTO(const char * const ctrl_pkt_str, __u32 sq_node, __u32 sq_port),

	TP_ARGS(ctrl_pkt_str, sq_node, sq_port),

	TP_STRUCT__entry(
		__string(ctrl_pkt_str, ctrl_pkt_str)
		__field(__u32, sq_node)
		__field(__u32, sq_port)
	),

	TP_fast_assign(
		__assign_str(ctrl_pkt_str);
		__entry->sq_node = sq_node;
		__entry->sq_port = sq_port;
	),

	TP_printk("%s from %d:%d",
		  __get_str(ctrl_pkt_str), __entry->sq_node, __entry->sq_port
	)
);

TRACE_EVENT(qrtr_ns_announce_result,
	TP_PROTO(unsigned int service, u32 node, u32 port, int rc),
	TP_ARGS(service, node, port, rc),
	TP_STRUCT__entry(
		__field(unsigned int, service)
		__field(u32, node)
		__field(u32, port)
		__field(int, rc)
	),
	TP_fast_assign(
		__entry->service = service;
		__entry->node = node;
		__entry->port = port;
		__entry->rc = rc;
	),
	TP_printk("service=%u destination=%u:%u rc=%d",
		  __entry->service, __entry->node, __entry->port, __entry->rc)
);

TRACE_EVENT(qrtr_ns_announce_retry,
	TP_PROTO(u32 node, unsigned int remaining, int rc),
	TP_ARGS(node, remaining, rc),
	TP_STRUCT__entry(
		__field(u32, node)
		__field(unsigned int, remaining)
		__field(int, rc)
	),
	TP_fast_assign(
		__entry->node = node;
		__entry->remaining = remaining;
		__entry->rc = rc;
	),
	TP_printk("node=%u remaining=%u rc=%d",
		  __entry->node, __entry->remaining, __entry->rc)
);

TRACE_EVENT(qrtr_tx_hello_gate,
	TP_PROTO(u32 node, unsigned int type, u32 from_port, u32 to_port),
	TP_ARGS(node, type, from_port, to_port),
	TP_STRUCT__entry(
		__field(u32, node)
		__field(unsigned int, type)
		__field(u32, from_port)
		__field(u32, to_port)
	),
	TP_fast_assign(
		__entry->node = node;
		__entry->type = type;
		__entry->from_port = from_port;
		__entry->to_port = to_port;
	),
	TP_printk("node=%u type=%u from_port=%u to_port=%u rc=-11",
		  __entry->node, __entry->type, __entry->from_port,
		  __entry->to_port)
);

#endif /* _TRACE_QRTR_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
