// intel-actuator.h — Intel platform-specific actuator
//
// Bridges ResourceResult (solver output) to concrete Intel sysfs/MSR writes.
// Populates ControlChannel list for the greedy channel picker.
//
// Control channels implemented:
//   1. RAPL PP0 budget     — fast (10ms), coarse (125mW), full range
//   2. EPP (P-cores)       — medium (50ms), medium granularity
//   3. Turbo disable        — fast (5ms), binary, ~10% power impact
//   4. max_perf_pct         — medium (20ms), 1% granularity
//   5. Hotplug              — slow (5s), coarse (whole core groups)

#pragma once

#include "control-channel.h"
#include "resource-domain.h"

#include <vector>
#include <string>

// ═══════════════════════════════════════════════════════════
// IntelActuator — platform-specific control application
// ═══════════════════════════════════════════════════════════

class IntelActuator {
public:
    IntelActuator() = default;

    // Populate control channels from available hardware
    // Returns the list of available channels
    std::vector<ControlChannel> get_channels();

    // Apply ResourceResult to the system via available channels
    // Returns true if any changes were made
    bool apply(const ResourceResult& result, double current_cpu_w);

    // Get current CPU power reading (for channel selection)
    double get_current_cpu_w() const;

private:
    // Helper: populate RAPL PP0 channel
    void add_rapl_channel(std::vector<ControlChannel>& channels,
                          double max_w, double current_w);

    // Helper: populate EPP P-core channel
    void add_epp_p_channel(std::vector<ControlChannel>& channels,
                           double min_w, double max_w);

    // Helper: populate turbo channel
    void add_turbo_channel(std::vector<ControlChannel>& channels,
                           double max_w);

    // Helper: populate max_perf_pct channel
    void add_maxperf_channel(std::vector<ControlChannel>& channels,
                             int current_pct);

    // Helper: populate hotplug channel
    void add_hotplug_channel(std::vector<ControlChannel>& channels,
                             double min_w, double max_w);

    // Current system state (updated periodically by power-balance.cpp)
    double current_cpu_power_w_ = 0.0;
    int current_max_perf_pct_ = 100;
    int current_no_turbo_ = 0;
    bool have_rapl_core_ = false;
    int total_core_groups_ = 8;
};
