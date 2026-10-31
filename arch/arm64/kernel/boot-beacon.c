// SPDX-License-Identifier: GPL-2.0-only
/*
 * Early boot beacon for headless arm64 bring-up.
 *
 * Some phones have no accessible UART, a DSI command-mode panel that cannot be
 * driven by simple-framebuffer, and a stock kernel built without CONFIG_DEVMEM.
 * On those devices a new kernel that dies before the console comes up leaves no
 * trace at all, so there is no way to tell "the bootloader never jumped to us"
 * apart from "we died in paging_init".
 *
 * The ramoops region is the one channel that survives a warm reset and can be
 * read back by the kernel that runs next. This file hand-writes a record in the
 * fs/pstore/ram_core.c persistent_ram_buffer format at a fixed physical
 * address, from points where nothing else is available yet. The next kernel to
 * boot exposes it under /sys/fs/pstore.
 *
 * CONFIG_ARM64_BOOT_BEACON_ADDR must name a zone that no ramoops node in this
 * kernel's own device tree covers. Two writers with independent bookkeeping on
 * one 12-byte header produce a torn header that the reading kernel discards.
 *
 * BIT DECAY
 * ---------
 * On the device this was written for the ramoops region loses roughly 30% of
 * its set bits across a reset and gains about 0.6% of its zero bits. Neither is
 * this code's doing: the stock kernel's own 2 MB pmsg record shows the same
 * after a plain reboot. Three things here are designed around it.
 *
 * First, start is pinned to 0 and never advanced, so start <= size holds at
 * every instant whatever the reset timing. (persistent_ram_save_old() reads
 * data[start..size) then data[0..start), so start == 0 and start == size give
 * the same linear text for a buffer that never wrapped.)
 *
 * Second, size advertises the whole area this boot zeroed rather than the
 * length actually written. The reading kernel keeps a record only while
 * size <= buffer_size, so the field must never read back larger than it was
 * written -- forward decay cannot do that -- and must stay large enough to
 * cover the text -- which forward decay can undo. Advertising the maximum
 * turns the second requirement into a soft one: 0xfff0 carries twelve set
 * bits, so what survives is some prefix of the area, and every whole stride
 * inside that prefix is a whole copy of the text. Round D advertised the exact
 * record length, lost bits 9..11 of it on both runs, and handed back 499 bytes
 * of a 4083-byte record.
 *
 * Third, the text is written at a fixed stride until the advertised area is
 * full. Because forward loss is one-directional, OR-ing the copies is exact
 * recovery rather than a vote: a bit reads 1 if any single copy still carries
 * it, so residual loss falls as p^N. Thirty-one copies is not thoroughness for
 * its own sake, it is simply what is left over once the area has been sized for
 * the size field's cushion, and it costs one memcpy per copy per append.
 *
 * The stride is a compile-time constant on purpose. Packing copies against a
 * running length would make every chunk boundary depend on the size field, and
 * that field decays: a reader dividing a truncated record by a copy count gets
 * misaligned chunks, and OR-ing misaligned text corrupts the copy that was
 * still intact. With a fixed stride a truncated record just yields fewer whole
 * copies. Readers must therefore be told the stride rather than infer it from
 * the record length -- see work/decode-beacon.py --stride.
 *
 * LIFETIME
 * --------
 * Nothing here is __init. The point of round E is to reach call sites in
 * kernel_init() past free_initmem(), plus panic() and die(), and .init.text is
 * gone by then. beacon_map() and beacon_unmap() are __ref because they still
 * call early_memremap() on the SYSTEM_BOOTING side of the line, which is the
 * same escape hatch kernel_init() itself uses.
 *
 * Once vmalloc is up the mapping is taken once and kept forever. That is not an
 * optimisation: memremap() allocates, and a panic-time append must not, because
 * the panic may well have come from inside the allocator.
 */

#include <linux/build_bug.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/stdarg.h>
#include <linux/string.h>
#include <linux/types.h>

#include <generated/utsrelease.h>

#include <asm/boot-beacon.h>
#include <asm/cacheflush.h>

/* Must match struct persistent_ram_buffer in fs/pstore/ram_core.c. */
struct beacon_ram_buffer {
	u32 sig;
	u32 start;
	u32 size;
	u8  data[];
};

/* PERSISTENT_RAM_SIG, "DBGC" in little endian. */
#define BEACON_SIG		0x43474244

#define BEACON_ZONE_PA		((phys_addr_t)CONFIG_ARM64_BOOT_BEACON_ADDR)
#define BEACON_ZONE_SIZE	((u64)CONFIG_ARM64_BOOT_BEACON_SIZE)

/*
 * 64 KB of the zone is mapped, zeroed and advertised. The ceiling is
 * early_memremap()'s: arm64 sizes one btmap slot at NR_FIX_BTMAPS =
 * SZ_256K / PAGE_SIZE pages, so sixteen pages in a single call is legal.
 *
 * Every byte between the header and BEACON_ADVERTISE has to be written by this
 * boot, because that whole span is what the reading kernel hands back and a
 * reader OR-merges all of it. Zeroed padding merges harmlessly -- 0x00 is
 * subset-consistent with every character -- but a byte left behind by an
 * earlier boot would merge straight into the recovered text.
 *
 * BEACON_ADVERTISE is 0xfff0 rather than BEACON_TEXT_MAX so that its low four
 * bits are clear: those are the only bits whose loss is free, since the last
 * fifteen bytes of the area are padding either way.
 */
