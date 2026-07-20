// power-optimizer.cpp — Linear constraint optimization for power balancing
//
// Implements the greedy feasible-point solver for the power balancing
// optimization problem. No LP library needed — each variable has a
// closed-form solution derived from constraints + objectives.

#include "power-optimizer.h"

// ── EPP string conversion ──

int OptimizerResult::smooth_max_perf(int prev) const {
    // Exponential moving average: smooth = alpha * raw + (1-alpha) * prev
    // This prevents rapid max_perf_pct fluctuations between cycles
    return (int)(MAX_PERF_SMOOTH_ALPHA * (double)raw_max_perf_pct
               + (1.0 - MAX_PERF_SMOOTH_ALPHA) * (double)prev);
}

static const char* EPP_NAMES[] = {
    "performance",
    "balance_performance",
    "balance_power",
    "power"
};

const char* epp_to_string(EppLevel e) {
    return EPP_NAMES[(int)e];
}

// ── Thermal helpers ──

double compute_thermal_pressure(double temp_c) {
    if (temp_c < 0 || temp_c <= 70.0) return 0.0;
    if (temp_c >= 90.0) return 1.0;
    return (temp_c - 70.0) / 20.0;  // linear from 70→90
}

int compute_thermal_max_perf(double temp_c) {
    if (temp_c < 0 || temp_c <= 70.0) return 100;
    if (temp_c >= 90.0) return 20;
    double pressure = (temp_c - 70.0) / 20.0;
    return 100 - (int)(pressure * 80.0);  // clamp [20, 100] implicit from above
}

int compute_thermal_no_turbo(double temp_c) {
    if (temp_c >= 82.0) return 1;
    return 0;
}

int compute_thermal_epp_tier(double temp_c) {
    if      (temp_c >= 85.0) return 2;  // power
    else if (temp_c >= 80.0) return 1;  // balance_power
    return 0;                            // balance_performance
}

double compute_thermal_headroom(double temp_c) {
    double pressure = compute_thermal_pressure(temp_c);
    return pressure * THERMAL_HEADROOM_R;
}

// ── Weight schedule ──

void compute_weights(const OptimizerInputs& inputs,
                     double& w_thermal, double& w_throttle, double& w_perf) {
    // Base weights — default to performance-oriented when idle
    w_thermal   = 1.0;
    w_throttle  = 0.0;
    w_perf      = 10.0;

    // Thermal weight increases with temperature
    if (inputs.temp_c >= 0) {
        double pressure = compute_thermal_pressure(inputs.temp_c);
        w_thermal = 1.0 + pressure * 9.0;  // 1.0 at 70°C → 10.0 at 90°C+
    }

    // GPU throttling → crank up thermal + throttle weights, drop perf weight
    if (inputs.gpu_throttling) {
        w_thermal   = std::max(w_thermal, 5.0);
        w_throttle  = std::max(w_throttle, 10.0);
        w_perf      = 0.5;
    }

    // GPU active (C0 residency) → moderate shift toward GPU headroom
    if (inputs.gpu_c0_pct >= 0.70) {
        w_thermal   = std::max(w_thermal, 3.0);
        w_throttle  = std::max(w_throttle, 5.0);
        w_perf      = std::min(w_perf, 2.0);
    } else if (inputs.gpu_c0_pct >= 0.30) {
        w_thermal   = std::max(w_thermal, 2.0);
        w_throttle  = std::max(w_throttle, 3.0);
        w_perf      = std::min(w_perf, 5.0);
    }

    // FALLBACK: GPU power-based weights when C0 residency is unavailable
    // or inaccurate (platforms without C0 sysfs, driver quirks, etc.).
    // If GPU power is high but C0 says idle, don't keep full CPU performance.
    if (!inputs.gpu_throttling && inputs.gpu_w > GPU_HEAVY_W) {
        w_throttle  = std::max(w_throttle, 3.0);
        w_perf      = std::min(w_perf, 5.0);
    }

    // Ensure weights are positive (no zero or negative)
    w_thermal   = std::max(w_thermal, 0.5);
    w_throttle  = std::max(w_throttle, 0.0);
    w_perf      = std::max(w_perf, 0.1);
}

int aggression_from_weights(double /*w_thermal*/, double w_throttle, double /*w_perf*/) {
    // Map weights to aggression levels matching the legacy model:
    // idle: low throttle weight, high perf weight
    // active: moderate throttle, moderate perf
    // throttling: high throttle, low perf
    if (w_throttle >= 5.0) return 2;
    if (w_throttle >= 2.0) return 1;
    return 0;
}

// ── Greedy solver ──

