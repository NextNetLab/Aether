#include "virtual_modem_buffer.hh"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace cellular_emulation {

VirtualModemBuffer::VirtualModemBuffer(const VModemConfig& config)
    : buffer_(),
      config_(config),
      total_bytes_(0),
      backpressure_state_(false),
      total_enqueued_(0),
      total_dequeued_(0),
      total_rejected_(0),
      backpressure_callback_(nullptr)
{
}

VirtualModemBuffer::VirtualModemBuffer()
    : VirtualModemBuffer(VModemConfig())
{
}

VirtualModemBuffer::~VirtualModemBuffer()
{
}

VirtualModemBuffer::VirtualModemBuffer(VirtualModemBuffer&& other) noexcept
    : buffer_(std::move(other.buffer_)),
      config_(std::move(other.config_)),
      total_bytes_(other.total_bytes_),
      backpressure_state_(other.backpressure_state_),
      total_enqueued_(other.total_enqueued_),
      total_dequeued_(other.total_dequeued_),
      total_rejected_(other.total_rejected_),
      backpressure_callback_(std::move(other.backpressure_callback_))
{
    other.total_bytes_ = 0;
    other.total_enqueued_ = 0;
    other.total_dequeued_ = 0;
    other.total_rejected_ = 0;
}

VirtualModemBuffer& VirtualModemBuffer::operator=(VirtualModemBuffer&& other) noexcept
{
    if (this != &other) {
        buffer_ = std::move(other.buffer_);
        config_ = std::move(other.config_);
        total_bytes_ = other.total_bytes_;
        backpressure_state_ = other.backpressure_state_;
        total_enqueued_ = other.total_enqueued_;
        total_dequeued_ = other.total_dequeued_;
        total_rejected_ = other.total_rejected_;
        backpressure_callback_ = std::move(other.backpressure_callback_);
        
        other.total_bytes_ = 0;
        other.total_enqueued_ = 0;
        other.total_dequeued_ = 0;
        other.total_rejected_ = 0;
    }
    return *this;
}

bool VirtualModemBuffer::enqueue(QueuedPacket&& packet)
{
    /* Check if we can accept the packet */
    if (buffer_.size() >= config_.max_buffer_size) {
        /* 
         * Backpressure condition: Buffer is full
         * This triggers the backpressure mechanism that prevents the driver
         * queue from transferring more packets.
         */
        total_rejected_++;
        update_backpressure_state();
        return false;
    }

    /* Check byte limit if configured */
    if (config_.max_buffer_bytes > 0 &&
        total_bytes_ + packet.contents.size() > config_.max_buffer_bytes) {
        total_rejected_++;
        update_backpressure_state();
        return false;
    }

    /* Accept the packet */
    size_t packet_size = packet.contents.size();
    buffer_.push_back(std::move(packet));
    total_bytes_ += packet_size;
    total_enqueued_++;

    /* Update backpressure state */
    update_backpressure_state();

    return true;
}

QueuedPacket VirtualModemBuffer::dequeue()
{
    if (buffer_.empty()) {
        return QueuedPacket("", 0);
    }

    QueuedPacket packet = std::move(buffer_.front());
    buffer_.pop_front();
    
    total_bytes_ -= packet.contents.size();
    total_dequeued_++;

    /* Update backpressure state - may release backpressure */
    update_backpressure_state();

    return packet;
}

bool VirtualModemBuffer::can_accept() const noexcept
{
    return buffer_.size() < config_.max_buffer_size;
}

bool VirtualModemBuffer::backpressure_active() const noexcept
{
    return backpressure_state_;
}

bool VirtualModemBuffer::empty() const noexcept
{
    return buffer_.empty();
}

size_t VirtualModemBuffer::size_packets() const noexcept
{
    return buffer_.size();
}

size_t VirtualModemBuffer::size_bytes() const noexcept
{
    return total_bytes_;
}

size_t VirtualModemBuffer::remaining_capacity() const noexcept
{
    if (buffer_.size() >= config_.max_buffer_size) {
        return 0;
    }
    return config_.max_buffer_size - buffer_.size();
}

double VirtualModemBuffer::utilization() const noexcept
{
    if (config_.max_buffer_size == 0) return 0.0;
    return static_cast<double>(buffer_.size()) / 
           static_cast<double>(config_.max_buffer_size);
}

void VirtualModemBuffer::set_backpressure_callback(std::function<void(bool)> callback)
{
    backpressure_callback_ = callback;
}

std::string VirtualModemBuffer::to_string() const
{
    std::ostringstream oss;
    oss << "VirtualModemBuffer{"
        << "size=" << buffer_.size()
        << "/" << config_.max_buffer_size
        << ", bytes=" << total_bytes_
        << ", backpressure=" << (backpressure_state_ ? "YES" : "no")
        << ", utilization=" << (utilization() * 100.0) << "%"
        << "}";
    return oss.str();
}

void VirtualModemBuffer::update_backpressure_state()
{
    bool new_state = (buffer_.size() >= config_.max_buffer_size);
    
    /* Check if state changed */
    if (new_state != backpressure_state_) {
        backpressure_state_ = new_state;
        
        /* Trigger callback if registered */
        if (backpressure_callback_) {
            backpressure_callback_(backpressure_state_);
        }
    }
}

} // namespace cellular_emulation
