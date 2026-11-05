/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DADA_DIAG_DECODE_H
#define _DADA_DIAG_DECODE_H

/* No unaligned accesses; used only by the opt-in transport tracepoint. */
static inline u32 dada_diag_le32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static inline u16 dada_diag_le16(const u8 *p)
{
	return (u16)p[0] | (u16)p[1] << 8;
}

static void dada_diag_decode_frame(const void *data, size_t len,
				   struct dada_qrtr_frame *frame)
{
	const u8 *p = data;
	size_t header, payload, capture;

	memset(frame, 0, sizeof(*frame));
	if (!len)
		return;
	frame->version = p[0];
	if (p[0] == 1) {
		if (len < 32 || dada_diag_le32(p) != 1)
			return;
		header = 32;
		frame->type = dada_diag_le32(p + 4);
		frame->src_node = dada_diag_le32(p + 8);
		frame->src_port = dada_diag_le32(p + 12);
		frame->size = dada_diag_le32(p + 20);
		frame->dst_node = dada_diag_le32(p + 24);
		frame->dst_port = dada_diag_le32(p + 28);
	} else if (p[0] == 3) {
		if (len < 16)
			return;
		header = 16 + p[3];
		frame->type = p[1];
		frame->size = dada_diag_le32(p + 4);
		frame->src_node = dada_diag_le16(p + 8);
		frame->src_port = dada_diag_le16(p + 10);
		frame->dst_node = dada_diag_le16(p + 12);
		frame->dst_port = dada_diag_le16(p + 14);
	} else {
		return;
	}
	if (header > len || !frame->size || frame->size > len - header)
		return;
	payload = frame->size;
	if (len - header - payload != (4 - (payload & 3)) % 4)
		return;
	frame->valid = true;
	p += header;
	capture = payload < DADA_QRTR_PREFIX_SIZE ? payload : DADA_QRTR_PREFIX_SIZE;
	frame->captured = capture;
	memcpy(frame->prefix, p, capture);
	/* DATA can contain other protocols: label a plausible QMI header only. */
	if (frame->type != 1 || payload < 7 ||
	    (p[0] != 0 && p[0] != 2 && p[0] != 4))
		return;
	frame->qmi_type = p[0];
	frame->txn_id = dada_diag_le16(p + 1);
	frame->msg_id = dada_diag_le16(p + 3);
	frame->msg_len = dada_diag_le16(p + 5);
	frame->qmi_candidate = frame->msg_len == payload - 7;
}
#endif
