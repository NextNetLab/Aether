/*
 * aether_test.ko — BPF ARRAY map signal reader for Aether CCA.
 *
 * The BPF ARRAY map (8 entries, key = seq % 8) holds the most recent
 * AETHER_SIGNAL_MAX_ENTRIES samples pushed by mm-link's BpfQueueExporter.
 * This module caches all entries and exports functions that tcp_aether.ko
 * can call, mirroring the MHI driver's API:
 *   - aether_bpf_get_sample(back, ...)   read the N-th most recent sample
 *   - aether_bpf_get_latest_sample(...)  shorthand for back=0
 *   - aether_bpf_get_queue_len(...)      latest live queue length
 *
 * Load AFTER aether-map-setup has pinned the map:
 *   sudo insmod aether_test.ko
 *
 * Debug:  cat /proc/aether_test   prints latest + recent samples
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/bpf.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include "aether_slot.h"

static struct bpf_map *aether_map;
static struct proc_dir_entry *proc_entry;

static struct aether_slot cached_slots[AETHER_SIGNAL_MAX_ENTRIES];
static u64 cached_head;
static DEFINE_SPINLOCK(cached_lock);

static struct bpf_map *aether_get_pinned_map(const char *path)
{
	struct path p;
	struct inode *inode;
	struct bpf_map *map;
	int err;

	err = kern_path(path, LOOKUP_FOLLOW, &p);
	if (err) {
		pr_err("aether_test: kern_path(%s) failed: %d\n", path, err);
		return ERR_PTR(err);
	}
	inode = d_backing_inode(p.dentry);
	if (!inode || !inode->i_private) {
		path_put(&p);
		pr_err("aether_test: %s has no bpf object\n", path);
		return ERR_PTR(-EINVAL);
	}
	map = inode->i_private;
	if (map->map_type != BPF_MAP_TYPE_ARRAY ||
	    map->value_size != sizeof(struct aether_slot)) {
		path_put(&p);
		pr_err("aether_test: %s is not our aether_signal "
		       "(type=%u value_size=%u)\n",
		       path, map->map_type, map->value_size);
		return ERR_PTR(-EINVAL);
	}
	bpf_map_inc(map);
	path_put(&p);
	return map;
}

static void aether_refresh_cache(void)
{
	struct aether_slot *src;
	__u32 key;
	int i;
	u64 max_seq = 0;
	u64 new_head = cached_head;

	if (!aether_map || !aether_map->ops || !aether_map->ops->map_lookup_elem)
		return;

	rcu_read_lock();
	for (i = 0; i < AETHER_SIGNAL_MAX_ENTRIES; i++) {
		key = i;
		src = aether_map->ops->map_lookup_elem(aether_map, &key);
		if (!src)
			continue;
		if (src->seq > max_seq) {
			max_seq = src->seq;
			new_head = src->seq + 1;
		}
		spin_lock_bh(&cached_lock);
		cached_slots[i] = *src;
		spin_unlock_bh(&cached_lock);
	}
	spin_lock_bh(&cached_lock);
	cached_head = new_head;
	spin_unlock_bh(&cached_lock);
	rcu_read_unlock();
}

int aether_bpf_get_sample(u32 back, u64 *sample_seq, u64 *tstamp_ns,
			  u64 *dwell_ns, u32 *queue_len, u32 *bytes)
{
	struct aether_slot slot;
	u64 head;
	u32 index;

	if (!aether_map)
		return -ENODEV;

	spin_lock_bh(&cached_lock);
	head = cached_head;
	spin_unlock_bh(&cached_lock);

	if (head == 0 || back >= AETHER_SIGNAL_MAX_ENTRIES || back >= head)
		return -ENOENT;

	index = (head - 1 - back) % AETHER_SIGNAL_MAX_ENTRIES;

	spin_lock_bh(&cached_lock);
	slot = cached_slots[index];
	spin_unlock_bh(&cached_lock);

	if (slot.seq != head - 1 - back)
		return -ENOENT;

	if (!slot.seq && !slot.tstamp_ns)
		return -ENOENT;

	*sample_seq = slot.seq;
	*tstamp_ns  = slot.tstamp_ns;
	*dwell_ns   = (u64)slot.dwell_ms * NSEC_PER_MSEC;
	*queue_len  = slot.qlen;
	*bytes      = 0;

	return 0;
}
EXPORT_SYMBOL_GPL(aether_bpf_get_sample);

int aether_bpf_get_latest_sample(u64 *sample_seq, u64 *tstamp_ns,
				 u64 *dwell_ns, u32 *queue_len,
				 u32 *bytes)
{
	return aether_bpf_get_sample(0, sample_seq, tstamp_ns, dwell_ns,
				      queue_len, bytes);
}
EXPORT_SYMBOL_GPL(aether_bpf_get_latest_sample);

int aether_bpf_get_queue_len(u32 *queue_len)
{
	struct aether_slot slot;
	u64 head;

	if (!aether_map)
		return -ENODEV;

	spin_lock_bh(&cached_lock);
	head = cached_head;
	if (head == 0) {
		spin_unlock_bh(&cached_lock);
		return -ENOENT;
	}
	slot = cached_slots[(head - 1) % AETHER_SIGNAL_MAX_ENTRIES];
	spin_unlock_bh(&cached_lock);

	*queue_len = slot.live_qlen;
	return 0;
}
EXPORT_SYMBOL_GPL(aether_bpf_get_queue_len);

static ssize_t proc_read(struct file *f, char __user *buf,
			 size_t count, loff_t *pos)
{
	char kbuf[512];
	int n, i;
	u64 head;
	struct aether_slot *s;

	if (*pos > 0)
		return 0;

	aether_refresh_cache();

	spin_lock_bh(&cached_lock);
	head = cached_head;
	s = &cached_slots[(head - 1) % AETHER_SIGNAL_MAX_ENTRIES];
	n = snprintf(kbuf, sizeof(kbuf),
		     "head=%llu latest: seq=%llu ts=%llu qlen=%u live=%u "
		     "dwell=%u ΔD=%dms\n\nRecent samples:\n",
		     (unsigned long long)head,
		     (unsigned long long)s->seq,
		     (unsigned long long)s->tstamp_ns,
		     s->qlen, s->live_qlen, s->dwell_ms, s->delta_dwell);
	for (i = 0; i < AETHER_SIGNAL_MAX_ENTRIES && head > i; i++) {
		u32 idx = (head - 1 - i) % AETHER_SIGNAL_MAX_ENTRIES;
		struct aether_slot *p = &cached_slots[idx];
		int left = sizeof(kbuf) - n;
		int m;
		if (!p->seq && !p->tstamp_ns)
			continue;
		m = snprintf(kbuf + n, left,
			     "  back=%d seq=%llu qlen=%u dwell=%u ΔD=%dms\n",
			     i, (unsigned long long)p->seq,
			     p->qlen, p->dwell_ms, p->delta_dwell);
		if (m >= left)
			break;
		n += m;
	}
	spin_unlock_bh(&cached_lock);

	if (n > count)
		return -EINVAL;
	if (copy_to_user(buf, kbuf, n))
		return -EFAULT;
	*pos += n;
	return n;
}

static const struct proc_ops aether_test_pops = {
	.proc_read = proc_read,
};

static int __init aether_test_init(void)
{
	aether_map = aether_get_pinned_map(AETHER_SIGNAL_PIN_PATH);
	if (IS_ERR(aether_map))
		return PTR_ERR(aether_map);

	aether_refresh_cache();

	proc_entry = proc_create("aether_test", 0444, NULL, &aether_test_pops);
	if (!proc_entry) {
		bpf_map_put(aether_map);
		return -ENOMEM;
	}
	pr_info("aether_test: loaded, map=%p type=ARRAY entries=%u\n",
		aether_map, aether_map->max_entries);
	return 0;
}

static void __exit aether_test_exit(void)
{
	proc_remove(proc_entry);
	bpf_map_put(aether_map);
	pr_info("aether_test: unloaded\n");
}

module_init(aether_test_init);
module_exit(aether_test_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aether");
MODULE_DESCRIPTION("Aether BPF ARRAY multi-entry signal reader");
