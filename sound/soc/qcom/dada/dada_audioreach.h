/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef DADA_AUDIOREACH_UAPI_H
#define DADA_AUDIOREACH_UAPI_H
#include <linux/ioctl.h>
#include <linux/types.h>

#define DADA_AR_MAX_PAYLOAD (256 * 1024)

/* Fixed-width pointers keep the ioctl identical for 32 and 64 bit clients. */
struct dada_ar_command {
	__u32 opcode;
	__u32 destination;
	__u32 payload_bytes;
	__u32 response_bytes; /* capacity in, actual length out */
	__u64 payload;
	__u64 response;
	__u32 response_opcode;
	__u32 dsp_status;
};
#define DADA_AR_COMMAND _IOWR('D', 0x56, struct dada_ar_command)

/* op=0 registers an inline calibration blob and returns an opaque cookie.
 * op=1 deregisters that cookie using the original driver-owned DMA buffer.
 * A cookie belongs to the same open file; it is never a DSP mapping handle.
 */
struct dada_ar_persistent {
	__u32 op;
	__u32 cookie;
	__u32 payload_bytes;
	__u32 dsp_status;
	__u64 payload;
	__u32 response_opcode;
	__u32 reserved;
};
#define DADA_AR_PERSISTENT _IOWR('D', 0x57, struct dada_ar_persistent)
#endif
