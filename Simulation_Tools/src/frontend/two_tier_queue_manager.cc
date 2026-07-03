#include "two_tier_queue_manager.hh"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace cellular_emulation {

TwoTierQueueManager::TwoTierQueueManager(const TwoTierQueueConfig& config)
    : driver_queue_(nullptr),
      modem_buffer_(nullptr),
      signal_exporter_(nullptr),
      config_(config),
      last_update_time_(0),
      last_signal_export_time_(0),
      total_packets_received_(0),
      total_packets_transferred_(0),
      total_backpressure_events_(0),
      next_sample_seq_(0),
      last_transfer_time_(0),
      last_delta_dwell_(0),
      sample_ring_head_(0),
      sample_ring_count_(0),
      sample_ring_(),
      latest_sample_(),
      drop_callback_(nullptr)
{
    initialize();
}

TwoTierQueueManager::TwoTierQueueManager()
    : TwoTierQueueManager(TwoTierQueueConfig())
{
}

TwoTierQueueManager::~TwoTierQueueManager()
{
    /* Cleanup handled by unique_ptr */
}

void TwoTierQueueManager::initialize()
{
    /* Create driver queue */
    driver_queue_ = std::make_unique<VirtualDriverQueue>(config_.driver_queue_config);

    /* Create modem buffer */
    modem_buffer_ = std::make_unique<VirtualModemBuffer>(config_.modem_buffer_config);

    /* Setup signal exporter */
    if (config_.enable_signal_export) {
        std::string type = config_.signal_exporter_type;
        std::string path = config_.signal_export_path;
        if (type.empty()) {
            type = config_.enable_csv_logging ? "multi" : "file";
        }
        if (type == "multi" || type == "csv") {
            path = config_.csv_log_path;
        }
        signal_exporter_ = SignalExporterFactory::create(type, path);
    }

    /* Wire up callbacks */
    modem_buffer_->set_backpressure_callback(
        [this](bool active) { this->handle_backpressure_change(active); }
    );

    /* Forward drop events */
    driver_queue_->set_drop_callback(
        [this](size_t packets, size_t bytes) {
            if (this->drop_callback_) {
                this->drop_callback_(packets, bytes);
            }
        }
    );
}

bool TwoTierQueueManager::receive_packet(const std::string& packet, uint64_t arrival_time)
{
    total_packets_received_++;

    /* 
     * Stage 1: Packet enters Driver Queue
     * 
     * In a real system, this corresponds to the application sending data,
     * which gets queued in the kernel's network driver transmit queue.
     */
    bool accepted = driver_queue_->enqueue(packet, arrival_time);

    if (!accepted) {
        /* 
         * Packet dropped at driver queue (tail drop)
         * This happens when the driver queue is full, which typically
         * occurs during severe backpressure from the modem.
         */
        return false;
    }

    /*
     * Stage 2: Attempt to transfer to Modem Buffer
     * 
     * Immediately try to move packets from driver queue to modem buffer.
     * In reality, this would be triggered by DMA completion interrupts.
     */
    try_move_pkt(arrival_time);

    return true;
}

size_t TwoTierQueueManager::try_move_pkt(uint64_t current_time)
{
    size_t transferred = 0;

    /*
     * Transfer loop: Move packets from driver queue to modem buffer
     * 
     * This simulates the DMA transfer from host memory (where the driver
     * queue resides) to device memory (the modem's internal buffer).
     * 
     * Transfer stops when:
     * 1. Driver queue is empty, OR
     * 2. Modem buffer is full (backpressure)
     */
    while (!driver_queue_->empty() && modem_buffer_->can_accept()) {
        /* Dequeue from driver queue */
        QueuedPacket packet = driver_queue_->dequeue();

        if (packet.contents.empty()) {
            /* Unexpected: empty packet from non-empty queue */
            break;
        }

        /* Enqueue to modem buffer */
        const uint64_t dwell_time = current_time > packet.arrival_time
                                  ? current_time - packet.arrival_time
                                  : 0;
        bool accepted = modem_buffer_->enqueue(std::move(packet));

        if (!accepted) {
            /* can_accept() only checks the packet-count limit, but enqueue()
             * may still reject on the byte-budget limit. Put the packet back
             * at the head of the driver queue so it is not lost, then stop:
             * retrying immediately would just dequeue the same packet again. */
            driver_queue_->push_front(std::move(packet));
            break;
        }

        record_driver_sample(current_time, dwell_time);
        transferred++;
        total_packets_transferred_++;
    }

    return transferred;
}

