/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef BACKPRESSURE_CONTROLLER_HH
#define BACKPRESSURE_CONTROLLER_HH

#include <cstdint>
#include <string>
#include <functional>
#include <vector>
#include <deque>
#include <memory>
#include <sstream>
#include <cmath>

namespace cellular_emulation {

enum class BackpressurePolicy {
    THRESHOLD,
    HYSTERESIS,
    PREDICTIVE,
    ADAPTIVE
};

struct BackpressureConfig {
    BackpressurePolicy policy = BackpressurePolicy::HYSTERESIS;
    double high_threshold = 1.0;
    double low_threshold = 0.8;
    uint64_t prediction_horizon_ms = 100;
    size_t rate_estimation_window = 10;
    uint64_t min_state_duration_ms = 5;
    bool enable_early_activation = false;
    
    bool validate() const
    {
        if (high_threshold < 0.0 || high_threshold > 1.0) return false;
        if (low_threshold < 0.0 || low_threshold > high_threshold) return false;
        return true;
    }
    
    std::string to_string() const
    {
        std::ostringstream oss;
        oss << "BackpressureConfig{"
            << "policy=" << static_cast<int>(policy)
            << ", high=" << high_threshold
            << ", low=" << low_threshold
            << "}";
        return oss.str();
    }
};

struct BackpressureState {
    bool active = false;
    uint64_t state_start_time = 0;
    double utilization = 0.0;
    double growth_rate = 0.0;
    int64_t predicted_time_to_full_ms = -1;
    uint64_t transition_count = 0;
};

class BackpressureController {
public:
    explicit BackpressureController(const BackpressureConfig& config)
        : config_(config),
          state_(),
          utilization_history_(),
          state_change_callback_(nullptr)
    {
    }
    
    BackpressureController() : BackpressureController(BackpressureConfig()) {}
    
    bool update(size_t current_size, size_t max_size, uint64_t current_time)
    {
        double utilization = (max_size > 0) ? 
            static_cast<double>(current_size) / static_cast<double>(max_size) : 0.0;
        
        state_.utilization = utilization;
        update_utilization_history(utilization, current_time);
        state_.growth_rate = estimate_growth_rate();
        
        if (state_.growth_rate > 0) {
            double remaining = config_.high_threshold - utilization;
            if (remaining > 0) {
                state_.predicted_time_to_full_ms = 
                    static_cast<int64_t>(remaining / state_.growth_rate * 1000.0);
            } else {
                state_.predicted_time_to_full_ms = 0;
            }
        } else {
            state_.predicted_time_to_full_ms = -1;
        }
        
        bool new_active = evaluate_policy(utilization, current_time);
        
        if (new_active != state_.active) {
            uint64_t state_duration = current_time - state_.state_start_time;
            if (state_duration >= config_.min_state_duration_ms) {
                transition_state(new_active, current_time);
            }
        }
        
        return state_.active;
    }
    
    bool is_active() const { return state_.active; }
    const BackpressureState& state() const { return state_; }
    
    void set_state_change_callback(std::function<void(bool)> callback)
    {
        state_change_callback_ = callback;
    }
    
    void reset()
    {
        state_ = BackpressureState();
        utilization_history_.clear();
    }
    
    void update_config(const BackpressureConfig& config)
    {
        config_ = config;
    }
    
    std::string statistics_summary() const
    {
        std::ostringstream oss;
        oss << "BackpressureController Stats:\n";
        oss << "  Active: " << (state_.active ? "YES" : "no") << "\n";
        oss << "  Utilization: " << (state_.utilization * 100.0) << "%\n";
        oss << "  Growth rate: " << state_.growth_rate << "/s\n";
        oss << "  Transitions: " << state_.transition_count << "\n";
        if (state_.predicted_time_to_full_ms >= 0) {
            oss << "  Time to full: " << state_.predicted_time_to_full_ms << " ms\n";
        }
        return oss.str();
    }

private:
    BackpressureConfig config_;
    BackpressureState state_;
    
    struct UtilizationSample {
        uint64_t timestamp;
        double utilization;
    };
    std::deque<UtilizationSample> utilization_history_;
    
    std::function<void(bool)> state_change_callback_;
    
    void update_utilization_history(double utilization, uint64_t timestamp)
    {
        utilization_history_.push_back({timestamp, utilization});
        
        while (utilization_history_.size() > config_.rate_estimation_window) {
            utilization_history_.pop_front();
        }
    }
    
    double estimate_growth_rate() const
    {
        if (utilization_history_.size() < 2) {
            return 0.0;
        }
        
        const auto& first = utilization_history_.front();
        const auto& last = utilization_history_.back();
        
        double dt = static_cast<double>(last.timestamp - first.timestamp) / 1000.0;
        if (dt <= 0) {
            return 0.0;
        }
        
        double du = last.utilization - first.utilization;
        return du / dt;
    }
    
    bool evaluate_policy(double utilization, uint64_t current_time)
    {
        switch (config_.policy) {
            case BackpressurePolicy::THRESHOLD:
                return utilization >= config_.high_threshold;
                
            case BackpressurePolicy::HYSTERESIS:
                if (state_.active) {
                    return utilization >= config_.low_threshold;
                } else {
                    return utilization >= config_.high_threshold;
                }
                
            case BackpressurePolicy::PREDICTIVE:
                return evaluate_predictive_policy(utilization);
                
            case BackpressurePolicy::ADAPTIVE:
                return evaluate_adaptive_policy(utilization, current_time);
                
            default:
                return utilization >= config_.high_threshold;
        }
    }
    
    bool evaluate_predictive_policy(double utilization)
    {
        if (utilization >= config_.high_threshold) {
            return true;
        }
        
        if (config_.enable_early_activation && state_.growth_rate > 0) {
            if (state_.predicted_time_to_full_ms >= 0 &&
                static_cast<uint64_t>(state_.predicted_time_to_full_ms) < config_.prediction_horizon_ms) {
                return true;
            }
        }
        
        if (state_.active && utilization >= config_.low_threshold) {
            return true;
        }
        
        return false;
    }
    
    bool evaluate_adaptive_policy(double utilization, uint64_t /* current_time */)
    {
        double effective_high = config_.high_threshold;
        double effective_low = config_.low_threshold;
        
        if (state_.growth_rate > 1.0) {
            effective_high *= 0.9;
            effective_low *= 0.9;
        }
        
        if (state_.growth_rate < -0.5) {
            effective_high = std::min(1.0, effective_high * 1.1);
            effective_low = std::min(effective_high, effective_low * 1.1);
        }
        
        if (state_.active) {
            return utilization >= effective_low;
        } else {
            return utilization >= effective_high;
        }
    }
    
    void transition_state(bool new_active, uint64_t current_time)
    {
        state_.active = new_active;
        state_.state_start_time = current_time;
        state_.transition_count++;
        
        if (state_change_callback_) {
            state_change_callback_(new_active);
        }
    }
};

} // namespace cellular_emulation

#endif /* BACKPRESSURE_CONTROLLER_HH */