OptimizerResult solve(const OptimizerInputs& inputs) {
    OptimizerResult result;

    // ── Step 1: Compute weight schedule ──
    double w_thermal, w_throttle, w_perf;
    compute_weights(inputs, w_thermal, w_throttle, w_perf);
    result.weight_thermal   = w_thermal;
    result.weight_throttle  = w_throttle;
    result.weight_performance = w_perf;

    // ── Step 2: Compute thermal pressure and headroom ──
    (void)compute_thermal_pressure(inputs.temp_c);  // logged elsewhere
    double thermal_headroom = 0.0;
    if (inputs.have_coretemp || inputs.temp_c >= 0) {
        thermal_headroom = compute_thermal_headroom(inputs.temp_c);
    }

    // ── Step 3: Compute GPU headroom ──
    // Always reserve headroom for GPU: base margin + thermal extra + spike margin
    double gpu_headroom = HEADROOM_MARGIN_W + thermal_headroom;

    // Spike margin: GPU power can spike 25% above smoothed average within a 500ms window.
    // Reserve headroom for these spikes so the CPU doesn't consume power that the GPU
    // might need momentarily, preventing package PL1 hits that cause GPU self-throttle.
    // Only apply when GPU is doing real work (> 3W), not background noise.
    if (inputs.have_gpu && inputs.gpu_w > 3.0) {
        gpu_headroom += inputs.gpu_w * 0.25;
    }

    result.gpu_headroom_w = gpu_headroom;
    result.thermal_extra_w = thermal_headroom;

    // ── Step 4: Compute core power budget (constraint 1) ──
    double core_budget = inputs.pl1_w - inputs.gpu_w - gpu_headroom;

    // When GPU is throttling, cap core budget to prevent CPU from starving GPU
    if (w_throttle >= 5.0) {
        core_budget = std::min(CRITICAL_CPU_MAX_W, core_budget);
    }

    // Power can't be negative
    core_budget = std::max(core_budget, 0.0);

    // Round to 125mW granularity
    double core_limit_r = std::round(core_budget / 0.125) * 0.125;

    result.core_power_w = core_budget;
    result.core_limit_r = core_limit_r;

    // ── Step 5: Compute max_perf_pct ──
    // Primary constraint: GPU throttling → aggressive cap
    int max_perf = 100;

    if (w_throttle >= 5.0) {
        // Critical: GPU throttling → cap at CRITICAL_MAX_PERF
        max_perf = (int)CRITICAL_MAX_PERF;
    } else if (w_throttle >= 2.0) {
        // Active GPU: proportional scaling based on actual GPU power
        double ratio = std::min(inputs.gpu_w / GPU_HEAVY_W, 1.0);
        max_perf = 100 - (int)(ratio * (100.0 - ACTIVE_MAX_PERF));
        max_perf = std::max(max_perf, (int)ACTIVE_MAX_PERF);
    }
    // else: idle → 100%

    // Thermal override: temperature-based cap (most conservative wins)
    int thermal_max_perf = compute_thermal_max_perf(inputs.temp_c);
    max_perf = std::min(max_perf, thermal_max_perf);

    result.raw_max_perf_pct = max_perf;
    result.max_perf_pct = result.smooth_max_perf(inputs.prev_max_perf);

    // ── Step 6: Compute no_turbo ──
    int no_turbo = 0;
    if (w_throttle >= 5.0) {
        no_turbo = 1;  // GPU throttling → disable turbo
    }
    // Thermal override
    no_turbo = std::max(no_turbo, compute_thermal_no_turbo(inputs.temp_c));
    result.no_turbo = no_turbo;

    // ── Step 7: Compute EPP levels ──
    // Weight ratio determines aggressiveness:
    //   low w_perf → power-efficient (GPU priority)
    //   high w_perf → performance-oriented (CPU priority)
    EppLevel epp_p = EppLevel::BalancePerformance;
    EppLevel epp_e = EppLevel::BalancePerformance;

    if (w_perf <= 1.0) {
        // Low performance weight → power mode
        epp_p = EppLevel::Power;
        if (inputs.temp_c >= 90.0) {
            epp_e = EppLevel::Power;
        } else {
            epp_e = EppLevel::BalancePower;
        }
    } else if (w_perf <= 3.0) {
        // Medium weight → balance_power for P, balance_performance for E
        epp_p = EppLevel::BalancePower;
        epp_e = EppLevel::BalancePerformance;
    }
    // else: default BalancePerformance

    // Thermal override
    int thermal_epp_tier = compute_thermal_epp_tier(inputs.temp_c);
    if (thermal_epp_tier >= 2) {
        epp_p = EppLevel::Power;
        if (inputs.temp_c >= 90.0) {
            epp_e = EppLevel::Power;
        } else {
            epp_e = EppLevel::BalancePower;
        }
    } else if (thermal_epp_tier >= 1) {
        epp_p = EppLevel::BalancePower;
        epp_e = EppLevel::BalancePerformance;
    }

    result.epp_p = epp_p;
    result.epp_e = epp_e;

    // ── Step 8: Compute core groups to keep online ──
    // Map weight profile to core group count using a piecewise function:
    //   high throttle / high thermal → few groups (aggressive offlining)
    //   low throttle / low thermal → all groups (0 = all)
    int keep_groups = 0;  // 0 = all online

    if (w_throttle >= 5.0 || inputs.temp_c >= 90.0) {
        // Critical: 1 P-core group (2 threads)
        keep_groups = 1;
    } else if (inputs.temp_c >= 85.0) {
        // High thermal: 4 groups (3P + 1E = 7 threads)
        keep_groups = 4;
    } else if (w_throttle >= 2.0 || inputs.temp_c >= 80.0) {
        // Moderate load: 8 groups (6P + 2E = 14 threads)
        keep_groups = 8;
    } else if (w_perf <= 5.0 || inputs.temp_c >= 70.0) {
        // Active GPU or moderate temp: 12 groups (6P + 6E = 18 threads)
        keep_groups = 12;
    }
    // else: idle → 0 (all online)

    // Safety floor: always keep at least MIN_CORE_GROUPS
    keep_groups = std::max(keep_groups, MIN_CORE_GROUPS);
    // Cap at total available
    if (keep_groups > inputs.total_core_groups) {
        keep_groups = 0;  // not enough groups to offine
    }

    result.keep_groups = keep_groups;
    result.temp_c = inputs.temp_c;

    return result;
}
