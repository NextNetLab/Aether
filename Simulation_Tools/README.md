## Build

### 1. Build the eBPF signal pipeline

```bash
cd ebpf && make
cd ..
```

This produces:
- `ebpf/aether-map-setup` — user-space tool that creates and pins the BPF ARRAY map at `/sys/fs/bpf/aether_signal`
- `ebpf/aether_test.ko` — kernel module that reads the map and exports signals via `/proc/aether_test`

### 2. Build Mahimahi

```bash
./autogen.sh && ./configure && make
```

### 3. Set SUID on mm-link

`mm-link` creates network namespaces and configures iptables rules, so it needs root privileges:

```bash
sudo chown root:root src/frontend/mm-link
sudo chmod u+s src/frontend/mm-link
```

## Running

### Start the eBPF signal pipeline

```bash
# Create and pin the BPF ARRAY map (8 entries, keyed by seq % 8)
sudo ./ebpf/aether-map-setup

# Load the kernel reader module
sudo insmod ./ebpf/aether_test.ko
```

### Verify the pipeline is live

```bash
watch -n 1 'echo "== mahimahi queue info =="; cat /proc/aether_test'
```

### Try to run a trace-driven emulation

