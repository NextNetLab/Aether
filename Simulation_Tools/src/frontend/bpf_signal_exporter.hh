/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef BPF_SIGNAL_EXPORTER_HH
#define BPF_SIGNAL_EXPORTER_HH

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/bpf.h>

#include "aether_slot.h"
#include "signal_exporter.hh"

namespace cellular_emulation {

class BpfQueueExporter : public SignalExporter {
public:
    BpfQueueExporter()
        : map_fd_(-1),
          pushed_(0),
          overwritten_(0),
          errors_(0),
          last_log_pushed_(0),
          mutex_()
    {
        union bpf_attr attr;
        std::memset(&attr, 0, sizeof(attr));
        attr.pathname = reinterpret_cast<uint64_t>(AETHER_SIGNAL_PIN_PATH);
        map_fd_ = static_cast<int>(::syscall(__NR_bpf, BPF_OBJ_GET, &attr,
                                             sizeof(attr)));
        if (map_fd_ < 0) {
            fprintf(stderr,
                    "[aether] BpfQueueExporter: BPF_OBJ_GET(%s) failed: %s "
                    "(is aether-map-setup run and %s world-writable?)\n",
                    AETHER_SIGNAL_PIN_PATH, std::strerror(errno),
                    AETHER_SIGNAL_PIN_PATH);
        } else {
            fprintf(stderr,
                    "[aether] BpfQueueExporter: attached to %s fd=%d\n",
                    AETHER_SIGNAL_PIN_PATH, map_fd_);
        }
    }

    ~BpfQueueExporter() override
    {
        if (map_fd_ >= 0) ::close(map_fd_);
        fprintf(stderr,
                "[aether] BpfQueueExporter: pushed=%llu overwritten=%llu "
                "errors=%llu\n",
                (unsigned long long)pushed_.load(),
                (unsigned long long)overwritten_.load(),
                (unsigned long long)errors_.load());
    }

    bool export_signal(const QueueSignal& sig) override
    {
        if (map_fd_ < 0) return false;

        struct aether_slot slot;
        slot.seq        = sig.sample_seq;
        slot.tstamp_ns  = static_cast<uint64_t>(sig.timestamp_ms) * 1000000ULL;
        slot.qlen       = static_cast<uint32_t>(sig.sample_queue_length);
        slot.live_qlen  = static_cast<uint32_t>(sig.live_queue_length);
        slot.dwell_ms   = static_cast<uint32_t>(sig.sample_dwell_time_ms);
        slot.delta_dwell = sig.sample_delta_dwell;

        /* ARRAY map with N entries: round-robin write.
         * key = seq % MAX_ENTRIES ensures the most recent
         * AETHER_SIGNAL_MAX_ENTRIES samples are always available. */
        static constexpr uint32_t kMaxEntries = AETHER_SIGNAL_MAX_ENTRIES;
        __u32 key = static_cast<__u32>(sig.sample_seq % kMaxEntries);

        union bpf_attr attr;
        std::memset(&attr, 0, sizeof(attr));
        attr.map_fd = map_fd_;
        attr.key    = reinterpret_cast<uint64_t>(&key);
        attr.value  = reinterpret_cast<uint64_t>(&slot);
        attr.flags  = BPF_ANY;

        std::lock_guard<std::mutex> lock(mutex_);
        int err = static_cast<int>(::syscall(__NR_bpf,
                                             BPF_MAP_UPDATE_ELEM,
                                             &attr, sizeof(attr)));
        if (err == 0) {
            uint64_t n = ++pushed_;
            ++overwritten_;   /* ARRAY: every update overwrites previous */
            /* Periodic heartbeat so operator can confirm exporter is live. */
            if (n - last_log_pushed_ >= 2000) {
                last_log_pushed_ = n;
                fprintf(stderr,
                        "[aether] pushed=%llu overwritten=%llu "
                        "(last seq=%llu qlen=%u dwell=%u)\n",
                        (unsigned long long)n,
                        (unsigned long long)overwritten_.load(),
                        (unsigned long long)slot.seq,
                        slot.qlen, slot.dwell_ms);
            }
            return true;
        }

        /* ARRAY maps should never fail with E2BIG, but handle
         * any error gracefully. */
        if ((++errors_) <= 5) {
            fprintf(stderr,
                    "[aether] BPF_MAP_UPDATE_ELEM failed: %s (seq=%llu)\n",
                    std::strerror(errno),
                    (unsigned long long)slot.seq);
        }
        return false;
    }

    std::string type() const override { return "bpf_queue"; }
    bool is_ready() const override { return map_fd_ >= 0; }

private:
    int map_fd_;
    std::atomic<uint64_t> pushed_;
    std::atomic<uint64_t> overwritten_;
    std::atomic<uint64_t> errors_;
    uint64_t last_log_pushed_;
    std::mutex mutex_;
};

} // namespace cellular_emulation

#endif /* BPF_SIGNAL_EXPORTER_HH */