#define BEACON_MAP_SIZE		0x10000UL
#define BEACON_TEXT_MAX		(BEACON_MAP_SIZE - sizeof(struct beacon_ram_buffer))
#define BEACON_ADVERTISE	0xfff0U

#define BEACON_STRIDE		CONFIG_ARM64_BOOT_BEACON_STRIDE
#define BEACON_REPLICAS		(BEACON_ADVERTISE / BEACON_STRIDE)
#define BEACON_RECORD_LEN	(BEACON_STRIDE * BEACON_REPLICAS)

static_assert(BEACON_ADVERTISE <= BEACON_TEXT_MAX,
	      "the advertised span must be inside the area that gets zeroed");
static_assert(BEACON_REPLICAS >= 1,
	      "the stride must leave room for at least one copy");

/*
 * ROUND K: THE BULK REGION
 * ------------------------
 * Round J measured what eleven rounds of hunting for console-ramoops-0 had been
 * assuming: the probe's own pstore-ram console zone is fine. At reset it held
 * 22202 bytes of text under a header that passes every check the reading kernel
 * makes. Stock produced no console-ramoops-0 anyway, and a read-only search of
 * the vendor log partitions found those bytes nowhere on flash. So the record
 * was lost, and what lost it is the one thing that zone has and this one does
 * not: a single unreplicated twelve-byte header, in DRAM that clears 31% of its
 * set bits per reset.
 *
 * So round K stops relying on that header. The console payload is copied in
 * here, behind the line record, replicated at a stride, under the header that
 * has survived all eleven rounds.
 *
 *   data[0 .. BEACON_ADVERTISE)        the line record: BEACON_REPLICAS copies
 *                                      at BEACON_STRIDE. Unchanged.
 *   data[BEACON_ADVERTISE .. size)     the bulk region: N copies of the payload
 *                                      at a stride the line record reports.
 *
 * The order is deliberate twice over. `size` decays downward, so a truncated
 * record is a prefix and the prefix must be the part that already works; and a
 * reader has to learn the bulk geometry from the line record, because the length
 * of the record is exactly the field that cannot be trusted.
 *
 * Advertising the whole zone also widens what `start` decay the reading kernel
 * tolerates. persistent_ram_post_init() rejects a zone when start > size, and
 * this DRAM has been seen flipping start up at bits 25 and 26. Advertising
 * ~2 MB instead of 64 KB makes bits 16..20 survivable. Bit 25 is still fatal --
 * nothing on the write side can fix that, which is why the panic path in
 * kernel_restart() exists as a second, independent channel.
 */
#define BEACON_BULK_OFF		BEACON_ADVERTISE
#define BEACON_BULK_ALIGN	16U

static bool beacon_started;
static bool beacon_busy;
static u32 beacon_used;
static void *beacon_pin;

/*
 * What the header currently claims. BEACON_ADVERTISE until a bulk copy widens
 * it; every publish rewrites the field from here, so a line appended after the
 * bulk copy cannot narrow the window back down and discard it.
 */
static u32 beacon_advertised = BEACON_ADVERTISE;

/*
 * early_memremap() is legal only while system_state is SYSTEM_BOOTING and it
 * lives in .init.text, hence __ref. Past that point vmalloc is up, memremap()
 * is the right call, and its result is kept: see LIFETIME above.
 */
static void * __ref beacon_map(void)
{
	if (beacon_pin)
		return beacon_pin;

	if (system_state == SYSTEM_BOOTING)
		return early_memremap(BEACON_ZONE_PA, BEACON_MAP_SIZE);

	/*
	 * Round K maps the whole zone, not just the advertised 64 KB. The bulk
	 * copy runs from kernel_restart() and could run from panic(), memremap()
	 * allocates, and a panic-time write must not allocate -- the panic may
	 * have come from inside the allocator. So the mapping that the bulk copy
	 * needs has to be the one taken here, during initcalls.
	 */
	beacon_pin = memremap(BEACON_ZONE_PA, BEACON_ZONE_SIZE, MEMREMAP_WB);

	return beacon_pin;
}

static void __ref beacon_unmap(void *va)
{
	/* The pinned mapping is deliberately never torn down. */
	if (va != beacon_pin)
		early_memunmap(va, BEACON_MAP_SIZE);
}

