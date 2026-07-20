// power-optimizer.cpp — Utility-maximizing power budget allocator
//
// Solves:
//   maximize  U = α·f_gpu(P_gpu) + β·f_cpu(P_cpu) - γ·h(T) - δ·I(throttle)
//   subject to: P_cpu + P_gpu + H ≤ PL1, P_cpu ≥ 0
//
// The continuous headroom problem has an analytical water-filling solution
// where marginal utilities are equalized. This is clamped to a probabilistic
// headroom floor that accounts for GPU power fluctuation risk.
//
// Controls (EPP, turbo, hotplug) are chosen to implement the optimal power
// allocation within the thermal safety envelope.

#include "power-optimizer.h"

// ═══════════════════════════════════════════════════════════
// Default configuration
// ═══════════════════════════════════════════════════════════

const OptimizerConfig default_config{};

// ═══════════════════════════════════════════════════════════
// EPP string conversion
// ═══════════════════════════════════════════════════════════

static const char* EPP_NAMES[] = {
    "performance",
    "balance_performance",
    "balance_power",
    "power"
};

const char* epp_to_string(EppLevel e) {
    return EPP_NAMES[static_cast<int>(e)];
}

// ═══════════════════════════════════════════════════════════
// Performance curves: f(P) = P_max * (1 - exp(-P / P_scale))
// ═══════════════════════════════════════════════════════════

double perf_curve(double p_max, double p_scale, double power_w) {
    if (power_w < 0) return 0;
    if (p_scale <= 0) return p_max;
    return p_max * (1.0 - std::exp(-power_w / p_scale));
}

double perf_marginal(double p_max, double p_scale, double power_w) {
    if (power_w < 0) return p_max / p_scale;
    if (p_scale <= 0) return 0;
    return (p_max / p_scale) * std::exp(-power_w / p_scale);
}

// ═══════════════════════════════════════════════════════════
// Thermal discomfort: smooth step from t_warn to t_max
// ═══════════════════════════════════════════════════════════

double thermal_discomfort(const OptimizerConfig& cfg, double temp_c) {
    if (temp_c < 0 || temp_c <= cfg.t_warn) return 0.0;
    if (temp_c >= cfg.t_max) return 1.0;
    // Sigmoid-like ramp: smooth but accelerates near t_max
    double x = (temp_c - cfg.t_warn) / (cfg.t_max - cfg.t_warn);
    // Use a cubic for sharper increase near the ceiling
    return x * x * (3.0 - 2.0 * x);
}

// ═══════════════════════════════════════════════════════════
// Analytical headroom solver
// ═══════════════════════════════════════════════════════════

// Water-filling: find headroom H where marginal GPU utility equals
// marginal CPU utility:
//   α·f_gpu'(P_gpu + H) = β·f_cpu'(P_cpu_budget - H)
//
// With exponential curves, this has a closed-form solution derived
// from equating the two marginal utility expressions.
//
// The result is clamped to the probabilistic headroom floor:
//   H_floor = H_base + z·H_nominal + thermal_extra + spike_margin
//
// Returns the optimal headroom in watts.
static double solve_headroom_analytical(const OptimizerConfig& cfg,
                                         double pl1_w, double gpu_w) {
    // Analytical solution: equate marginal utilities
    // α·(gpu_p_max/gpu_p_scale)·exp(-(P_gpu + H)/gpu_p_scale) =
    // β·(cpu_p_max/cpu_p_scale)·exp(-(P_cpu - H)/cpu_p_scale)
    //
    // Solving for H:
    // H = (PL1 - P_gpu - K) / (1 + cpu_p_scale/gpu_p_scale)
    // where K = gpu_p_scale * cpu_p_scale / (cpu_p_scale + gpu_p_scale) *
    //           ln(β * cpu_p_max * gpu_p_scale / (α * gpu_p_max * cpu_p_scale))

    double ratio = cfg.cpu_p_scale / cfg.gpu_p_scale;
    double K = (cfg.gpu_p_scale * cfg.cpu_p_scale) /
               (cfg.cpu_p_scale + cfg.gpu_p_scale);

    // Log of the utility weight ratio
    double log_term = std::log(
        (cfg.beta * cfg.cpu_p_max * cfg.gpu_p_scale) /
        (cfg.alpha * cfg.gpu_p_max * cfg.cpu_p_scale + 1e-12)
    );

    double H_opt = (pl1_w - gpu_w - K * log_term) / (1.0 + ratio);

    // Clamp to non-negative
    return std::max(H_opt, 0.0);
}

