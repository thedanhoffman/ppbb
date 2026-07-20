// power-optimizer.h — Utility-maximizing power budget allocator
//
// Models the power balancer as a constrained optimization problem:
//
//   maximize  U = α·f_gpu(P_gpu) + β·f_cpu(P_cpu) - γ·h(T) - δ·I(throttle)
//   subject to:
//     P_cpu + P_gpu + H ≤ PL1         (shared power budget)
//     H = H_base + z·H_nominal        (probabilistic GPU headroom)
//     P_cpu ∈ [0, P_cpu_max]          (RAPL domain limits)
//
// Performance curves: saturating exponential model
//   f(P) = P_max · (1 - exp(-P / P_scale))
//
// Headroom: probabilistic framing with configurable risk parameter z.
//   H_nominal is a fixed parameter (not learned variance) representing
//   the typical magnitude of GPU power fluctuation within a polling window.
//   z scales this to achieve a target risk tolerance.
//
// Solver: analytical. The continuous problem (P_cpu allocation) has a closed-form
// solution derived from equating marginal utilities. The discrete controls
// (EPP, turbo, hotplug) are enumerated over the small feasible space and
// evaluated with the continuous optimum for each configuration.

#pragma once

#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdint>

// ═══════════════════════════════════════════════════════════
// EPP levels
// ═══════════════════════════════════════════════════════════

enum class EppLevel {
    Performance,          // "performance"
    BalancePerformance,   // "balance_performance"
    BalancePower,         // "balance_power"
    Power                 // "power"
};

const char* epp_to_string(EppLevel e);

// ═══════════════════════════════════════════════════════════
// Configuration: all tunable parameters in one place
// ═══════════════════════════════════════════════════════════

struct OptimizerConfig {
    // ── Utility weights ──
    double alpha = 1.0;   // GPU performance weight
    double beta  = 1.0;   // CPU performance weight
    double gamma = 2.0;   // thermal discomfort weight
    double delta = 10.0;  // throttle event penalty weight

    // ── Performance curves: f(P) = P_max * (1 - exp(-P / P_scale)) ──
    // CPU performance saturates near P_max_cpu as power increases.
    // P_scale_cpu controls the "knee" — lower = earlier saturation.
    double cpu_p_max   = 1.0;   // normalized CPU max performance
    double cpu_p_scale = 25.0;  // CPU power scale (W)

    // GPU performance saturates near P_max_gpu.
    // GPU curves are typically more sub-linear than CPU.
    double gpu_p_max   = 1.0;   // normalized GPU max performance
    double gpu_p_scale = 20.0;  // GPU power scale (W)

    // ── Probabilistic headroom ──
    // H = H_base + z * H_nominal + thermal_extra + spike_margin
    double headroom_base   = 2.0;  // fixed base headroom (W)
    double headroom_z      = 1.5;  // risk multiplier (z-score analog)
    double headroom_nominal = 3.0; // nominal fluctuation scale (W) — not learned variance

    // ── Thermal model ──
    double t_target = 70.0;  // target operating temperature (°C)
    double t_warn   = 80.0;  // thermal discomfort begins (°C)
    double t_max    = 90.0;  // thermal ceiling (°C)

    // ── Thermal headroom ──
    double thermal_headroom_max = 5.0;  // max extra headroom from thermal (W)

    // ── Spike margin ──
    double spike_ratio = 0.25;  // fraction of GPU power reserved for spikes
    double spike_min_gpu_w = 3.0;  // only apply spike margin above this GPU power

    // ── CPU control limits ──
    int cpu_min_perf = 20;     // floor for max_perf_pct
    int cpu_max_perf = 100;    // ceiling for max_perf_pct
    double cpu_critical_max_w = 8.0;  // max CPU power when GPU throttling (W)

    // ── Smoothing ──
    double max_perf_smooth_alpha = 0.5;  // EMA alpha for max_perf_pct

    // ── Safety ──
    int min_core_groups = 2;  // minimum core groups to keep online
};

// ═══════════════════════════════════════════════════════════
// Inputs: observed system state each cycle
// ═══════════════════════════════════════════════════════════

struct OptimizerInputs {
    // Power budget
    double pl1_w        = 40.0;   // desired package PL1 limit (W)
    double gpu_w        = 0.0;    // smoothed GPU power (W)
    bool   have_gpu     = false;  // GPU detected

    // Thermal
    double temp_c       = -1.0;   // max core temperature (°C), -1 = unknown
    bool   have_coretemp = false; // thermal sensors available

    // GPU state
    double gpu_c0_pct   = 0.0;    // GPU C0 residency fraction [0, 1]
    bool   gpu_throttling = false; // GPU throttle event detected

