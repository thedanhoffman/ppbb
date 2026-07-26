// control-channel.cpp — Control channel selection and application
//
// Greedy algorithm for picking the fastest control channel that can
// cover the required power delta, with quantization to hardware granularity.

#include "control-channel.h"

// ═══════════════════════════════════════════════════════════
// select_channel — greedy control selection
// ═══════════════════════════════════════════════════════════

int select_channel(const std::vector<ControlChannel>& channels,
                   double current_w, double target_w) {
    double delta = target_w - current_w;
    if (std::fabs(delta) < 0.01) return -1;  // negligible change

    // Filter channels that can cover this delta
    std::vector<int> candidates;
    for (int i = 0; i < (int)channels.size(); ++i) {
        if (channels[i].max_change_w >= std::fabs(delta)) {
            candidates.push_back(i);
        }
    }

    if (candidates.empty()) return -1;  // no channel can handle it

    // Pick fastest (lowest latency_ms)
    int best = candidates[0];
    for (int i = 1; i < (int)candidates.size(); ++i) {
        if (channels[candidates[i]].latency_ms < channels[best].latency_ms) {
            best = candidates[i];
        }
    }

    return best;
}

// ═══════════════════════════════════════════════════════════
// apply_budget — quantize and apply
// ═══════════════════════════════════════════════════════════

bool apply_channel_budget(const ControlChannel& ch, double target_w) {
    // Quantize to granularity
    double gran = ch.granularity_w;
    if (gran <= 0) gran = 0.125;  // default 125 mW

    double quantized = std::round(target_w / gran) * gran;

    // Clamp to channel range
    quantized = std::max(quantized, ch.min_change_w);
    quantized = std::min(quantized, ch.max_change_w);

    return ch.apply(quantized);
}

// ═══════════════════════════════════════════════════════════
// Generic channel presets
// ═══════════════════════════════════════════════════════════

void populate_generic_rapl_channel(std::vector<ControlChannel>& channels,
                                   double max_w) {
    channels.emplace_back(
        "rapl_budget",
        0.125,     // min_change_w (RAPL granularity = 125 mW)
        max_w,     // max_change_w (full range)
        10.0,      // latency_ms (RAPL writes are fast)
        0.125,     // granularity_w
        // apply: set RAPL power limit to target watts
        [max_w](double /* watts */) {
            // Placeholder — actual implementation uses RAPL sysfs
            return true;
        },
        // current_state: "[RAPL target]"
        []() { return "[RAPL]"; }
    );
}

void populate_generic_epp_channel(std::vector<ControlChannel>& channels,
                                  double min_w, double max_w) {
    channels.emplace_back(
        "epp",
        min_w,
        max_w,
        50.0,      // latency_ms (EPP change is slower)
        0.5,       // granularity_w (EPP steps are coarse)
        [](double /* watts */) {
            // Placeholder — actual implementation writes energy_performance_preference
            return true;
        },
        []() { return "[EPP]"; }
    );
}

void populate_generic_turbo_channel(std::vector<ControlChannel>& channels,
                                    double max_w) {
    channels.emplace_back(
        "turbo",
        0.0,       // min_change_w (can disable turbo = 0 W turbo power)
        max_w * 0.1, // max_change_w (turbo adds ~10% power)
        5.0,       // latency_ms (turbo disable is instant)
        1.0,       // granularity_w (binary: on/off)
        [](double /* watts */) {
            // Placeholder — actual implementation writes intel_pstate/no_turbo
            return true;
        },
        []() { return "[TURBO]"; }
    );
}

void populate_generic_hotplug_channel(std::vector<ControlChannel>& channels,
                                      double min_w, double max_w) {
    channels.emplace_back(
        "hotplug",
        min_w,
        max_w,
        5000.0,    // latency_ms (hotplug is slow — CPU bringup takes seconds)
        5.0,       // granularity_w (offline entire cores — coarse)
        [](double /* watts */) {
            // Placeholder — actual implementation writes cpu/online
            return true;
        },
        []() { return "[HOTPLUG]"; }
    );
}