// Compute the probabilistic headroom floor.
// This is the minimum headroom we must reserve, based on risk tolerance.
// The z parameter controls risk aversion — higher z = more conservative.
static double solve_headroom_floor(const OptimizerConfig& cfg,
                                    double gpu_w, double thermal_extra,
                                    bool gpu_throttling) {
    double H = cfg.headroom_base;

    // Risk term: z * H_nominal. Scale up when GPU is active.
    H += cfg.headroom_z * cfg.headroom_nominal;

    // When GPU is throttling, be more conservative — scale up the floor
    if (gpu_throttling) {
        H *= 1.5;  // 50% more headroom when GPU is already throttling
    }

    // Thermal headroom: extra buffer to let CPU cool
    H += thermal_extra;

    // Spike margin: GPU power can spike above the smoothed average.
    // Reserve fraction of GPU power for these spikes.
    if (gpu_w > cfg.spike_min_gpu_w) {
        H += gpu_w * cfg.spike_ratio;
    }

    return H;
}

// ═══════════════════════════════════════════════════════════
// Control mapping: from power allocation to sysfs controls
// ═══════════════════════════════════════════════════════════

// Map CPU power budget to max_perf_pct.
// Model: P_cpu ≈ P_cpu_ref * (max_perf / 100)^2
// Inverting: max_perf ≈ 100 * sqrt(P_cpu / P_cpu_ref)
// P_cpu_ref is estimated as the CPU power at 100% max_perf.
static int map_cpu_power_to_perf_pct(double cpu_power_w, double cpu_power_ref,
                                      int min_perf, int max_perf) {
    if (cpu_power_ref <= 0) return max_perf;
    if (cpu_power_w <= 0) return min_perf;

    double ratio = cpu_power_w / cpu_power_ref;
    int perf = static_cast<int>(100.0 * std::sqrt(std::min(ratio, 1.0)));

    return static_cast<int>(std::clamp(static_cast<double>(perf),
                                        static_cast<double>(min_perf),
                                        static_cast<double>(max_perf)));
}

// Choose EPP level based on CPU power utilization fraction.
// Higher utilization → more performance-oriented EPP.
// Lower utilization → more power-efficient EPP.
static EppLevel choose_epp(double cpu_power_ratio, double temp_c,
                            const OptimizerConfig& cfg) {
    // Thermal override: temperature directly constrains EPP
    if (temp_c >= cfg.t_max - 5.0) {
        // Near thermal ceiling: force power mode
        return EppLevel::Power;
    }
    if (temp_c >= cfg.t_warn + 5.0) {
        // Approaching thermal warning: balance_power at least
        if (cpu_power_ratio > 0.7) return EppLevel::BalancePower;
        return EppLevel::BalancePower;
    }

    // Power-based EPP selection
    if (cpu_power_ratio > 0.85) {
        return EppLevel::BalancePerformance;
    }
    if (cpu_power_ratio > 0.5) {
        return EppLevel::BalancePower;
    }
    return EppLevel::Power;
}

// Choose whether turbo should be enabled.
// Turbo is allowed when thermal headroom is sufficient and GPU isn't throttling.
static bool choose_turbo(double temp_c, double cpu_power_w, double cpu_power_ref,
                          bool gpu_throttling, const OptimizerConfig& cfg) {
    // GPU throttling → disable turbo (give CPU power to GPU)
    if (gpu_throttling) return false;

    // Thermal constraints
    if (temp_c >= cfg.t_warn + 5.0) return false;

    // CPU is using less than 70% of its reference power — turbo is safe
    if (cpu_power_ref > 0 && cpu_power_w / cpu_power_ref < 0.7) return true;

    return false;
}

// Choose core groups to keep online based on CPU power needs.
// Fewer groups = less leakage = more headroom for GPU.
static int choose_keep_groups(double cpu_power_w, double cpu_power_ref,
                               int total_groups, const OptimizerConfig& cfg) {
    if (cpu_power_ref <= 0) return 0;  // all online
    double ratio = cpu_power_w / cpu_power_ref;

    // If CPU needs most of its power, keep all groups
    if (ratio > 0.9) return 0;

    // Scale groups down as CPU power needs decrease
    if (ratio > 0.7) return std::min(total_groups - 2, 12);
    if (ratio > 0.5) return std::min(total_groups - 4, 8);
    if (ratio > 0.3) return std::min(total_groups - 6, 4);
    return std::max(cfg.min_core_groups, 1);
}

// ═══════════════════════════════════════════════════════════
// Smoothing helpers
// ═══════════════════════════════════════════════════════════

int OptimizerResult::smooth_max_perf(int prev) const {
    return static_cast<int>(default_config.max_perf_smooth_alpha *
           static_cast<double>(raw_max_perf_pct) +
           (1.0 - default_config.max_perf_smooth_alpha) *
           static_cast<double>(prev));
}

