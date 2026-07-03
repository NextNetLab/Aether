#include <iostream>
#include <cassert>

#include "cellular_emulation.hh"

using namespace cellular_emulation;

void test_vdqueue_config()
{
    std::cout << "Testing VDQueueConfig..." << std::endl;
    
    VDQueueConfig config;
    assert(config.max_queue_size == 128);
    assert(config.max_packet_size == 1504);
    assert(config.validate());
    
    std::cout << "  Config: " << config.to_string() << std::endl;
    std::cout << "  PASSED" << std::endl;
}

void test_vdqueue_stats()
{
    std::cout << "Testing VDQueueStats..." << std::endl;
    
    VDQueueStats stats;
    stats.total_enqueued_packets = 100;
    stats.total_dequeued_packets = 90;
    stats.total_dropped_packets = 10;
    
    double drop_rate = stats.drop_rate();
    assert(drop_rate > 0.09 && drop_rate < 0.11);  /* Approximately 0.1 */
    
    std::cout << "  " << stats.to_string() << std::endl;
    std::cout << "  PASSED" << std::endl;
}

void test_virtual_driver_queue()
{
    std::cout << "Testing VirtualDriverQueue..." << std::endl;
    
    VDQueueConfig config;
    config.max_queue_size = 10;
    
    VirtualDriverQueue queue(config);
    
    /* Test enqueue */
    for (int i = 0; i < 10; ++i) {
        bool result = queue.enqueue("test_packet", static_cast<uint64_t>(i * 100));
        assert(result == true);
    }
    
    assert(queue.size() == 10);
    assert(queue.full());
    
    /* Test overflow - should be dropped */
    bool result = queue.enqueue("overflow_packet", 1000);
    assert(result == false);
    
    /* Test dequeue */
    auto packet = queue.dequeue();
    assert(!packet.contents.empty());
    assert(queue.size() == 9);
    
    std::cout << "  " << queue.to_string() << std::endl;
    std::cout << "  PASSED" << std::endl;
}

void test_virtual_modem_buffer()
{
    std::cout << "Testing VirtualModemBuffer..." << std::endl;
    
    VModemConfig config;
    config.max_buffer_size = 5;
    
    VirtualModemBuffer buffer(config);
    
    /* Enqueue packets */
    for (int i = 0; i < 5; ++i) {
        QueuedPacket pkt("modem_packet", static_cast<uint64_t>(i * 100));
        bool result = buffer.enqueue(std::move(pkt));
        assert(result == true);
    }
    
    assert(buffer.size_packets() == 5);
    assert(buffer.backpressure_active());
    
    /* Try one more - should fail */
    QueuedPacket overflow("overflow", 500);
    bool result = buffer.enqueue(std::move(overflow));
    assert(result == false);
    
    std::cout << "  " << buffer.to_string() << std::endl;
    std::cout << "  PASSED" << std::endl;
}

void test_two_tier_queue_manager()
{
    std::cout << "Testing TwoTierQueueManager..." << std::endl;
    
    TwoTierQueueConfig config;
    config.driver_queue_config.max_queue_size = 20;
    config.modem_buffer_config.max_buffer_size = 10;
    config.enable_signal_export = false;  /* Disable file export for test */
    
    TwoTierQueueManager manager(config);
    
    /* Send packets */
    for (int i = 0; i < 30; ++i) {
        manager.receive_packet("test_data_packet", static_cast<uint64_t>(i * 10));
    }
    
    std::cout << "  Driver queue: " << manager.driver_queue_size() << std::endl;
    std::cout << "  Modem buffer: " << manager.modem_buffer_size() << std::endl;
    std::cout << "  Backpressure: " << (manager.is_backpressure_active() ? "YES" : "no") << std::endl;
    
    /* Drain some packets */
    for (int i = 0; i < 5; ++i) {
        auto pkt = manager.drain_packet(static_cast<uint64_t>(1000 + i));
        assert(!pkt.contents.empty());
    }
    
    std::cout << "  After drain - Driver: " << manager.driver_queue_size() 
              << ", Modem: " << manager.modem_buffer_size() << std::endl;
    
    std::cout << "  " << manager.to_string() << std::endl;
    std::cout << "  PASSED" << std::endl;
}

void test_backpressure_controller()
{
    std::cout << "Testing BackpressureController..." << std::endl;
    
    BackpressureConfig config;
    config.policy = BackpressurePolicy::HYSTERESIS;
    config.high_threshold = 0.8;
    config.low_threshold = 0.5;
    
    BackpressureController controller(config);
    
    /* Simulate queue filling */
    assert(!controller.update(50, 100, 0));   /* 50% - not active */
    assert(!controller.update(70, 100, 10));  /* 70% - not active */
    assert(controller.update(85, 100, 20));   /* 85% - active! */
    assert(controller.update(60, 100, 30));   /* 60% - still active (hysteresis) */
    assert(!controller.update(40, 100, 40));  /* 40% - deactivated */
    
    std::cout << "  " << controller.statistics_summary() << std::endl;
    std::cout << "  PASSED" << std::endl;
}

void test_signal_exporter()
{
    std::cout << "Testing SignalExporter..." << std::endl;
    
    QueueSignal signal;
    signal.sample_seq = 1;
    signal.sample_queue_length = 50;
    signal.sample_dwell_time_ms = 25;
    signal.live_queue_length = 49;
    signal.modem_queue_length = 10;
    signal.total_queue_length = 59;
    signal.drain_complete = false;
    signal.timestamp_ms = 1000;
    signal.backpressure_active = true;
    
    std::cout << "  Simple: " << signal.to_simple_string() << std::endl;
    std::cout << "  Full: " << signal.to_string() << std::endl;
    std::cout << "  CSV: " << signal.to_csv_line() << std::endl;
    
    std::cout << "  PASSED" << std::endl;
}

void test_modem_profiles()
{
    std::cout << "Testing Modem Profiles..." << std::endl;
    
    auto sdx55 = ModemProfiles::qualcomm_sdx55();
    assert(sdx55.max_queue_size == 128);
    
    auto intel = ModemProfiles::intel_xmm7560();
    assert(intel.max_queue_size == 64);
    
    auto samsung = ModemProfiles::samsung_exynos();
    assert(samsung.max_queue_size == 96);
    
    std::cout << "  SDX55: " << sdx55.to_string() << std::endl;
    std::cout << "  Intel: " << intel.to_string() << std::endl;
    std::cout << "  Samsung: " << samsung.to_string() << std::endl;
    std::cout << "  PASSED" << std::endl;
}

int main()
{
    std::cout << "=== Cellular Emulation Module Tests ===" << std::endl;
    std::cout << "Version: " << Version::STRING << std::endl;
    std::cout << std::endl;
    
    try {
        test_vdqueue_config();
        test_vdqueue_stats();
        test_virtual_driver_queue();
        test_virtual_modem_buffer();
        test_two_tier_queue_manager();
        test_backpressure_controller();
        test_signal_exporter();
        test_modem_profiles();
        
        std::cout << std::endl;
        std::cout << "=== ALL TESTS PASSED ===" << std::endl;
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
