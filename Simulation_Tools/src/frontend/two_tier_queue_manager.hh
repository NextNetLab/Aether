/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef TWO_TIER_QUEUE_MANAGER_HH
#define TWO_TIER_QUEUE_MANAGER_HH

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <functional>

#include "virtual_driver_queue.hh"
#include "virtual_modem_buffer.hh"
#include "signal_exporter.hh"
#include "queued_packet.hh"

namespace cellular_emulation {

static const size_t AETHER_SAMPLE_RING_SIZE = 32;

struct AetherQueueSample {
    uint64_t sample_seq;
    uint64_t timestamp_ms;
    uint64_t dwell_time_ms;
    size_t sample_queue_length;
    size_t live_queue_length;
    size_t modem_queue_length;
    size_t total_queue_length;
    bool drain_complete;
    bool backpressure_active;
    bool valid;

    AetherQueueSample()
        : sample_seq(0),
          timestamp_ms(0),
          dwell_time_ms(0),
          sample_queue_length(0),
          live_queue_length(0),
          modem_queue_length(0),
          total_queue_length(0),
          drain_complete(true),
          backpressure_active(false),
          valid(false)
    {
    }
};

struct TwoTierQueueConfig {
    VDQueueConfig driver_queue_config;
    VModemConfig modem_buffer_config;
    std::string signal_export_path;
    bool enable_signal_export;
    bool enable_csv_logging;
    std::string csv_log_path;
    /* "file" | "csv" | "bpf_queue" | "multi" (bpf + csv). Empty = pick
     * automatically from enable_csv_logging (backwards compatible). */
    std::string signal_exporter_type;

    TwoTierQueueConfig()
        : driver_queue_config(),
          modem_buffer_config(),
          signal_export_path("/tmp/mm_virtual_driver_queue"),
          enable_signal_export(true),
          enable_csv_logging(false),
          csv_log_path("/tmp/mm_vdqueue_log.csv"),
          signal_exporter_type()
    {
    }
};

/*
 * Packet flow:
 *   Application -> VDQueue (driver layer) -> VModemBuf (modem) -> Link
 *
 * Backpressure flow:
 *   Link <- VModemBuf backpressure <- VDQueue buildup <- Application
 */
class TwoTierQueueManager {
public:
    explicit TwoTierQueueManager(const TwoTierQueueConfig& config);
    TwoTierQueueManager();
    ~TwoTierQueueManager();

    TwoTierQueueManager(const TwoTierQueueManager&) = delete;
    TwoTierQueueManager& operator=(const TwoTierQueueManager&) = delete;

    bool receive_packet(const std::string& packet, uint64_t arrival_time);
    size_t try_move_pkt(uint64_t current_time);
    QueuedPacket drain_packet(uint64_t current_time);
    void update(uint64_t current_time);

    size_t driver_queue_size() const;
    size_t modem_buffer_size() const;
    size_t total_queued_packets() const;
    bool is_backpressure_active() const;
    uint64_t driver_queue_dwell_time(uint64_t current_time) const;

    const VDQueueStats& driver_queue_statistics() const;
    const AetherQueueSample& latest_sample() const;
    std::string statistics_summary() const;
    void reset_statistics();

    void export_signal_now(uint64_t current_time);
    void set_drop_callback(std::function<void(size_t, size_t)> callback);
    std::string to_string() const;

private:
    std::unique_ptr<VirtualDriverQueue> driver_queue_;
    std::unique_ptr<VirtualModemBuffer> modem_buffer_;
    std::shared_ptr<SignalExporter> signal_exporter_;

    TwoTierQueueConfig config_;

    uint64_t last_update_time_;
    uint64_t last_signal_export_time_;

    uint64_t total_packets_received_;
    uint64_t total_packets_transferred_;
    uint64_t total_backpressure_events_;
    uint64_t next_sample_seq_;
    uint64_t last_transfer_time_;     /* ms, for delta_dwell calculation */
    int32_t last_delta_dwell_;        /* ΔD_k from last actual transfer */
    size_t sample_ring_head_;
    size_t sample_ring_count_;
    std::array<AetherQueueSample, AETHER_SAMPLE_RING_SIZE> sample_ring_;
    AetherQueueSample latest_sample_;

    std::function<void(size_t, size_t)> drop_callback_;

    void initialize();
    void record_driver_sample(uint64_t current_time, uint64_t dwell_time);
    void export_signal(uint64_t current_time, int32_t delta_dwell = 0);
    void handle_backpressure_change(bool active);
};

} // namespace cellular_emulation

#endif /* TWO_TIER_QUEUE_MANAGER_HH */