int OptimizerResult::smooth_max_perf_static(int raw, int prev, double alpha) {
    return static_cast<int>(alpha * static_cast<double>(raw) +
           (1.0 - alpha) * static_cast<double>(prev));
}

// ═══════════════════════════════════════════════════════════
// Thermal headroom: temperature → extra power buffer
// ═══════════════════════════════════════════════════════════

static double compute_thermal_headroom_w(const OptimizerConfig& cfg, double temp_c) {
    double discomfort = thermal_discomfort(cfg, temp_c);
    return discomfort * cfg.thermal_headroom_max;
}

// ═══════════════════════════════════════════════════════════
// Sample and power computation (for tests)
// ═══════════════════════════════════════════════════════════

double compute_power_w(const Sample& prev, const Sample& cur) {
    if (prev.energy_uj < 0 || cur.energy_uj < 0) return 0;
    long long delta_uj = cur.energy_uj - prev.energy_uj;
    if (delta_uj < 0) return -1.0;
    auto delta_us = std::chrono::duration_cast<std::chrono::microseconds>(
        cur.time - prev.time).count();
    if (delta_us <= 0) return 0.0;
    return static_cast<double>(delta_uj) / delta_us;
}

double round_to_125mw(double watts) {
    if (watts < 0) return 0;
    return std::round(watts / 0.125) * 0.125;
}

// ═══════════════════════════════════════════════════════════
// Core solver
// ═══════════════════════════════════════════════════════════

OptimizerResult solve(const OptimizerInputs& inputs) {
    OptimizerResult result{};
    const auto& cfg = (inputs.config) ? *inputs.config : default_config;

    // ── Step 1: Compute thermal headroom ──
    double thermal_extra = 0.0;
    if (inputs.temp_c >= 0) {
        thermal_extra = compute_thermal_headroom_w(cfg, inputs.temp_c);
    }
    result.thermal_extra_w = thermal_extra;

    // ── Step 2: Solve for optimal headroom ──
    // Analytical water-filling gives the utility-optimal headroom.
    // Probabilistic floor ensures we never starve the GPU.
    double H_analytical = solve_headroom_analytical(cfg, inputs.pl1_w, inputs.gpu_w);
    double H_floor = solve_headroom_floor(cfg, inputs.gpu_w, thermal_extra,
                                           inputs.gpu_throttling);

    // Optimal headroom: take the more conservative of analytical and floor
    double gpu_headroom = std::max(H_analytical, H_floor);
    result.gpu_headroom_w = gpu_headroom;

    // ── Step 3: Compute CPU power budget ──
    double cpu_budget = inputs.pl1_w - inputs.gpu_w - gpu_headroom;

    // GPU throttling → hard cap on CPU power
    if (inputs.gpu_throttling) {
        cpu_budget = std::min(cpu_budget, cfg.cpu_critical_max_w);
    }

    cpu_budget = std::max(cpu_budget, 0.0);

    // Round to RAPL granularity
    double core_limit_r = std::round(cpu_budget / 0.125) * 0.125;
    result.core_power_w = cpu_budget;
    result.core_limit_r = core_limit_r;

    // ── Step 4: Map CPU budget to max_perf_pct ──
    // Estimate CPU power reference: the CPU power when running at 100% max_perf.
    // Approximated as PL1 minus GPU power when GPU is idle.
    double cpu_power_ref = inputs.pl1_w - (inputs.have_gpu ? inputs.gpu_w * 0.1 : 0.0);

    int max_perf = map_cpu_power_to_perf_pct(cpu_budget, cpu_power_ref,
                                              cfg.cpu_min_perf, cfg.cpu_max_perf);

    // Thermal override: temperature directly constrains max_perf
    if (inputs.temp_c >= cfg.t_warn) {
        double pressure = (inputs.temp_c - cfg.t_warn) / (cfg.t_max - cfg.t_warn);
        pressure = std::min(pressure, 1.0);
        int thermal_cap = cfg.cpu_max_perf -
                          static_cast<int>(pressure *
                          (cfg.cpu_max_perf - cfg.cpu_min_perf));
        max_perf = std::min(max_perf, std::max(thermal_cap, cfg.cpu_min_perf));
    }

    result.raw_max_perf_pct = max_perf;
    result.max_perf_pct = result.smooth_max_perf_static(
        max_perf, inputs.prev_max_perf, cfg.max_perf_smooth_alpha);

    // ── Step 5: Choose turbo ──
    result.no_turbo = choose_turbo(inputs.temp_c, cpu_budget, cpu_power_ref,
                                    inputs.gpu_throttling, cfg) ? 0 : 1;

    // Thermal override for turbo
    if (inputs.temp_c >= cfg.t_warn + 2.0) {
        result.no_turbo = 1;
    }

    // ── Step 6: Choose EPP levels ──
    double cpu_ratio = (cpu_power_ref > 0) ? cpu_budget / cpu_power_ref : 0;
    result.epp_p = choose_epp(cpu_ratio, inputs.temp_c, cfg);

    // E-cores get one tier less aggressive (more performance-oriented)
    if (result.epp_p == EppLevel::Power && inputs.temp_c < cfg.t_warn) {
        result.epp_e = EppLevel::BalancePower;
    } else {
        result.epp_e = result.epp_p;
    }

    // Thermal override for EPP
    if (inputs.temp_c >= cfg.t_max - 5.0) {
        result.epp_p = EppLevel::Power;
        result.epp_e = EppLevel::Power;
    }

    // ── Step 7: Choose core groups ──
    int keep_groups = choose_keep_groups(cpu_budget, cpu_power_ref,
                                          inputs.total_core_groups, cfg);
    keep_groups = std::max(keep_groups, cfg.min_core_groups);
    if (keep_groups > inputs.total_core_groups) {
        keep_groups = 0;  // not enough groups to offline
    }
    result.keep_groups = keep_groups;

    // ── Step 8: Compute utility (for diagnostics) ──
    result.util_gpu = cfg.alpha * perf_curve(cfg.gpu_p_max, cfg.gpu_p_scale,
                                              inputs.gpu_w);
    result.util_cpu = cfg.beta * perf_curve(cfg.cpu_p_max, cfg.cpu_p_scale,
                                             cpu_budget);
    result.util_thermal = -cfg.gamma * thermal_discomfort(cfg, inputs.temp_c);
    result.util_throttle = inputs.gpu_throttling ? -cfg.delta : 0.0;
    result.util_total = result.util_gpu + result.util_cpu +
                        result.util_thermal + result.util_throttle;

    // ── Step 9: Diagnostics ──
    result.temp_c = inputs.temp_c;

    // Legacy weight diagnostics (for compatibility with existing logging)
    compute_weights(inputs, result.weight_thermal, result.weight_throttle,
                    result.weight_performance);

    return result;
}

