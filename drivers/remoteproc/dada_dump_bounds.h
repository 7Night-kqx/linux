/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DADA_DUMP_BOUNDS_H
#define _DADA_DUMP_BOUNDS_H

/* Half-open range containment without overflowing base+length. */
static inline bool dada_dump_range(u64 base, u64 length, u64 address, u64 size)
{
	return length && size && address >= base && address - base < length &&
		size <= length - (address - base);
}

/* Only the observed 4 KiB named buffer is eligible for the SMEM exception. */
static inline bool dada_dump_smem_allowed(const char *name, u64 segment_size,
					 u64 segment_offset, u64 address,
					 u64 offset, u64 size,
					 u64 smem_base, u64 smem_size)
{
	return name && !strcmp(name, "md_dbg_buf") && segment_size == 4096 &&
		!segment_offset && offset < segment_size && size &&
		size <= segment_size - offset &&
		dada_dump_range(smem_base, smem_size, address, segment_size);
}
#endif
