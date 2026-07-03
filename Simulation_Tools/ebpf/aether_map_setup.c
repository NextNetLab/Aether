/*
 * Build:  cc -O2 aether_map_setup.c -o aether-map-setup
 * Run  :  sudo mount -t bpf bpf /sys/fs/bpf 2>/dev/null
 *         sudo ./aether-map-setup
 *
 * Uses raw bpf() syscall so it works without libbpf-dev.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/bpf.h>
#include "aether_slot.h"

static int sys_bpf(int cmd, union bpf_attr *attr, unsigned int size)
{
	return syscall(__NR_bpf, cmd, attr, size);
}

int main(void)
{
	union bpf_attr attr = {};
	int map_fd, err;
	struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };

	/* Kernels <5.11 charge BPF maps against RLIMIT_MEMLOCK; the default
	 * 64 KiB limit causes BPF_MAP_CREATE to return EPERM (not ENOMEM). */
	if (setrlimit(RLIMIT_MEMLOCK, &rl) < 0)
		perror("setrlimit(RLIMIT_MEMLOCK) [continuing]");

	attr.map_type    = BPF_MAP_TYPE_ARRAY;
	attr.key_size    = sizeof(__u32);
	attr.value_size  = sizeof(struct aether_slot);
	attr.max_entries = AETHER_SIGNAL_MAX_ENTRIES;
	strncpy(attr.map_name, "aether_signal", sizeof(attr.map_name) - 1);

	map_fd = sys_bpf(BPF_MAP_CREATE, &attr, sizeof(attr));
	if (map_fd < 0) { perror("BPF_MAP_CREATE"); return 1; }

	unlink(AETHER_QUEUE_PIN_PATH);

	memset(&attr, 0, sizeof(attr));
	attr.pathname  = (unsigned long)AETHER_QUEUE_PIN_PATH;
	attr.bpf_fd    = map_fd;
	attr.file_flags = 0;

	err = sys_bpf(BPF_OBJ_PIN, &attr, sizeof(attr));
	if (err < 0) { perror("BPF_OBJ_PIN"); return 1; }

	/* mahimahi drops privileges before opening the map, so make it
	 * world-writable. Adjust if you need finer-grained access control. */
	if (chmod(AETHER_QUEUE_PIN_PATH, 0666) < 0)
		perror("chmod [continuing]");

	printf("pinned ARRAY map at %s (value_size=%zu, max_entries=%u)\n",
	       AETHER_SIGNAL_PIN_PATH, sizeof(struct aether_slot),
	       AETHER_SIGNAL_MAX_ENTRIES);
	close(map_fd);
	return 0;
}