// ═══════════════════════════════════════════════════════════
// Legacy compatibility helpers
// ═══════════════════════════════════════════════════════════

double compute_thermal_pressure(double temp_c) {
    if (temp_c < 0 || temp_c <= 70.0) return 0.0;
    if (temp_c >= 90.0) return 1.0;
    return (temp_c - 70.0) / 20.0;
}

int compute_thermal_max_perf(double temp_c) {
    if (temp_c < 0 || temp_c <= 70.0) return 100;
    if (temp_c >= 90.0) return 20;
    double pressure = (temp_c - 70.0) / 20.0;
    return std::max(20, 100 - static_cast<int>(pressure * 80.0));
}

int compute_thermal_no_turbo(double temp_c) {
    return (temp_c >= 82.0) ? 1 : 0;
}

int compute_thermal_epp_tier(double temp_c) {
    if (temp_c >= 85.0) return 2;
    if (temp_c >= 80.0) return 1;
    return 0;
}

double compute_thermal_headroom(double temp_c) {
    return compute_thermal_pressure(temp_c) * 5.0;
}

// Legacy weight computation — maps to the new solver's behavior
void compute_weights(const OptimizerInputs& inputs,
                     double& w_thermal, double& w_throttle, double& w_perf) {
    w_thermal = 1.0;
    w_throttle = 0.0;
    w_perf = 10.0;

    if (inputs.temp_c >= 0) {
        double pressure = compute_thermal_pressure(inputs.temp_c);
        w_thermal = 1.0 + pressure * 9.0;
    }

    if (inputs.gpu_throttling) {
        w_thermal = std::max(w_thermal, 5.0);
        w_throttle = 10.0;
        w_perf = 0.5;
    }

    if (inputs.gpu_c0_pct >= 0.70) {
        w_thermal = std::max(w_thermal, 3.0);
        w_throttle = std::max(w_throttle, 5.0);
        w_perf = std::min(w_perf, 2.0);
    } else if (inputs.gpu_c0_pct >= 0.30) {
        w_thermal = std::max(w_thermal, 2.0);
        w_throttle = std::max(w_throttle, 3.0);
        w_perf = std::min(w_perf, 5.0);
    }

    if (!inputs.gpu_throttling && inputs.gpu_w > 15.0) {
        w_throttle = std::max(w_throttle, 3.0);
        w_perf = std::min(w_perf, 5.0);
    }

    w_thermal = std::max(w_thermal, 0.5);
    w_perf = std::max(w_perf, 0.1);
}

int aggression_from_weights(double /*w_thermal*/, double w_throttle,
                             double /*w_perf*/) {
    if (w_throttle >= 5.0) return 2;
    if (w_throttle >= 2.0) return 1;
    return 0;
}
