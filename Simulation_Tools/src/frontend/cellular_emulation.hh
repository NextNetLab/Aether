/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef CELLULAR_EMULATION_HH
#define CELLULAR_EMULATION_HH

#include "virtual_driver_queue.hh"
#include "virtual_modem_buffer.hh"
#include "two_tier_queue_manager.hh"
#include "vdqueue_config.hh"
#include "vmodem_config.hh"
#include "vdqueue_stats.hh"
#include "signal_exporter.hh"
#include "backpressure_controller.hh"

namespace cellular_emulation {

struct Version {
    static constexpr int MAJOR = 1;
    static constexpr int MINOR = 0;
    static constexpr int PATCH = 0;
    static constexpr const char* STRING = "1.0.0";
    static constexpr const char* NAME = "Aether Mahimahi Extension";
};

inline TwoTierQueueManager create_default_manager()
{
    TwoTierQueueConfig config;
    config.driver_queue_config = ModemProfiles::qualcomm_sdx55();
    return TwoTierQueueManager(config);
}

inline TwoTierQueueManager create_manager_with_profile(const std::string& profile_name)
{
    TwoTierQueueConfig config;
    
    if (profile_name == "sdx55" || profile_name == "qualcomm") {
        config.driver_queue_config = ModemProfiles::qualcomm_sdx55();
    } else if (profile_name == "xmm7560" || profile_name == "intel") {
        config.driver_queue_config = ModemProfiles::intel_xmm7560();
    } else if (profile_name == "exynos" || profile_name == "samsung") {
        config.driver_queue_config = ModemProfiles::samsung_exynos();
    } else if (profile_name == "generic" || profile_name == "generic_lte") {
        config.driver_queue_config = ModemProfiles::generic_lte();
    } else if (profile_name == "5g" || profile_name == "5g_high") {
        config.driver_queue_config = ModemProfiles::high_throughput_5g();
    } else {
        config.driver_queue_config = ModemProfiles::qualcomm_sdx55();
    }
    
    return TwoTierQueueManager(config);
}

inline void print_info()
{
    std::cout << "=================================\n";
    std::cout << " " << Version::NAME << "\n";
    std::cout << " Version: " << Version::STRING << "\n";
    std::cout << "=================================\n";
    std::cout << "\n";
    std::cout << "Two-Tier Queue Architecture:\n";
    std::cout << "  - VDQueue (Driver Layer)\n";
    std::cout << "  - vModemBuf (Modem Layer)\n";
    std::cout << "\n";
    std::cout << "Available Modem Profiles:\n";
    std::cout << "  - sdx55 (Qualcomm SDX55)\n";
    std::cout << "  - xmm7560 (Intel XMM 7560)\n";
    std::cout << "  - exynos (Samsung Exynos)\n";
    std::cout << "  - generic_lte\n";
    std::cout << "  - 5g_high\n";
}

} // namespace cellular_emulation

#endif /* CELLULAR_EMULATION_HH */