QueuedPacket TwoTierQueueManager::drain_packet(uint64_t current_time)
{
    /*
     * Drain a packet from modem buffer for transmission
     * 
     * This is called by the link layer when bandwidth is available
     * to transmit a packet.
     */
    QueuedPacket packet = modem_buffer_->dequeue();

    if (!packet.contents.empty()) {
        /*
         * Packet extracted successfully.
         * The modem buffer now has more space, so we should try
         * to refill it from the driver queue.
         */
        try_move_pkt(current_time);
    }

    return packet;
}

void TwoTierQueueManager::update(uint64_t current_time)
{
    last_update_time_ = current_time;

    /* Try to transfer any pending packets */
    try_move_pkt(current_time);

    /* Export signal periodically */
    uint64_t export_interval = config_.driver_queue_config.signal_export_interval_ms;
    if (current_time - last_signal_export_time_ >= export_interval) {
        export_signal(current_time, last_delta_dwell_);
        last_signal_export_time_ = current_time;
    }
}

void TwoTierQueueManager::record_driver_sample(uint64_t current_time,
                                               uint64_t dwell_time)
{
    AetherQueueSample sample;

    sample.sample_seq = next_sample_seq_++;
    sample.timestamp_ms = current_time;
    sample.dwell_time_ms = dwell_time > 0 ? dwell_time : 1;
    sample.live_queue_length = driver_queue_->size();
    sample.modem_queue_length = modem_buffer_->size_packets();
    sample.total_queue_length = sample.live_queue_length
                              + sample.modem_queue_length;
    sample.sample_queue_length = sample.live_queue_length;
    sample.drain_complete = sample.live_queue_length == 0;
    sample.backpressure_active = is_backpressure_active();
    sample.valid = true;

    /* Calculate ΔD_k = D_k - D_{k-1} (paper §4.4: dwell time difference
     * between consecutive packets, used for bottleneck detection and
     * sudden degradation detection) */
    int32_t delta_dwell = 0;
    if (last_transfer_time_ > 0) {
        delta_dwell = (int32_t)(dwell_time > 0 ? dwell_time : 1)
                    - (int32_t)last_transfer_time_;
    }
    last_transfer_time_ = dwell_time > 0 ? dwell_time : 1;

    latest_sample_ = sample;
    sample_ring_[sample_ring_head_] = sample;
    sample_ring_head_ = (sample_ring_head_ + 1) % AETHER_SAMPLE_RING_SIZE;
    if (sample_ring_count_ < AETHER_SAMPLE_RING_SIZE) {
        sample_ring_count_++;
    }

    /* Export signal immediately on every packet transfer so that
     * each sample_seq appears in the CSV log consecutively.
     * The FileSignalExporter's min_interval_ms_ throttle still
     * limits file writes to ~1 per ms, while the CSVSignalLogger
     * records every transfer.  Update last_signal_export_time_ to
     * avoid duplicate periodic exports in update(). */
    export_signal(current_time, delta_dwell);
    last_delta_dwell_ = delta_dwell;
    last_signal_export_time_ = current_time;
}

void TwoTierQueueManager::export_signal(uint64_t current_time,
                                        int32_t delta_dwell)
{
    if (!signal_exporter_ || !config_.enable_signal_export) {
        return;
    }

    /* Build signal */
    QueueSignal signal;
    signal.live_queue_length = driver_queue_->size();
    signal.modem_queue_length = modem_buffer_->size_packets();
    signal.total_queue_length = signal.live_queue_length
                              + signal.modem_queue_length;
    signal.drain_complete = signal.live_queue_length == 0;
    signal.backpressure_active = is_backpressure_active();

    if (latest_sample_.valid) {
        signal.sample_seq = latest_sample_.sample_seq;
        signal.timestamp_ms = current_time;
        signal.sample_queue_length = latest_sample_.sample_queue_length;
        signal.sample_dwell_time_ms = latest_sample_.dwell_time_ms;
        signal.sample_delta_dwell = delta_dwell;
    } else {
        signal.sample_seq = 0;
        signal.timestamp_ms = current_time;
        signal.sample_queue_length = signal.live_queue_length;
        signal.sample_dwell_time_ms = 0;
        signal.sample_delta_dwell = 0;
    }

    /* Export */
    signal_exporter_->export_signal(signal);
}

