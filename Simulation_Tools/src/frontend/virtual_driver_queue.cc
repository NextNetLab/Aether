#include "virtual_driver_queue.hh"

#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>
#include <numeric>
#include <utility>

namespace cellular_emulation {

VirtualDriverQueue::VirtualDriverQueue(const VDQueueConfig& config)
    : queue_(),
      config_(config),
      stats_(),
      total_bytes_(0),
      drop_callback_(nullptr),
      enqueue_callback_(nullptr),
      mutex_()
{
    stats_.reset();
}

VirtualDriverQueue::VirtualDriverQueue()
    : VirtualDriverQueue(VDQueueConfig())
{
}

VirtualDriverQueue::~VirtualDriverQueue()
{
    // Cleanup if needed
}

VirtualDriverQueue::VirtualDriverQueue(VirtualDriverQueue&& other) noexcept
    : queue_(std::move(other.queue_)),
      config_(std::move(other.config_)),
      stats_(std::move(other.stats_)),
      total_bytes_(other.total_bytes_),
      drop_callback_(std::move(other.drop_callback_)),
      enqueue_callback_(std::move(other.enqueue_callback_)),
      mutex_()
{
    other.total_bytes_ = 0;
}

VirtualDriverQueue& VirtualDriverQueue::operator=(VirtualDriverQueue&& other) noexcept
{
    if (this != &other) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_ = std::move(other.queue_);
        config_ = std::move(other.config_);
        stats_ = std::move(other.stats_);
        total_bytes_ = other.total_bytes_;
        drop_callback_ = std::move(other.drop_callback_);
        enqueue_callback_ = std::move(other.enqueue_callback_);
        other.total_bytes_ = 0;
    }
    return *this;
}

bool VirtualDriverQueue::enqueue(const std::string& packet, uint64_t arrival_time)
{
    std::lock_guard<std::mutex> lock(mutex_);

    /* Check packet size constraints */
    if (packet.size() > config_.max_packet_size) {
        record_drop(1, packet.size());
        return false;
    }

    /* Check queue capacity - implement tail drop policy */
    if (queue_.size() >= config_.max_queue_size) {
        /*
         * Backpressure condition: Queue is full
         * This simulates the scenario where the modem cannot accept more data,
         * causing packets to be dropped at the driver layer.
         */
        record_drop(1, packet.size());
        return false;
    }

    /* Check byte limit if configured */
    if (config_.max_queue_bytes > 0 && 
        total_bytes_ + packet.size() > config_.max_queue_bytes) {
        record_drop(1, packet.size());
        return false;
    }

    /* Enqueue the packet */
    queue_.emplace_back(packet, arrival_time);
    total_bytes_ += packet.size();

    /* Update statistics and trigger callback */
    record_enqueue(arrival_time, packet.size());

    return true;
}

bool VirtualDriverQueue::enqueue(std::string&& packet, uint64_t arrival_time)
{
    std::lock_guard<std::mutex> lock(mutex_);

    size_t packet_size = packet.size();

    /* Check packet size constraints */
    if (packet_size > config_.max_packet_size) {
        record_drop(1, packet_size);
        return false;
    }

    /* Check queue capacity - implement tail drop policy */
    if (queue_.size() >= config_.max_queue_size) {
        record_drop(1, packet_size);
        return false;
    }

    /* Check byte limit if configured */
    if (config_.max_queue_bytes > 0 && 
        total_bytes_ + packet_size > config_.max_queue_bytes) {
        record_drop(1, packet_size);
        return false;
    }

    /* Enqueue the packet using move semantics */
    queue_.emplace_back(std::move(packet), arrival_time);
    total_bytes_ += packet_size;

    /* Update statistics and trigger callback */
    record_enqueue(arrival_time, packet_size);

    return true;
}

QueuedPacket VirtualDriverQueue::dequeue()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (queue_.empty()) {
        return QueuedPacket("", 0);
    }

    QueuedPacket packet = std::move(queue_.front());
    queue_.pop_front();

    /* Update byte counter */
    total_bytes_ -= packet.contents.size();

    /* Update statistics */
    stats_.total_dequeued_packets++;
    stats_.total_dequeued_bytes += packet.contents.size();

    return packet;
}

