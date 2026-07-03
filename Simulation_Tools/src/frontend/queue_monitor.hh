/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef QUEUE_MONITOR_HH
#define QUEUE_MONITOR_HH

#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <mutex>
#include <memory>

namespace cellular_emulation {

struct QueueMetrics {
    uint64_t timestamp_ms = 0;
    
    size_t driver_queue_size = 0;
    size_t driver_queue_bytes = 0;
    uint64_t driver_head_dwell_ms = 0;
    
    size_t modem_buffer_size = 0;
    size_t modem_buffer_bytes = 0;
    
    uint64_t packets_received = 0;
    uint64_t packets_transferred = 0;
    uint64_t packets_transmitted = 0;
    uint64_t packets_dropped = 0;
    
    bool backpressure_active = false;
    
    std::string to_csv_line() const
    {
        std::ostringstream oss;
        oss << timestamp_ms << ","
            << driver_queue_size << ","
            << driver_queue_bytes << ","
            << driver_head_dwell_ms << ","
            << modem_buffer_size << ","
            << modem_buffer_bytes << ","
            << packets_received << ","
            << packets_transferred << ","
            << packets_transmitted << ","
            << packets_dropped << ","
            << (backpressure_active ? 1 : 0);
        return oss.str();
    }
    
    static std::string csv_header()
    {
        return "timestamp_ms,dq_size,dq_bytes,dq_dwell_ms,"
               "mb_size,mb_bytes,pkts_recv,pkts_xfer,pkts_tx,pkts_drop,bp";
    }
};

class Histogram {
public:
    Histogram(size_t num_bins, double min_value, double max_value)
        : num_bins_(num_bins),
          min_value_(min_value),
          max_value_(max_value),
          bin_width_((max_value - min_value) / static_cast<double>(num_bins)),
          bins_(num_bins, 0),
          count_(0),
          sum_(0.0),
          sum_sq_(0.0),
          min_seen_(std::numeric_limits<double>::max()),
          max_seen_(std::numeric_limits<double>::lowest())
    {
    }
    
    void add(double value)
    {
        count_++;
        sum_ += value;
        sum_sq_ += value * value;
        min_seen_ = std::min(min_seen_, value);
        max_seen_ = std::max(max_seen_, value);
        
        if (value < min_value_) {
            bins_[0]++;
        } else if (value >= max_value_) {
            bins_[num_bins_ - 1]++;
        } else {
            size_t bin = static_cast<size_t>((value - min_value_) / bin_width_);
            bin = std::min(bin, num_bins_ - 1);
            bins_[bin]++;
        }
    }
    
    size_t count() const { return count_; }
    
    double mean() const 
    { 
        return (count_ > 0) ? (sum_ / static_cast<double>(count_)) : 0.0; 
    }
    
    double stddev() const
    {
        if (count_ < 2) return 0.0;
        double n = static_cast<double>(count_);
        double variance = (sum_sq_ - (sum_ * sum_) / n) / (n - 1.0);
        return (variance > 0.0) ? std::sqrt(variance) : 0.0;
    }
    
    double min() const { return (count_ > 0) ? min_seen_ : 0.0; }
    double max() const { return (count_ > 0) ? max_seen_ : 0.0; }
    
    double percentile(double p) const
    {
        if (count_ == 0) return 0.0;
        
        size_t target = static_cast<size_t>(p * static_cast<double>(count_));
        size_t cumulative = 0;
        
        for (size_t i = 0; i < num_bins_; ++i) {
            cumulative += bins_[i];
            if (cumulative >= target) {
                return min_value_ + (static_cast<double>(i) + 0.5) * bin_width_;
            }
        }
        
        return max_value_;
    }
    
    const std::vector<size_t>& bins() const { return bins_; }
    
    void reset()
    {
        std::fill(bins_.begin(), bins_.end(), 0);
        count_ = 0;
        sum_ = 0.0;
        sum_sq_ = 0.0;
        min_seen_ = std::numeric_limits<double>::max();
        max_seen_ = std::numeric_limits<double>::lowest();
    }
    
    std::string to_ascii(int width = 40) const
    {
        if (count_ == 0) return "(empty histogram)\n";
        
        size_t max_count = *std::max_element(bins_.begin(), bins_.end());
        if (max_count == 0) return "(all zeros)\n";
        
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1);
        
        for (size_t i = 0; i < num_bins_; ++i) {
            double range_start = min_value_ + static_cast<double>(i) * bin_width_;
            double range_end = range_start + bin_width_;
            
            oss << std::setw(6) << range_start << "-" 
                << std::setw(6) << range_end << " |";
            
            int bar_len = static_cast<int>(
                static_cast<double>(bins_[i]) / static_cast<double>(max_count) * width);
            
            for (int j = 0; j < bar_len; ++j) {
                oss << "#";
            }
            
            oss << " (" << bins_[i] << ")\n";
        }
        
        return oss.str();
    }
    
    std::string summary() const
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "Histogram: n=" << count_
            << ", mean=" << mean()
            << ", stddev=" << stddev()
            << ", min=" << min()
            << ", max=" << max()
            << ", p50=" << percentile(0.5)
            << ", p95=" << percentile(0.95)
            << ", p99=" << percentile(0.99);
        return oss.str();
    }

private:
    size_t num_bins_;
    double min_value_;
    double max_value_;
    double bin_width_;
    std::vector<size_t> bins_;
    size_t count_;
    double sum_;
    double sum_sq_;
    double min_seen_;
    double max_seen_;
};

