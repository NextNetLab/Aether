/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef QUEUE_POLICY_HH
#define QUEUE_POLICY_HH

#include <cstdint>
#include <string>
#include <memory>
#include <deque>
#include <random>
#include <functional>
#include <sstream>
#include <cmath>

#include "queued_packet.hh"

namespace cellular_emulation {

class QueueDropPolicy {
public:
    virtual ~QueueDropPolicy() = default;
    virtual bool should_drop(size_t current_size, size_t max_size, 
                             size_t packet_size) = 0;
    virtual std::string name() const = 0;
    virtual void reset() {}
};

/* Drop when queue is full (default driver behavior) */
class TailDropPolicy : public QueueDropPolicy {
public:
    bool should_drop(size_t current_size, size_t max_size, 
                     size_t /* packet_size */) override
    {
        return current_size >= max_size;
    }
    
    std::string name() const override { return "TailDrop"; }
};

/* Random Early Detection (Floyd & Jacobson, 1993) */
class REDPolicy : public QueueDropPolicy {
public:
    REDPolicy(double min_thresh = 0.3, double max_thresh = 0.8,
              double max_p = 0.1, double w_q = 0.002)
        : min_thresh_(min_thresh),
          max_thresh_(max_thresh),
          max_p_(max_p),
          w_q_(w_q),
          avg_queue_(0.0),
          count_(0),
          rng_(std::random_device{}())
    {
    }
    
    bool should_drop(size_t current_size, size_t max_size, 
                     size_t /* packet_size */) override
    {
        if (max_size == 0) return false;
        
        double q = static_cast<double>(current_size) / static_cast<double>(max_size);
        avg_queue_ = w_q_ * q + (1.0 - w_q_) * avg_queue_;
        
        double drop_prob = 0.0;
        
        if (avg_queue_ < min_thresh_) {
            count_ = -1;
            drop_prob = 0.0;
        } else if (avg_queue_ < max_thresh_) {
            count_++;
            double pb = max_p_ * (avg_queue_ - min_thresh_) / (max_thresh_ - min_thresh_);
            drop_prob = pb / (1.0 - count_ * pb);
            drop_prob = std::max(0.0, std::min(1.0, drop_prob));
        } else {
            count_ = 0;
            drop_prob = 1.0;
        }
        
        if (drop_prob > 0.0) {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            if (dist(rng_) < drop_prob) {
                count_ = 0;
                return true;
            }
        }
        
        return false;
    }
    
    std::string name() const override { return "RED"; }
    
    void reset() override
    {
        avg_queue_ = 0.0;
        count_ = 0;
    }
    
    double avg_queue() const { return avg_queue_; }

private:
    double min_thresh_;
    double max_thresh_;
    double max_p_;
    double w_q_;
    double avg_queue_;
    int count_;
    std::mt19937 rng_;
};

/* Controlled Delay (Nichols & Jacobson, 2012) */
class CoDeLPolicy : public QueueDropPolicy {
public:
    CoDeLPolicy(uint64_t target_ms = 5, uint64_t interval_ms = 100)
        : target_ms_(target_ms),
          interval_ms_(interval_ms),
          first_above_time_(0),
          drop_next_(0),
          count_(0),
          dropping_(false),
          last_count_(0)
    {
    }
    
    bool should_drop_codel(uint64_t sojourn_time_ms, uint64_t current_time_ms)
    {
        bool ok_to_drop = sojourn_time_ms >= target_ms_;
        
        if (!ok_to_drop) {
            first_above_time_ = 0;
            return false;
        }
        
        if (first_above_time_ == 0) {
            first_above_time_ = current_time_ms + interval_ms_;
            return false;
        }
        
        if (current_time_ms >= first_above_time_) {
            if (!dropping_) {
                dropping_ = true;
                count_ = (count_ > 2 && current_time_ms - drop_next_ < 8 * interval_ms_) 
                         ? (count_ - 2) : 1;
                drop_next_ = current_time_ms;
                return true;
            }
            
            if (current_time_ms >= drop_next_) {
                count_++;
                drop_next_ = current_time_ms + interval_ms_ / static_cast<uint64_t>(std::sqrt(static_cast<double>(count_)));
                return true;
            }
        }
        
        return false;
    }
    
