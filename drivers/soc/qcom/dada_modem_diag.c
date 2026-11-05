// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>

#define CREATE_TRACE_POINTS
#include <linux/dada_modem_diag.h>

/* Shared by remoteproc, PD mapper and optional QRTR transport modules. */
EXPORT_TRACEPOINT_SYMBOL_GPL(dada_rproc_phase);
EXPORT_TRACEPOINT_SYMBOL_GPL(dada_pdm_result);
EXPORT_TRACEPOINT_SYMBOL_GPL(dada_qrtr_frame);