void VirtualDriverQueue::push_front(QueuedPacket&& packet)
{
    std::lock_guard<std::mutex> lock(mutex_);

    size_t packet_size = packet.contents.size();

    /* Reverse the bookkeeping that dequeue() applied: this packet is being
     * returned to the queue because a downstream consumer rejected it. */
    total_bytes_ += packet_size;
    if (stats_.total_dequeued_packets > 0) {
        stats_.total_dequeued_packets--;
    }
    if (stats_.total_dequeued_bytes >= packet_size) {
        stats_.total_dequeued_bytes -= packet_size;
    }

    queue_.push_front(std::move(packet));
}

const QueuedPacket& VirtualDriverQueue::front() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (queue_.empty()) {
        throw std::runtime_error("VirtualDriverQueue::front() called on empty queue");
    }

    return queue_.front();
}

bool VirtualDriverQueue::empty() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

bool VirtualDriverQueue::full() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() >= config_.max_queue_size;
}

size_t VirtualDriverQueue::size() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

size_t VirtualDriverQueue::size_bytes() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return total_bytes_;
}

size_t VirtualDriverQueue::capacity() const noexcept
{
    return config_.max_queue_size;
}

uint64_t VirtualDriverQueue::head_dwell_time(uint64_t current_time) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (queue_.empty()) {
        return 0;
    }

    const QueuedPacket& head = queue_.front();
    
    /* Calculate time since packet entered the queue */
    if (current_time > head.arrival_time) {
        return current_time - head.arrival_time;
    }

    return 0;
}

double VirtualDriverQueue::average_dwell_time(uint64_t current_time) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (queue_.empty()) {
        return 0.0;
    }

    uint64_t total_dwell = 0;
    for (const auto& packet : queue_) {
        if (current_time > packet.arrival_time) {
            total_dwell += (current_time - packet.arrival_time);
        }
    }

    return static_cast<double>(total_dwell) / static_cast<double>(queue_.size());
}

const VDQueueStats& VirtualDriverQueue::statistics() const noexcept
{
    return stats_;
}

void VirtualDriverQueue::reset_statistics()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.reset();
}

void VirtualDriverQueue::set_drop_callback(std::function<void(size_t, size_t)> callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    drop_callback_ = callback;
}

void VirtualDriverQueue::set_enqueue_callback(std::function<void(uint64_t, size_t)> callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    enqueue_callback_ = callback;
}

std::string VirtualDriverQueue::to_string() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::ostringstream oss;
    oss << "VirtualDriverQueue{"
        << "size=" << queue_.size()
        << "/" << config_.max_queue_size
        << ", bytes=" << total_bytes_
        << ", dropped=" << stats_.total_dropped_packets
        << "}";

    return oss.str();
}

void VirtualDriverQueue::update_config(const VDQueueConfig& config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

void VirtualDriverQueue::record_enqueue(uint64_t time, size_t bytes)
{
    stats_.total_enqueued_packets++;
    stats_.total_enqueued_bytes += bytes;

    /* Update peak queue size */
    if (queue_.size() > stats_.peak_queue_size) {
        stats_.peak_queue_size = queue_.size();
    }

    /* Update peak queue bytes */
    if (total_bytes_ > stats_.peak_queue_bytes) {
        stats_.peak_queue_bytes = total_bytes_;
    }

    /* Trigger callback if registered */
    if (enqueue_callback_) {
        enqueue_callback_(time, bytes);
    }
}

void VirtualDriverQueue::record_drop(size_t packets, size_t bytes)
{
    stats_.total_dropped_packets += packets;
    stats_.total_dropped_bytes += bytes;

    /* Trigger callback if registered */
    if (drop_callback_) {
        drop_callback_(packets, bytes);
    }
}

void VirtualDriverQueue::update_statistics(uint64_t current_time)
{
    /* Calculate current dwell time statistics */
    if (!queue_.empty()) {
        uint64_t head_dwell = head_dwell_time(current_time);
        if (head_dwell > stats_.max_dwell_time) {
            stats_.max_dwell_time = head_dwell;
        }
    }
}

} // namespace cellular_emulation