    bool should_drop(size_t current_size, size_t max_size, 
                     size_t /* packet_size */) override
    {
        if (current_size >= max_size) return true;
        
        double util = static_cast<double>(current_size) / static_cast<double>(max_size);
        return util > 0.9;
    }
    
    std::string name() const override { return "CoDel"; }
    
    void reset() override
    {
        first_above_time_ = 0;
        drop_next_ = 0;
        count_ = 0;
        dropping_ = false;
    }
    
    uint32_t count() const { return count_; }
    bool dropping() const { return dropping_; }

private:
    uint64_t target_ms_;
    uint64_t interval_ms_;
    uint64_t first_above_time_;
    uint64_t drop_next_;
    uint32_t count_;
    bool dropping_;
    uint32_t last_count_;
};

class ByteLimitPolicy : public QueueDropPolicy {
public:
    explicit ByteLimitPolicy(size_t max_bytes)
        : max_bytes_(max_bytes),
          current_bytes_(0)
    {
    }
    
    bool should_drop(size_t /* current_size */, size_t /* max_size */, 
                     size_t packet_size) override
    {
        return (current_bytes_ + packet_size) > max_bytes_;
    }
    
    std::string name() const override { return "ByteLimit"; }
    void reset() override { current_bytes_ = 0; }
    
    void add_bytes(size_t bytes) { current_bytes_ += bytes; }
    void remove_bytes(size_t bytes) { 
        current_bytes_ = (bytes > current_bytes_) ? 0 : (current_bytes_ - bytes);
    }

private:
    size_t max_bytes_;
    size_t current_bytes_;
};

class PriorityPolicy : public QueueDropPolicy {
public:
    void set_priority_extractor(std::function<int(const std::string&)> extractor)
    {
        priority_extractor_ = extractor;
    }
    
    bool should_drop(size_t current_size, size_t max_size, 
                     size_t /* packet_size */) override
    {
        return current_size >= max_size;
    }
    
    std::string name() const override { return "Priority"; }

private:
    std::function<int(const std::string&)> priority_extractor_;
};

class QueueDropPolicyFactory {
public:
    static std::unique_ptr<QueueDropPolicy> create(const std::string& name)
    {
        if (name == "taildrop" || name == "td") {
            return std::make_unique<TailDropPolicy>();
        } else if (name == "red") {
            return std::make_unique<REDPolicy>();
        } else if (name == "codel") {
            return std::make_unique<CoDeLPolicy>();
        } else if (name == "byte" || name == "bytelimit") {
            return std::make_unique<ByteLimitPolicy>(1024 * 1024);
        } else if (name == "priority") {
            return std::make_unique<PriorityPolicy>();
        }
        
        return std::make_unique<TailDropPolicy>();
    }
};

class SchedulingPolicy {
public:
    virtual ~SchedulingPolicy() = default;
    virtual size_t select_next(const std::deque<QueuedPacket>& queue) = 0;
    virtual std::string name() const = 0;
};

class FIFOScheduling : public SchedulingPolicy {
public:
    size_t select_next(const std::deque<QueuedPacket>& /* queue */) override
    {
        return 0;
    }
    
    std::string name() const override { return "FIFO"; }
};

class LIFOScheduling : public SchedulingPolicy {
public:
    size_t select_next(const std::deque<QueuedPacket>& queue) override
    {
        return queue.empty() ? 0 : (queue.size() - 1);
    }
    
    std::string name() const override { return "LIFO"; }
};

class ShortestJobFirstScheduling : public SchedulingPolicy {
public:
    size_t select_next(const std::deque<QueuedPacket>& queue) override
    {
        if (queue.empty()) return 0;
        
        size_t min_idx = 0;
        size_t min_size = queue[0].contents.size();
        
        for (size_t i = 1; i < queue.size(); ++i) {
            if (queue[i].contents.size() < min_size) {
                min_size = queue[i].contents.size();
                min_idx = i;
            }
        }
        
        return min_idx;
    }
    
    std::string name() const override { return "SJF"; }
};

} // namespace cellular_emulation

#endif /* QUEUE_POLICY_HH */