    // System topology
    int    total_core_groups = 8; // total physical core groups on system

    // Previous cycle state (for smoothing/hysteresis)
    int    prev_max_perf  = 100;  // previous max_perf_pct
    EppLevel prev_epp_p   = EppLevel::BalancePerformance;
    EppLevel prev_epp_e   = EppLevel::BalancePerformance;

    // Optional: reference to config (avoids passing it separately)
    const OptimizerConfig* config = nullptr;
};

// ═══════════════════════════════════════════════════════════
// Outputs: control decisions each cycle
// ═══════════════════════════════════════════════════════════

struct OptimizerResult {
    // Power budget allocation
    double core_power_w     = 0.0;    // optimal CPU power budget (W)
    double core_limit_r     = 0.0;    // rounded core limit (125mW granularity)
    double gpu_headroom_w   = 0.0;    // total reserved GPU headroom (W)
    double thermal_extra_w  = 0.0;    // extra headroom from thermal (W)

    // CPU frequency controls
    int    max_perf_pct     = 100;    // intel_pstate max_perf_pct (smoothed)
    int    raw_max_perf_pct = 100;    // unsmoothed max_perf_pct
    int    no_turbo         = 0;      // intel_pstate no_turbo (0 or 1)

    // CPU energy policy
    EppLevel epp_p          = EppLevel::BalancePerformance;  // P-core EPP
    EppLevel epp_e          = EppLevel::BalancePerformance;  // E-core EPP

    // CPU topology
    int    keep_groups      = 0;      // core groups to keep online (0 = all)

    // Diagnostics
    double util_cpu         = 0.0;    // CPU performance utility component
    double util_gpu         = 0.0;    // GPU performance utility component
    double util_thermal     = 0.0;    // thermal discomfort (negative contribution)
    double util_throttle    = 0.0;    // throttle penalty (negative contribution)
    double util_total       = 0.0;    // total utility
    double temp_c           = -1.0;   // max core temp (for logging)

    // Weight diagnostics (for logging)
    double weight_thermal   = 0.0;
    double weight_throttle  = 0.0;
    double weight_performance = 0.0;

    // Apply smoothing to max_perf_pct
    int smooth_max_perf(int prev) const;

    static int smooth_max_perf_static(int raw, int prev, double alpha);
};

// ═══════════════════════════════════════════════════════════
// Performance curve utilities (used by tests and solver)
// ═══════════════════════════════════════════════════════════

// Saturating performance curve: f(P) = P_max * (1 - exp(-P / P_scale))
double perf_curve(double p_max, double p_scale, double power_w);

// Marginal performance: df/dP = (P_max / P_scale) * exp(-P / P_scale)
double perf_marginal(double p_max, double p_scale, double power_w);

// Thermal discomfort: smooth step from t_warn to t_max
double thermal_discomfort(const OptimizerConfig& cfg, double temp_c);

// ═══════════════════════════════════════════════════════════
// Core solver
// ═══════════════════════════════════════════════════════════

// Solve the constrained utility maximization problem.
// Returns the optimal control decisions.
OptimizerResult solve(const OptimizerInputs& inputs);

// Default configuration
extern const OptimizerConfig default_config;

// ═══════════════════════════════════════════════════════════
// Legacy compatibility helpers (used by tests)
// ═══════════════════════════════════════════════════════════

// These are kept for backward compatibility with existing tests.
// They map to the new solver's behavior under default configuration.

double compute_thermal_pressure(double temp_c);
int    compute_thermal_max_perf(double temp_c);
int    compute_thermal_no_turbo(double temp_c);
int    compute_thermal_epp_tier(double temp_c);
double compute_thermal_headroom(double temp_c);

// Legacy weight computation (used for aggression level mapping)
void compute_weights(const OptimizerInputs& inputs,
                     double& w_thermal, double& w_throttle, double& w_perf);

int aggression_from_weights(double w_thermal, double w_throttle, double w_perf);

// Sample and power computation (used by tests)
struct Sample {
    long long energy_uj = -1;
    std::chrono::steady_clock::time_point time;
};

double compute_power_w(const Sample& prev, const Sample& cur);
double round_to_125mw(double watts);

// Legacy constants for backward compatibility with power-balance.cpp
static constexpr double MAX_PERF_SMOOTH_ALPHA = 0.5;
static constexpr int    MIN_CORE_GROUPS = 2;
static constexpr double HEADROOM_MARGIN_W = 2.0;
static constexpr double THERMAL_HEADROOM_R = 5.0;
static constexpr double CRITICAL_CPU_MAX_W = 8.0;
