/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef VMODEM_CONFIG_HH
#define VMODEM_CONFIG_HH

#include <cstddef>
#include <cstdint>
#include <string>
#include <sstream>

namespace cellular_emulation {

struct VModemConfig {
    size_t max_buffer_size;
    size_t max_buffer_bytes;
    double hysteresis_ratio;             /* release backpressure at this fill level */
    bool enable_scheduling;
    size_t min_batch_size;

    VModemConfig()
        : max_buffer_size(100),
          max_buffer_bytes(0),
          hysteresis_ratio(0.8),
          enable_scheduling(true),
          min_batch_size(1)
    {
    }

    std::string to_string() const
    {
        std::ostringstream oss;
        oss << "VModemConfig{"
            << "max_buffer_size=" << max_buffer_size
            << ", max_buffer_bytes=" << max_buffer_bytes
            << ", hysteresis=" << hysteresis_ratio
            << "}";
        return oss.str();
    }

    bool validate() const
    {
        if (max_buffer_size == 0) return false;
        if (hysteresis_ratio < 0.0 || hysteresis_ratio > 1.0) return false;
        return true;
    }
};

} // namespace cellular_emulation

#endif /* VMODEM_CONFIG_HH */
