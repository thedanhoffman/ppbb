// intel-actuator.cpp — Intel platform actuator implementation
//
// Bridges ResourceResult (solver output) to sysfs/MSR writes.
// Implements greedy channel selection based on latency, granularity, range.

#include "intel-actuator.h"
#include "power-utils.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <cmath>

// ═══════════════════════════════════════════════════════════
// IntelActuator::get_channels
// ═══════════════════════════════════════════════════════════

std::vector<ControlChannel> IntelActuator::get_channels() {
    std::vector<ControlChannel> channels;

    // Probe for RAPL core domain
    auto rapl_domains = read_all_rapl_domains();
    bool have_core = false;
    double core_max_w = 0.0;
    double core_current_w = 0.0;
    for (const auto& d : rapl_domains) {
        if (d.name == "core" || d.name == "pp0") {
            have_core = true;
            core_max_w = d.max_w();
            core_current_w = d.power_uw / 1e6;
            have_rapl_core_ = true;
            break;
        }
    }

    if (have_core) {
        add_rapl_channel(channels, core_max_w, core_current_w);
    }

    add_epp_p_channel(channels, 2.0, 40.0);
    add_turbo_channel(channels, 40.0);
    add_maxperf_channel(channels, current_max_perf_pct_);
    add_hotplug_channel(channels, 5.0, 40.0);

    return channels;
}

// ═══════════════════════════════════════════════════════════
// IntelActuator::apply
// ═══════════════════════════════════════════════════════════

bool IntelActuator::apply(const ResourceResult& result, double current_cpu_w) {
    current_cpu_power_w_ = current_cpu_w;

    auto channels = get_channels();
    if (channels.empty()) return false;

    double target_w = result.effective_cpu_w();

    int idx = select_channel(channels, current_cpu_w, target_w);
    if (idx < 0) return false;  // no change needed

    return apply_channel_budget(channels[idx], target_w);
}

double IntelActuator::get_current_cpu_w() const {
    return current_cpu_power_w_;
}

// ═══════════════════════════════════════════════════════════
// Channel helpers
// ═══════════════════════════════════════════════════════════

void IntelActuator::add_rapl_channel(std::vector<ControlChannel>& channels,
                                     double max_w, double current_w) {
    channels.emplace_back(
        "rapl_pp0",
        0.125, max_w, 10.0, 0.125,
        [max_w](double watts) -> bool {
            // Stub: would call rapl_set_power_limit() from power-utils.cpp
            return watts >= 0 && watts <= max_w;
        },
        [current_w]() -> std::string {
            return "RAPL=" + std::to_string(static_cast<int>(current_w)) + "W";
        }
    );
}

void IntelActuator::add_epp_p_channel(std::vector<ControlChannel>& channels,
                                      double min_w_param, double max_w_param) {
    channels.emplace_back(
        "epp_pcores",
        min_w_param, max_w_param, 50.0, 2.0,
        [](double /* watts */) -> bool {
            // Stub: would write energy_performance_preference
            return true;
        },
        []() -> std::string { return "EPP=busy"; }
    );
}

void IntelActuator::add_turbo_channel(std::vector<ControlChannel>& channels,
                                      double max_w) {
    channels.emplace_back(
        "turbo_disable",
        0.0, max_w * 0.15, 5.0, 1.0,
        [](double /* watts */) -> bool {
            // Stub: would write intel_pstate/no_turbo
            return true;
        },
        []() -> std::string { return "TURBO=on"; }
    );
}

void IntelActuator::add_maxperf_channel(std::vector<ControlChannel>& channels,
                                        int current_pct) {
    channels.emplace_back(
        "max_perf_pct",
        2.0, 40.0, 20.0, 0.4,
        [](double /* watts */) -> bool {
            // Stub: would write intel_pstate/max_perf_pct
            return true;
        },
        [current_pct]() -> std::string {
            return "MAX_PERF=" + std::to_string(current_pct) + "%";
        }
    );
}

void IntelActuator::add_hotplug_channel(std::vector<ControlChannel>& channels,
                                        double min_w_param, double max_w_param) {
    (void)min_w_param; (void)max_w_param;
    channels.emplace_back(
        "hotplug",
        min_w_param, max_w_param,
        5000.0, 5.0,
        [](double /* watts */) -> bool {
            // Stub: would write cpu/online
            return true;
        },
        [this]() -> std::string {
            return "HOTPLUG=" + std::to_string(total_core_groups_) + "groups";
        }
    );
}
