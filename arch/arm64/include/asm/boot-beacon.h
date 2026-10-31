/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Early boot beacon for headless arm64 bring-up.
 */
#ifndef __ASM_BOOT_BEACON_H
#define __ASM_BOOT_BEACON_H

#include <linux/types.h>

#ifdef CONFIG_ARM64_BOOT_BEACON
void arm64_boot_beacon(const char *stage);
void arm64_boot_beacon_val(const char *key, u64 val);
void arm64_boot_beacon_str(const char *key, const char *val);
u32 arm64_boot_beacon_bulk(const void *src, u32 len, u32 *stride_out);
#else
static inline void arm64_boot_beacon(const char *stage) { }
static inline void arm64_boot_beacon_val(const char *key, u64 val) { }
static inline void arm64_boot_beacon_str(const char *key, const char *val) { }
static inline u32 arm64_boot_beacon_bulk(const void *src, u32 len,
					 u32 *stride_out) { return 0; }
#endif

#endif /* __ASM_BOOT_BEACON_H */