class QueueMonitor {
public:
    explicit QueueMonitor(uint64_t sampling_interval_ms = 10)
        : sampling_interval_ms_(sampling_interval_ms),
          last_sample_time_(0),
          metrics_log_(),
          dwell_time_histogram_(50, 0.0, 500.0),
          queue_size_histogram_(32, 0.0, 128.0),
          enabled_(true),
          mutex_()
    {
        metrics_log_.reserve(10000);
    }
    
    void record(const QueueMetrics& metrics)
    {
        if (!enabled_) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (metrics.timestamp_ms - last_sample_time_ < sampling_interval_ms_) {
            return;
        }
        last_sample_time_ = metrics.timestamp_ms;
        
        metrics_log_.push_back(metrics);
        
        dwell_time_histogram_.add(static_cast<double>(metrics.driver_head_dwell_ms));
        queue_size_histogram_.add(static_cast<double>(metrics.driver_queue_size));
    }
    
    const std::vector<QueueMetrics>& metrics_log() const
    {
        return metrics_log_;
    }
    
    const Histogram& dwell_time_histogram() const
    {
        return dwell_time_histogram_;
    }
    
    const Histogram& queue_size_histogram() const
    {
        return queue_size_histogram_;
    }
    
    void export_csv(const std::string& filepath) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::ofstream file(filepath);
        if (!file.is_open()) return;
        
        file << QueueMetrics::csv_header() << "\n";
        
        for (const auto& m : metrics_log_) {
            file << m.to_csv_line() << "\n";
        }
    }
    
    std::string generate_report() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::ostringstream oss;
        
        oss << "=== Queue Monitor Report ===\n\n";
        oss << "Samples: " << metrics_log_.size() << "\n";
        
        if (!metrics_log_.empty()) {
            uint64_t duration = metrics_log_.back().timestamp_ms - 
                               metrics_log_.front().timestamp_ms;
            oss << "Duration: " << (duration / 1000.0) << " seconds\n";
        }
        
        oss << "\n--- Dwell Time Distribution ---\n";
        oss << dwell_time_histogram_.summary() << "\n";
        oss << dwell_time_histogram_.to_ascii(30) << "\n";
        
        oss << "--- Queue Size Distribution ---\n";
        oss << queue_size_histogram_.summary() << "\n";
        oss << queue_size_histogram_.to_ascii(30) << "\n";
        
        if (metrics_log_.size() >= 2) {
            const auto& first = metrics_log_.front();
            const auto& last = metrics_log_.back();
            
            double duration_sec = static_cast<double>(
                last.timestamp_ms - first.timestamp_ms) / 1000.0;
            
            if (duration_sec > 0) {
                uint64_t total_tx = last.packets_transmitted - first.packets_transmitted;
                oss << "Average throughput: " 
                    << (static_cast<double>(total_tx) / duration_sec)
                    << " packets/sec\n";
            }
        }
        
        return oss.str();
    }
    
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        metrics_log_.clear();
        dwell_time_histogram_.reset();
        queue_size_histogram_.reset();
        last_sample_time_ = 0;
    }
    
    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool is_enabled() const { return enabled_; }

private:
    uint64_t sampling_interval_ms_;
    uint64_t last_sample_time_;
    std::vector<QueueMetrics> metrics_log_;
    Histogram dwell_time_histogram_;
    Histogram queue_size_histogram_;
    bool enabled_;
    mutable std::mutex mutex_;
};

class EventLogger {
public:
    enum class EventType {
        PACKET_ENQUEUE,
        PACKET_DEQUEUE,
        PACKET_DROP,
        BACKPRESSURE_ON,
        BACKPRESSURE_OFF,
        TRANSFER,
        ERROR
    };
    
    struct Event {
        uint64_t timestamp_ms;
        EventType type;
        std::string message;
        size_t value;
        
        std::string to_string() const
        {
            static const char* type_names[] = {
                "ENQUEUE", "DEQUEUE", "DROP", "BP_ON", "BP_OFF", "XFER", "ERROR"
            };
            
            std::ostringstream oss;
            oss << "[" << timestamp_ms << "] "
                << type_names[static_cast<int>(type)]
                << ": " << message;
            if (value > 0) {
                oss << " (value=" << value << ")";
            }
            return oss.str();
        }
    };
    
    explicit EventLogger(size_t max_events = 1000)
        : max_events_(max_events),
          events_(),
          enabled_(true),
          mutex_()
    {
    }
    
    void log(uint64_t timestamp, EventType type, 
             const std::string& message, size_t value = 0)
    {
        if (!enabled_) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        events_.push_back({timestamp, type, message, value});
        
        while (events_.size() > max_events_) {
            events_.pop_front();
        }
    }
    
    std::vector<Event> recent_events(size_t count) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::vector<Event> result;
        size_t start = (events_.size() > count) ? (events_.size() - count) : 0;
        
        for (size_t i = start; i < events_.size(); ++i) {
            result.push_back(events_[i]);
        }
        
        return result;
    }
    
    void export_log(const std::string& filepath) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::ofstream file(filepath);
        if (!file.is_open()) return;
        
        for (const auto& e : events_) {
            file << e.to_string() << "\n";
        }
    }
    
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.clear();
    }
    
    void set_enabled(bool enabled) { enabled_ = enabled; }

private:
    size_t max_events_;
    std::deque<Event> events_;
    bool enabled_;
    mutable std::mutex mutex_;
};

} // namespace cellular_emulation

#endif /* QUEUE_MONITOR_HH */