static void beacon_publish(void *va, struct beacon_ram_buffer *buf)
{
	unsigned int r;

	/*
	 * Each copy is rewritten wholesale from copy 0 on every append rather
	 * than appended to in place. The copies are only worth having if they
	 * are individually whole, and this way a reset landing mid-publish can
	 * leave a copy short but never leave one misaligned.
	 */
	for (r = 1; r < BEACON_REPLICAS; r++)
		memcpy(buf->data + r * BEACON_STRIDE, buf->data, beacon_used);

	/* Text must be in DRAM before size advertises it. */
	dcache_clean_inval_poc((unsigned long)va,
			       (unsigned long)va + BEACON_MAP_SIZE);

	/*
	 * Constant after the first append and rewritten every time anyway: it
	 * costs one store and keeps the header's cache line freshly written.
	 * That is the leading explanation for why this 12-byte header survives
	 * decay better than the data area, and rewriting it here is what makes
	 * that explanation testable.
	 */
	buf->size = beacon_advertised;

	dcache_clean_inval_poc((unsigned long)va,
			       (unsigned long)va + BEACON_MAP_SIZE);
}

static void beacon_append(const char *fmt, ...)
{
	struct beacon_ram_buffer *buf;
	va_list args;
	void *va;

	if (BEACON_ZONE_SIZE < BEACON_MAP_SIZE)
		return;

	/*
	 * A fault inside this function reaches die(), which calls back in. One
	 * flag turns that into a lost line instead of unbounded recursion. Not
	 * a lock: this is a bring-up facility on a maxcpus=1 probe kernel, and
	 * two CPUs racing here would tear the record whatever we did.
	 */
	if (beacon_busy)
		return;
	beacon_busy = true;

	va = beacon_map();
	if (!va)
		goto out;

	buf = va;

	if (!beacon_started) {
		/*
		 * Take the zone over exactly once per boot. Anything the
		 * previous kernel left here is intentionally discarded: this
		 * record is only useful as evidence about the current boot.
		 */
		beacon_started = true;
		beacon_used = 0;
		buf->sig = BEACON_SIG;
		buf->start = 0;
		buf->size = 0;
		beacon_advertised = BEACON_ADVERTISE;
		memset(buf->data, 0, BEACON_TEXT_MAX);
		beacon_used += scnprintf(buf->data, BEACON_STRIDE,
					 "arm64-boot-beacon %s\n", UTS_RELEASE);
	}

	if (beacon_used < BEACON_STRIDE) {
		va_start(args, fmt);
		beacon_used += vscnprintf(buf->data + beacon_used,
					  BEACON_STRIDE - beacon_used,
					  fmt, args);
		va_end(args);
	}

	beacon_publish(va, buf);

	beacon_unmap(va);
out:
	beacon_busy = false;
}

void arm64_boot_beacon(const char *stage)
{
	beacon_append("stage: %s\n", stage);
}

void arm64_boot_beacon_val(const char *key, u64 val)
{
	beacon_append("%s: 0x%llx\n", key, val);
}

/*
 * Copy len bytes from src into the zone behind the line record, replicated as
 * many times as the zone has room for, and widen the header to cover it.
 * Returns the number of copies written and reports the stride through
 * stride_out; the caller publishes both as ordinary beacon lines, because the
 * line record is the part that survives and a reader must never have to guess
 * the geometry.
 *
 * Allocation-free by construction: this uses the mapping beacon_map() pinned
 * during initcalls. If that pin was never taken -- nothing appended past
 * SYSTEM_BOOTING -- there is nothing to copy into and this does nothing.
 *
 * src must not point inside this zone. The only caller passes the pstore-ram
 * console zone at CONFIG_ARM64_BOOT_BEACON_ADDR - 0x200000, so it does not.
 */
u32 arm64_boot_beacon_bulk(const void *src, u32 len, u32 *stride_out)
{
	struct beacon_ram_buffer *buf = beacon_pin;
	u32 stride, replicas, room, end, r;
	u8 *dst;

	if (!buf || !beacon_started || !src || !len || beacon_busy)
		return 0;

	stride = round_up(len, BEACON_BULK_ALIGN);
	if (BEACON_ZONE_SIZE < sizeof(*buf) + BEACON_BULK_OFF + stride)
		return 0;

	beacon_busy = true;

	room = (u32)(BEACON_ZONE_SIZE - sizeof(*buf)) - BEACON_BULK_OFF;
	replicas = room / stride;
	end = BEACON_BULK_OFF + replicas * stride;
	dst = buf->data + BEACON_BULK_OFF;

	/*
	 * Each copy is padded out to the stride rather than left short. The pad
	 * is inside the span the header advertises, so bytes an earlier boot left
	 * there would be handed to the reader and merged straight into the text.
	 */
	for (r = 0; r < replicas; r++) {
		memcpy(dst + r * stride, src, len);
		memset(dst + r * stride + len, 0, stride - len);
	}

	/* Every byte in DRAM before the field that advertises it. */
	dcache_clean_inval_poc((unsigned long)buf,
			       (unsigned long)buf + sizeof(*buf) + end);

	beacon_advertised = end;
	buf->size = beacon_advertised;

	dcache_clean_inval_poc((unsigned long)buf,
			       (unsigned long)buf + sizeof(*buf) + end);

	beacon_busy = false;

	if (stride_out)
		*stride_out = stride;

	return replicas;
}

void arm64_boot_beacon_str(const char *key, const char *val)
{
	beacon_append("%s: %s\n", key, val ? val : "(null)");
}
