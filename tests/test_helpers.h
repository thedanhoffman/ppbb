// test_helpers.h — Minimal pure-function helpers for power-balance tests
//
// Contains ONLY functions actually used by production code in power-balance.cpp.
// All other functions from the old power_balance_pure.h were dead code
// (never called by the daemon — the optimizer uses solve() as its single entry).

#pragma once

#include <chrono>
#include <cmath>
#include <algorithm>

// ── Sample struct — used by compute_power_w ──

struct Sample {
    long long energy_uj = -1;
    std::chrono::steady_clock::time_point time;
};

// ── Pure Functions (used in production) ──

// Compute power (Watts) from two energy samples with time delta.
// Returns -1 on invalid input (negative delta or zero time).
inline double compute_power_w(const Sample& prev, const Sample& cur) {
    if (prev.energy_uj < 0 || cur.energy_uj < 0) return 0;
    long long delta_uj = cur.energy_uj - prev.energy_uj;
    if (delta_uj < 0) return -1;
    auto delta_us = std::chrono::duration_cast<std::chrono::microseconds>(
        cur.time - prev.time).count();
    if (delta_us <= 0) return -1;
    return (double)delta_uj / delta_us;
}

// Round watts to nearest 125mW boundary.
// Returns 0 for negative input.
inline double round_to_125mw(double watts) {
    if (watts < 0) return 0;
    return std::round(watts / 0.125) * 0.125;
}
