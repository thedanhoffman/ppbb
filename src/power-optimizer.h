// power-optimizer.h — Linear constraint optimization for power balancing
//
// Refactors power-balance.cpp's threshold-driven control logic into a
// principled optimization problem:
//
//   minimize: w1 * thermal_pressure + w2 * throttle_penalty + w3 * performance_loss
//   subject to:
//     core_power_w <= pl1_w - gpu_w - headroom
//     core_power_w >= 0
//     max_perf_pct in [min_perf_pct, 100]
//     no_turbo in {0, 1}
//     EPP_p, EPP_e in {performance, balance_performance, balance_power, power}
//     n_online_groups >= 2 (safety floor)
//
// The solver is a fast greedy feasible-point finder — no LP library needed.
// Each variable has a closed-form solution derived from constraints + objectives.

#pragma once

#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

// ── Configuration constants ──

// Smoothing factor for max_perf_pct (exponential moving average)
// Prevents rapid fluctuations between cycles — 0.0 = no smoothing, 1.0 = frozen
static constexpr double MAX_PERF_SMOOTH_ALPHA = 0.5;

// Hysteresis margin for EPP transitions (W of GPU power)
// Prevents EPP from flapping between levels when GPU power is near a threshold
static constexpr double EPP_HYSTERESIS_W = 1.0;

// Core power budget constants
static constexpr double HEADROOM_MARGIN_W    = 2.0;   // base PL1 headroom for GPU
static constexpr double THERMAL_HEADROOM_R   = 5.0;   // extra headroom per unit pressure (W)
static constexpr double CRITICAL_CPU_MAX_W   = 8.0;   // max core budget when GPU throttling
static constexpr double CRITICAL_MAX_PERF    = 20.0;  // min max_perf_pct when GPU throttling
static constexpr double ACTIVE_MAX_PERF      = 50.0;  // min max_perf_pct when GPU active
static constexpr double GPU_HEAVY_W          = 15.0;  // GPU power for proportional scaling
static constexpr int    MIN_CORE_GROUPS            = 2;  // minimum core groups to keep online

// EPP levels (ordered from most performant to most power-efficient)
enum class EppLevel {
    Performance,          // "performance"
    BalancePerformance,   // "balance_performance"
    BalancePower,         // "balance_power"
    Power                 // "power"
};

// Convert EppLevel to string — defined in .cpp to avoid unused-variable/function warnings
const char* epp_to_string(EppLevel e);

// ── Optimization inputs (observed each cycle) ──

struct OptimizerInputs {
    // Power budget
    double pl1_w              = 40.0;   // desired package PL1 limit (W)
    double gpu_w              = 0.0;    // smoothed GPU power (W)
    bool   have_gpu           = false;  // GPU detected

    // Thermal
    double temp_c             = -1.0;   // max core temperature (°C), -1 = unknown

    // GPU state
    double gpu_c0_pct         = 0.0;    // GPU C0 residency fraction [0, 1]
    bool   gpu_throttling     = false;  // GPU throttle event detected

    // Environmental
    // System state
    int    total_core_groups  = 8;      // total physical core groups on system
    bool   have_coretemp      = false;  // thermal sensors available

    // Previous cycle state (for smoothing)
    int    prev_max_perf      = 100;    // previous max_perf_pct
    EppLevel prev_epp_p       = EppLevel::BalancePerformance;
    EppLevel prev_epp_e       = EppLevel::BalancePerformance;
};

// ── Optimization outputs (decisions made each cycle) ──

struct OptimizerResult {
    // Power limits
    double core_power_w       = 0.0;    // RAPL core domain power limit (W)
    double core_limit_r       = 0.0;    // rounded core limit (125mW granularity)

    // CPU frequency
    int    max_perf_pct       = 100;    // intel_pstate max_perf_pct
    int    no_turbo           = 0;      // intel_pstate no_turbo (0 or 1)
    int    min_perf_pct       = 8;      // intel_pstate min_perf_pct

    // EPP settings
    EppLevel epp_p            = EppLevel::BalancePerformance;
    EppLevel epp_e            = EppLevel::BalancePerformance;

    // Unsmoothed values (for logging/debug)
    int    raw_max_perf_pct   = 100;
    int    raw_no_turbo       = 0;

    // Core groups to keep online (0 = all online)
    int    keep_groups        = 0;

    // Debug info
    double gpu_headroom_w     = 0.0;    // reserved headroom for GPU (W)
    double thermal_extra_w    = 0.0;    // extra headroom from thermal constraints (W)
    double weight_thermal     = 0.0;    // current thermal weight
    double weight_throttle    = 0.0;    // current throttle weight
    double weight_performance = 0.0;    // current performance weight
    double temp_c             = -1.0;   // max core temp (for logging)

    // Apply smoothing to max_perf_pct using previous cycle value
    int    smooth_max_perf(int prev) const;

    // Helper: compute smoothed max_perf_pct from raw and previous value (inline)
    static int smooth_max_perf_static(int raw, int prev) {
        return (int)(MAX_PERF_SMOOTH_ALPHA * (double)raw
                   + (1.0 - MAX_PERF_SMOOTH_ALPHA) * (double)prev);
    }
};

// ── Core solver function ──

// Solve the optimization problem given current system state.
// Returns the optimal control decisions.
// This is the main entry point — call once per control cycle.
OptimizerResult solve(const OptimizerInputs& inputs);

// ── Utility functions (for testing and integration) ──

// Compute thermal pressure: 0.0 (cold) → 1.0 (at or above 90°C)
// Linear interpolation between 70°C and 90°C.
double compute_thermal_pressure(double temp_c);

// Compute temperature-based max_perf_pct cap:
// 70°C → 100%, 90°C → 20%, linear between. Clamped to [20, 100].
int compute_thermal_max_perf(double temp_c);

// Compute temperature-based no_turbo setting.
int compute_thermal_no_turbo(double temp_c);

// Compute temperature-based EPP tier.
int compute_thermal_epp_tier(double temp_c);

// Compute extra headroom from thermal pressure (W).
double compute_thermal_headroom(double temp_c);

// Compute weight schedule from observed conditions.
// Returns {w_thermal, w_throttle, w_performance}.
void compute_weights(const OptimizerInputs& inputs,
                     double& w_thermal, double& w_throttle, double& w_perf);

// Determine aggression level from weights (for logging).
// Returns 0=idle, 1=active, 2=throttling
int aggression_from_weights(double w_thermal, double w_throttle, double w_perf);
