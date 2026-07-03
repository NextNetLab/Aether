/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef VDQUEUE_CONFIG_HH
#define VDQUEUE_CONFIG_HH

#include <cstddef>
#include <cstdint>
#include <string>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <map>

namespace cellular_emulation {

struct VDQueueConfig {
    size_t max_queue_size;
    size_t max_queue_bytes;              /* 0 = packet-count limit only */
    size_t max_packet_size;              /* standard TUN MTU */
    bool enable_logging;
    std::string log_file_path;
    uint64_t signal_export_interval_ms;
    std::string signal_export_path;
    bool track_timestamps;
    bool collect_statistics;

    VDQueueConfig()
        : max_queue_size(128),
          max_queue_bytes(0),
          max_packet_size(1504),
          enable_logging(false),
          log_file_path(),
          signal_export_interval_ms(1),
          signal_export_path("/tmp/mm_virtual_driver_queue"),
          track_timestamps(true),
          collect_statistics(true)
    {
    }

    explicit VDQueueConfig(const std::string& config_path)
        : VDQueueConfig()
    {
        load_from_file(config_path);
    }

    void load_from_file(const std::string& config_path)
    {
        std::ifstream file(config_path);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot open config file: " + config_path);
        }

        std::string line;
        int line_number = 0;
        
        while (std::getline(file, line)) {
            line_number++;
            
            if (line.empty() || line[0] == '#') {
                continue;
            }

            size_t eq_pos = line.find('=');
            if (eq_pos == std::string::npos) {
                throw std::runtime_error("Invalid config line " + 
                    std::to_string(line_number) + ": " + line);
            }

            std::string key = line.substr(0, eq_pos);
            std::string value = line.substr(eq_pos + 1);

            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            set_value(key, value);
        }
    }

    void save_to_file(const std::string& config_path) const
    {
        std::ofstream file(config_path);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot open config file for writing: " + config_path);
        }

        file << "# Virtual Driver Queue Configuration\n\n";

        file << "max_queue_size=" << max_queue_size << "\n";
        file << "max_queue_bytes=" << max_queue_bytes << "\n";
        file << "max_packet_size=" << max_packet_size << "\n";
        file << "enable_logging=" << (enable_logging ? "true" : "false") << "\n";
        file << "log_file_path=" << log_file_path << "\n";
        file << "signal_export_interval_ms=" << signal_export_interval_ms << "\n";
        file << "signal_export_path=" << signal_export_path << "\n";
        file << "track_timestamps=" << (track_timestamps ? "true" : "false") << "\n";
        file << "collect_statistics=" << (collect_statistics ? "true" : "false") << "\n";
    }

    std::string to_string() const
    {
        std::ostringstream oss;
        oss << "VDQueueConfig{"
            << "max_queue_size=" << max_queue_size
            << ", max_queue_bytes=" << max_queue_bytes
            << ", max_packet_size=" << max_packet_size
            << ", signal_export_path=" << signal_export_path
            << "}";
        return oss.str();
    }

    bool validate() const
    {
        if (max_queue_size == 0) {
            throw std::runtime_error("max_queue_size must be > 0");
        }
        if (max_packet_size == 0) {
            throw std::runtime_error("max_packet_size must be > 0");
        }
        if (max_packet_size > 65535) {
            throw std::runtime_error("max_packet_size must be <= 65535");
        }
        return true;
    }

private:
    void set_value(const std::string& key, const std::string& value)
    {
        if (key == "max_queue_size") {
            max_queue_size = std::stoull(value);
        } else if (key == "max_queue_bytes") {
            max_queue_bytes = std::stoull(value);
        } else if (key == "max_packet_size") {
            max_packet_size = std::stoull(value);
        } else if (key == "enable_logging") {
            enable_logging = (value == "true" || value == "1");
        } else if (key == "log_file_path") {
            log_file_path = value;
        } else if (key == "signal_export_interval_ms") {
            signal_export_interval_ms = std::stoull(value);
        } else if (key == "signal_export_path") {
            signal_export_path = value;
        } else if (key == "track_timestamps") {
            track_timestamps = (value == "true" || value == "1");
        } else if (key == "collect_statistics") {
            collect_statistics = (value == "true" || value == "1");
        }
    }
};

namespace ModemProfiles {

inline VDQueueConfig qualcomm_sdx55()
{
    VDQueueConfig config;
    config.max_queue_size = 128;
    config.max_packet_size = 1504;
    config.signal_export_interval_ms = 1;
    return config;
}

inline VDQueueConfig intel_xmm7560()
{
    VDQueueConfig config;
    config.max_queue_size = 64;
    config.max_packet_size = 1504;
    config.signal_export_interval_ms = 2;
    return config;
}

inline VDQueueConfig samsung_exynos()
{
    VDQueueConfig config;
    config.max_queue_size = 96;
    config.max_packet_size = 1504;
    config.signal_export_interval_ms = 1;
    return config;
}

inline VDQueueConfig generic_lte()
{
    VDQueueConfig config;
    config.max_queue_size = 100;
    config.max_packet_size = 1500;
    config.signal_export_interval_ms = 2;
    return config;
}

inline VDQueueConfig high_throughput_5g()
{
    VDQueueConfig config;
    config.max_queue_size = 256;
    config.max_packet_size = 9000;
    config.signal_export_interval_ms = 1;
    return config;
}

} // namespace ModemProfiles

} // namespace cellular_emulation

#endif /* VDQUEUE_CONFIG_HH */
