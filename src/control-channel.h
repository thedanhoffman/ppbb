// control-channel.h — Platform-specific actuator abstraction
//
// Provides a unified interface for power control channels. The solver
// outputs a target power budget; this layer picks the right combination
// of control channels to achieve that budget with minimal latency and
// appropriate granularity.
//
// Each platform (Intel, AMD, ARM, etc.) implements a concrete
// ControlChannelRegistry that populates the available channels.

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <cmath>

// ═══════════════════════════════════════════════════════════
// ControlChannel — a single power control mechanism
// ═══════════════════════════════════════════════════════════

struct ControlChannel {
    std::string name;            // "rapl_pp0", "epp_pcores", "turbo", "hotplug"
    double    min_change_w;      // minimum power change this channel can make
    double    max_change_w;      // maximum power range this channel covers
    double    latency_ms;        // response time in ms
    double    granularity_w;     // smallest unit of change (e.g., 125 mW)

    // Apply a target power budget. Returns true on success.
    // The actuator decides the best way to translate watts → control.
    std::function<bool(double watts)> apply;

    // Human-readable string of current state (for logging)
    std::function<std::string()> current_state;

    ControlChannel() = default;

    ControlChannel(std::string n, double min_c, double max_c,
                   double lat, double gran,
                   std::function<bool(double)> setter,
                   std::function<std::string()> state)
        : name(std::move(n)), min_change_w(min_c), max_change_w(max_c),
          latency_ms(lat), granularity_w(gran),
          apply(std::move(setter)), current_state(std::move(state)) {}
};

// ═══════════════════════════════════════════════════════════
// select_channel — greedy control selection
// ═══════════════════════════════════════════════════════════

// Given a set of channels and a target power change, pick the fastest
// channel (or combination) that can cover the delta.
// Returns the channel index to use, or -1 if none can help.
//
// Strategy:
//   1. If target == current, return -1 (no action needed)
//   2. Pick the fastest channel whose max_change_w >= |delta|
//   3. If no single channel suffices, pick the combination with
//      the least total latency (greedy)
//   4. Apply quantization to granularity_w before calling apply()
int select_channel(const std::vector<ControlChannel>& channels,
                   double current_w, double target_w);

// ═══════════════════════════════════════════════════════════
// apply_budget — apply budget with quantization
// ═══════════════════════════════════════════════════════════

// Apply a target power budget to a channel, quantized to its granularity.
// Returns true on success.
bool apply_channel_budget(const ControlChannel& ch, double target_w);

// ═══════════════════════════════════════════════════════════
// ControlChannelRegistry — platform-specific channel population
// ═══════════════════════════════════════════════════════════

// Each platform provides a function that populates a channel list.
// The solver/actuator code calls this at startup.
using PopulateChannels = std::function<void(std::vector<ControlChannel>&)>;

// Intel-specific channel population (for now)
void populate_intel_channels(std::vector<ControlChannel>& channels);

// Generic channel presets (for reference/testing)
void populate_generic_rapl_channel(std::vector<ControlChannel>& channels,
                                   double max_w);
void populate_generic_epp_channel(std::vector<ControlChannel>& channels,
                                  double min_w, double max_w);
void populate_generic_turbo_channel(std::vector<ControlChannel>& channels,
                                    double max_w);
void populate_generic_hotplug_channel(std::vector<ControlChannel>& channels,
                                      double min_w, double max_w);
