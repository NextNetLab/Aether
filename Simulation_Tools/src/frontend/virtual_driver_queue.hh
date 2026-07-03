/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef VIRTUAL_DRIVER_QUEUE_HH
#define VIRTUAL_DRIVER_QUEUE_HH

#include <cstddef>
#include <deque>
#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>

#include "queued_packet.hh"
#include "vdqueue_config.hh"
#include "vdqueue_stats.hh"

namespace cellular_emulation {

class VirtualDriverQueue {
public:
    explicit VirtualDriverQueue(const VDQueueConfig& config);
    VirtualDriverQueue();
    ~VirtualDriverQueue();

    VirtualDriverQueue(const VirtualDriverQueue&) = delete;
    VirtualDriverQueue& operator=(const VirtualDriverQueue&) = delete;

    VirtualDriverQueue(VirtualDriverQueue&&) noexcept;
    VirtualDriverQueue& operator=(VirtualDriverQueue&&) noexcept;

    bool enqueue(const std::string& packet, uint64_t arrival_time);
    bool enqueue(std::string&& packet, uint64_t arrival_time);
    QueuedPacket dequeue();
    /* Rollback helper: place a previously-dequeued packet back at the head.
     * Does not affect enqueue/drop stats and bypasses capacity checks, since
     * the packet was already accounted for as in-queue prior to dequeue(). */
    void push_front(QueuedPacket&& packet);
    const QueuedPacket& front() const;

    bool empty() const noexcept;
    bool full() const noexcept;
    size_t size() const noexcept;
    size_t size_bytes() const noexcept;
    size_t capacity() const noexcept;

    uint64_t head_dwell_time(uint64_t current_time) const;
    double average_dwell_time(uint64_t current_time) const;

    const VDQueueStats& statistics() const noexcept;
    void reset_statistics();

    void set_drop_callback(std::function<void(size_t, size_t)> callback);
    void set_enqueue_callback(std::function<void(uint64_t, size_t)> callback);

    std::string to_string() const;
    void update_config(const VDQueueConfig& config);

private:
    std::deque<QueuedPacket> queue_;
    VDQueueConfig config_;
    VDQueueStats stats_;
    size_t total_bytes_;

    std::function<void(size_t, size_t)> drop_callback_;
    std::function<void(uint64_t, size_t)> enqueue_callback_;

    mutable std::mutex mutex_;

    void record_enqueue(uint64_t time, size_t bytes);
    void record_drop(size_t packets, size_t bytes);
    void update_statistics(uint64_t current_time);
};

} // namespace cellular_emulation

#endif /* VIRTUAL_DRIVER_QUEUE_HH */
