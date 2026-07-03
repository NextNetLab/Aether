/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef VIRTUAL_MODEM_BUFFER_HH
#define VIRTUAL_MODEM_BUFFER_HH

#include <cstddef>
#include <queue>
#include <deque>
#include <cstdint>
#include <string>
#include <memory>
#include <functional>

#include "queued_packet.hh"
#include "vmodem_config.hh"

namespace cellular_emulation {

class VirtualModemBuffer {
public:
    explicit VirtualModemBuffer(const VModemConfig& config);
    VirtualModemBuffer();
    ~VirtualModemBuffer();

    VirtualModemBuffer(const VirtualModemBuffer&) = delete;
    VirtualModemBuffer& operator=(const VirtualModemBuffer&) = delete;

    VirtualModemBuffer(VirtualModemBuffer&&) noexcept;
    VirtualModemBuffer& operator=(VirtualModemBuffer&&) noexcept;

    bool enqueue(QueuedPacket&& packet);
    QueuedPacket dequeue();

    bool can_accept() const noexcept;
    bool backpressure_active() const noexcept;
    bool empty() const noexcept;

    size_t size_packets() const noexcept;
    size_t size_bytes() const noexcept;
    size_t remaining_capacity() const noexcept;
    double utilization() const noexcept;

    void set_backpressure_callback(std::function<void(bool)> callback);
    std::string to_string() const;

    uint64_t total_enqueued() const noexcept { return total_enqueued_; }
    uint64_t total_dequeued() const noexcept { return total_dequeued_; }
    uint64_t total_rejected() const noexcept { return total_rejected_; }

private:
    std::deque<QueuedPacket> buffer_;
    VModemConfig config_;

    size_t total_bytes_;
    bool backpressure_state_;

    uint64_t total_enqueued_;
    uint64_t total_dequeued_;
    uint64_t total_rejected_;

    std::function<void(bool)> backpressure_callback_;

    void update_backpressure_state();
};

} // namespace cellular_emulation

#endif /* VIRTUAL_MODEM_BUFFER_HH */
