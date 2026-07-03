#ifndef AETHER_SLOT_H
#define AETHER_SLOT_H

#include <linux/types.h>

/*
 * Shared layout between user-space (mm-link BpfQueueExporter) and
 * kernel-space (aether_test.ko).  Stored in an 8-entry BPF ARRAY
 * map (AETHER_SIGNAL_MAX_ENTRIES) keyed by (seq % MAX_ENTRIES), so
 * the map always holds the most recent AETHER_SIGNAL_MAX_ENTRIES
 * samples.  This supports the gamma(=5) multi-sample capacity
 * estimation required by the paper (S4.5, Eq. 1) and the burst
 * detection logic in S4.4 (delta_dwell).
 */
struct aether_slot {
	__u64 seq;
	__u64 tstamp_ns;
	__u32 qlen;
	__u32 live_qlen;
	__u32 dwell_ms;
	__s32 delta_dwell;   /* ΔD_k = D_k - D_{k-1} in ms (paper §4.4) */
};

#define AETHER_SIGNAL_PIN_PATH "/sys/fs/bpf/aether_signal"
#define AETHER_SIGNAL_MAX_ENTRIES 8    /* ≥ γ(=5) from the paper */

/* Legacy compat aliases (scripts may still reference old names) */
#define AETHER_QUEUE_PIN_PATH  AETHER_SIGNAL_PIN_PATH
#define AETHER_QUEUE_CAPACITY  AETHER_SIGNAL_MAX_ENTRIES

#endif
