/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef SIGNAL_EXPORTER_HH
#define SIGNAL_EXPORTER_HH

#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <fstream>
#include <sstream>
#include <atomic>
#include <mutex>
#include <vector>

namespace cellular_emulation {

struct QueueSignal {
    uint64_t sample_seq;
    uint64_t timestamp_ms;
    size_t sample_queue_length;
    uint64_t sample_dwell_time_ms;
    int32_t sample_delta_dwell;     /* ΔD_k = D_k - D_{k-1} in ms (paper §4.4) */
    size_t live_queue_length;
    size_t modem_queue_length;
    size_t total_queue_length;
    bool drain_complete;
    bool backpressure_active;

    QueueSignal()
        : sample_seq(0),
          timestamp_ms(0),
          sample_queue_length(0),
          sample_dwell_time_ms(0),
          sample_delta_dwell(0),
          live_queue_length(0),
          modem_queue_length(0),
          total_queue_length(0),
          drain_complete(true),
          backpressure_active(false)
    {
    }
    
    std::string to_simple_string() const
    {
        std::ostringstream oss;
        oss << sample_seq << " "
            << sample_queue_length << " "
            << sample_dwell_time_ms << " "
            << live_queue_length;
        return oss.str();
    }
    
    std::string to_string() const
    {
        std::ostringstream oss;
        oss << "QueueSignal{"
            << "seq=" << sample_seq
            << ", sample_len=" << sample_queue_length
            << ", dwell=" << sample_dwell_time_ms << "ms"
            << ", live_len=" << live_queue_length
            << ", modem_len=" << modem_queue_length
            << ", ts=" << timestamp_ms
            << ", drain=" << (drain_complete ? "Y" : "N")
            << ", backpressure=" << (backpressure_active ? "Y" : "N")
            << "}";
        return oss.str();
    }
    
    std::string to_csv_line() const
    {
        std::ostringstream oss;
        oss << timestamp_ms << ","
            << sample_seq << ","
            << sample_queue_length << ","
            << sample_dwell_time_ms << ","
            << sample_delta_dwell << ","
            << live_queue_length << ","
            << modem_queue_length << ","
            << total_queue_length << ","
            << (drain_complete ? 1 : 0) << ","
            << (backpressure_active ? 1 : 0);
        return oss.str();
    }
};

class SignalExporter {
public:
    virtual ~SignalExporter() = default;
    virtual bool export_signal(const QueueSignal& signal) = 0;
    virtual std::string type() const = 0;
    virtual bool is_ready() const = 0;
};

class FileSignalExporter : public SignalExporter {
public:
    explicit FileSignalExporter(const std::string& filepath,
                                const std::string& format = "simple")
        : filepath_(filepath),
          format_(format),
          min_interval_ms_(1),
          last_export_time_(0),
          export_count_(0),
          file_(filepath, std::ios::out | std::ios::trunc),
          mutex_()
    {
    }

    ~FileSignalExporter() override
    {
        if (file_.is_open()) {
            file_.close();
        }
    }

    bool export_signal(const QueueSignal& signal) override
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (signal.timestamp_ms - last_export_time_ < min_interval_ms_) {
            return true;
        }

        if (!file_.is_open()) {
            return false;
        }

        /* Seek to beginning to overwrite previous content,
         * maintaining the same single-line overwrite semantics
         * as the original per-call open/close approach. */
        file_.seekp(0, std::ios::beg);
        file_.clear();

        if (format_ == "simple") {
            file_ << signal.to_simple_string() << std::endl;
        } else {
            file_ << signal.to_string() << std::endl;
        }

        file_.flush();

        last_export_time_ = signal.timestamp_ms;
        export_count_++;

        return true;
    }

    std::string type() const override { return "file"; }
    bool is_ready() const override { return file_.is_open(); }

    void set_min_interval(uint64_t interval_ms)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        min_interval_ms_ = interval_ms;
    }

    uint64_t export_count() const { return export_count_; }

private:
    std::string filepath_;
    std::string format_;
    uint64_t min_interval_ms_;
    uint64_t last_export_time_;
    std::atomic<uint64_t> export_count_;
    std::ofstream file_;
    std::mutex mutex_;
};

class CSVSignalLogger : public SignalExporter {
public:
    explicit CSVSignalLogger(const std::string& filepath,
                             bool write_header = true)
        : filepath_(filepath),
          file_(filepath, std::ios::out | std::ios::app),
          signal_count_(0),
          mutex_()
    {
        if (write_header && file_.is_open()) {
            file_ << "timestamp_ms,sample_seq,sample_queue_length,"
                  << "sample_dwell_time_ms,sample_delta_dwell,"
                  << "live_queue_length,"
                  << "modem_queue_length,total_queue_length,"
                  << "drain_complete,backpressure\n";
        }
    }
    
    ~CSVSignalLogger() override
    {
        if (file_.is_open()) {
            file_.close();
        }
    }
    
    bool export_signal(const QueueSignal& signal) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (!file_.is_open()) {
            return false;
        }
        
        file_ << signal.to_csv_line() << "\n";
        signal_count_++;
        
        if (signal_count_ % 100 == 0) {
            file_.flush();
        }
        
        return true;
    }
    
    std::string type() const override { return "csv_logger"; }
    bool is_ready() const override { return file_.is_open(); }
    
    void flush()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.flush();
        }
    }
    
    uint64_t signal_count() const { return signal_count_; }

private:
    std::string filepath_;
    std::ofstream file_;
    std::atomic<uint64_t> signal_count_;
    std::mutex mutex_;
};

class MultiExporter : public SignalExporter {
public:
    MultiExporter() : exporters_() {}
    
    void add_exporter(std::shared_ptr<SignalExporter> exporter)
    {
        exporters_.push_back(exporter);
    }
    
    bool export_signal(const QueueSignal& signal) override
    {
        bool all_success = true;
        for (auto& exporter : exporters_) {
            if (!exporter->export_signal(signal)) {
                all_success = false;
            }
        }
        return all_success;
    }
    
    std::string type() const override { return "multi"; }
    
    bool is_ready() const override
    {
        for (const auto& exporter : exporters_) {
            if (!exporter->is_ready()) {
                return false;
            }
        }
        return true;
    }
    
    size_t exporter_count() const { return exporters_.size(); }

private:
    std::vector<std::shared_ptr<SignalExporter>> exporters_;
};

class SignalExporterFactory {
public:
    static std::shared_ptr<SignalExporter> create(
        const std::string& type_str,
        const std::string& config = "/tmp/mm_virtual_driver_queue");
};

} // namespace cellular_emulation

#endif /* SIGNAL_EXPORTER_HH */