void TwoTierQueueManager::export_signal_now(uint64_t current_time)
{
    export_signal(current_time, 0);
    last_signal_export_time_ = current_time;
}

size_t TwoTierQueueManager::driver_queue_size() const
{
    return driver_queue_->size();
}

size_t TwoTierQueueManager::modem_buffer_size() const
{
    return modem_buffer_->size_packets();
}

size_t TwoTierQueueManager::total_queued_packets() const
{
    return driver_queue_->size() + modem_buffer_->size_packets();
}

bool TwoTierQueueManager::is_backpressure_active() const
{
    return modem_buffer_->backpressure_active();
}

uint64_t TwoTierQueueManager::driver_queue_dwell_time(uint64_t current_time) const
{
    return driver_queue_->head_dwell_time(current_time);
}

const VDQueueStats& TwoTierQueueManager::driver_queue_statistics() const
{
    return driver_queue_->statistics();
}

const AetherQueueSample& TwoTierQueueManager::latest_sample() const
{
    return latest_sample_;
}

std::string TwoTierQueueManager::statistics_summary() const
{
    std::ostringstream oss;
    
    oss << "=== Two-Tier Queue Manager Statistics ===\n\n";
    
    oss << "--- Driver Queue ---\n";
    oss << driver_queue_->statistics().to_string() << "\n";
    
    oss << "--- Modem Buffer ---\n";
    oss << "  Enqueued: " << modem_buffer_->total_enqueued() << " packets\n";
    oss << "  Transmitted: " << modem_buffer_->total_dequeued() << " packets\n";
    oss << "  Rejected: " << modem_buffer_->total_rejected() << " packets\n";
    oss << "  Current size: " << modem_buffer_->size_packets() << " packets\n";
    oss << "  Utilization: " << (modem_buffer_->utilization() * 100.0) << "%\n\n";
    
    oss << "--- Overall ---\n";
    oss << "  Total received: " << total_packets_received_ << " packets\n";
    oss << "  Total transferred: " << total_packets_transferred_ << " packets\n";
    oss << "  Backpressure events: " << total_backpressure_events_ << "\n";
    oss << "  Aether samples: " << sample_ring_count_ << " recent samples\n";
    
    return oss.str();
}

void TwoTierQueueManager::reset_statistics()
{
    driver_queue_->reset_statistics();
    total_packets_received_ = 0;
    total_packets_transferred_ = 0;
    total_backpressure_events_ = 0;
    next_sample_seq_ = 0;
    last_transfer_time_ = 0;
    last_delta_dwell_ = 0;
    sample_ring_head_ = 0;
    sample_ring_count_ = 0;
    sample_ring_ = std::array<AetherQueueSample, AETHER_SAMPLE_RING_SIZE>();
    latest_sample_ = AetherQueueSample();
}

void TwoTierQueueManager::set_drop_callback(std::function<void(size_t, size_t)> callback)
{
    drop_callback_ = callback;
    driver_queue_->set_drop_callback(callback);
}

void TwoTierQueueManager::handle_backpressure_change(bool active)
{
    if (active) {
        total_backpressure_events_++;
    }
}

std::string TwoTierQueueManager::to_string() const
{
    std::ostringstream oss;
    oss << "TwoTierQueueManager{"
        << "driver=" << driver_queue_->size()
        << "/" << driver_queue_->capacity()
        << ", modem=" << modem_buffer_->size_packets()
        << ", backpressure=" << (is_backpressure_active() ? "YES" : "no")
        << "}";
    return oss.str();
}

} // namespace cellular_emulation
