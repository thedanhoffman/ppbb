// resource-domain.h — Generic resource model for power budgeting
//
// Provides a platform-agnostic abstraction for power domains and constraints.
// The solver operates on this model; platform-specific actuator code maps
// the solver's output to sysfs/MSR/other control interfaces.
//
// Architecture:
//   Solver outputs: ResourceResult (target power + control decisions)
//   Actuator inputs: ResourceResult → concrete sysfs writes
//   Platform-agnostic: no RAPL, no intel_pstate, no GPU specifics here

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

// ═══════════════════════════════════════════════════════════
// EPP levels (exposed by intel_pstate / CPUFreq)
// ═══════════════════════════════════════════════════════════

enum class EppLevel {
    Performance,          // "performance"
    BalancePerformance,   // "balance_performance"
    BalancePower,         // "balance_power"
    Power                 // "power"
};

// Convert EppLevel to sysfs string
const char* epp_to_string(EppLevel e);

// ═══════════════════════════════════════════════════════════
// ResourceDomain — a power-managed resource on the system
// ═══════════════════════════════════════════════════════════

struct ResourceDomain {
    std::string name;               // "cpu", "gpu", "uncore", "npu", etc.
    double    current_power_w     = 0.0;  // measured instantaneous power
    double    max_power_w         = 0.0;  // hardware constraint_0_max_power_uw
    double    min_power_w         = 0.0;  // hardware minimum (0 = unlimited)
    double    granularity_w       = 0.125; // control granularity (RAPL = 125 mW)

    // Function pointer for budget-setting (populated by actuator)
    // Called with the target power in watts. Returns true on success.
    std::function<bool(double watts)> set_budget;

    // Latency to change budget in milliseconds (for control selection)
    double latency_ms = 10.0;

    // Optional: parent domain name (for hierarchical constraints)
    std::string parent;

    ResourceDomain() = default;
    ResourceDomain(std::string n, double cur, double mx,
                   std::function<bool(double)> setter)
        : name(std::move(n)), current_power_w(cur), max_power_w(mx),
          set_budget(std::move(setter)) {}
};

// ═══════════════════════════════════════════════════════════
// SharedConstraint — a shared power limit across domains
// ═══════════════════════════════════════════════════════════

struct SharedConstraint {
    std::string name;               // "package-PL1", "TDP-limit", etc.
    double    limit_w     = 0.0;    // shared power budget (PL1, TDP, etc.)
    std::vector<ResourceDomain*> members; // domains sharing this budget

    SharedConstraint() = default;
    SharedConstraint(std::string n, double limit,
                     std::vector<ResourceDomain*> doms)
        : name(std::move(n)), limit_w(limit), members(std::move(doms)) {}
};

// ═══════════════════════════════════════════════════════════
// ResourceResult — solver output: target power + control decisions
// ═══════════════════════════════════════════════════════════

struct ResourceResult {
    // Target power allocation per domain (the solver's main output)
    double cpu_target_w     = 0.0;  // optimal CPU power budget (W)
    double gpu_target_w     = 0.0;  // optimal GPU power budget (W)
    double gpu_headroom_w   = 0.0;  // reserved GPU headroom (W)

    // Power control
    double core_limit_w     = 0.0;  // core RAPL limit (rounded to 125mW granularity)

    // CPU frequency controls (intel_pstate)
    int    max_perf_pct     = 100;  // max_perf_pct target (0-100)
    int    no_turbo         = 0;    // 0 = turbo ON, 1 = turbo OFF

    // CPU energy policy
    EppLevel epp_p          = EppLevel::BalancePerformance;  // P-core EPP
    EppLevel epp_e          = EppLevel::BalancePerformance;  // E-core EPP

    // CPU topology (hotplug)
    int    keep_groups      = 0;    // core groups to keep online (0 = keep all)

    // Diagnostics
    double thermal_surrender = 0.0; // UNUSED — thermal surrender disabled (always 0.0)
    double demand_factor     = 1.0; // scheduler demand scaling (0.5-1.0)
    double risk_margin_w     = 0.0; // probabilistic headroom margin (W)
    int    aggression        = 0;   // 0=idle, 1=active, 2=aggressive-throttle

    // Effective CPU budget (thermal surrender removed —
    // temperature is ambient, not a power signal).
    double effective_cpu_w() const {
        return cpu_target_w;
    }
};

// ═══════════════════════════════════════════════════════════
// ResourceInputs — solver input: observed system state
// ═══════════════════════════════════════════════════════════

struct ResourceInputs {
    // Shared constraint
    double  pl1_w              = 40.0;   // shared power budget (W)
    double  gpu_power_w        = 0.0;    // measured GPU power (W)
    bool    have_gpu           = false;  // GPU present

    // Thermal
    double  temp_c             = -1.0;   // max pkg temp (-1 = unknown)

    // Scheduler demand (0.0 = idle, 1.0 = saturated)
    double  cpu_demand         = 1.0;

    // GPU C0 residency (0.0-1.0, used for weight scaling)
    double  gpu_c0_pct         = 0.0;

    // GPU power variance (measured over recent samples, for headroom calc)
    double  gpu_power_var_w    = 0.0;

    // CPU measured power at current settings (for reference)
    double  cpu_measured_w     = 0.0;

    // CPU domain hard limit (constraint_0_max_power_uw, 0 = none)
    double  cpu_domain_max_w   = 0.0;

    // GPU throttle state (affects penalty)
    bool    gpu_throttling     = false;

    // CPU topology
    int     total_core_groups  = 8;  // total physical core groups on system
    int     pcore_count        = 0;  // number of P-core groups (rest are E-core)

    // Runnable tasks (from /proc/loadavg) — used as safety floor for hotplug
    int     running_tasks      = 0;  // tasks in runnable state

    // Previous cycle state (for smoothing/hysteresis)
    int     prev_max_perf      = 100;
    EppLevel prev_epp_p        = EppLevel::BalancePerformance;
    EppLevel prev_epp_e        = EppLevel::BalancePerformance;
    int     prev_keep_groups   = 0;  // previous keep_groups (for hysteresis)
};

// ═══════════════════════════════════════════════════════════
// Simplified configuration
// ═══════════════════════════════════════════════════════════

struct ResourceConfig {
    // ── Thermal thresholds (UNUSED — thermal removed from solver) ──
    // Kept for backward compatibility. Temperature is ambient on laptops,
    // not a CPU power signal. The GPU-first budget is the only real constraint.
    double t_target = 70.0;  // UNUSED
    double t_warn   = 80.0;  // UNUSED
    double t_max    = 90.0;  // UNUSED

    // ── Risk tolerance (for GPU headroom) ──
    // z-score multiplier on measured GPU power variance.
    // 1.0 → ~84% confidence, 1.5 → ~93%, 2.0 → ~97.5%
    double risk_tolerance = 1.5;

    // ── CPU safety limits ──
    double cpu_min_w  = 2.0;  // floor: minimum CPU power (W)
    double cpu_max_w  = 100.0;// ceiling: maximum CPU power (W)
    double cpu_critical_w = 8.0; // max CPU power when GPU throttling (W)

    // ── CPU control limits ──
    int cpu_min_perf = 20;    // floor for max_perf_pct
    int cpu_max_perf = 100;   // ceiling for max_perf_pct

    // ── Smoothing ──
    double max_perf_smooth_alpha = 0.5;  // EMA alpha for max_perf_pct

    // ── Safety ──
    int min_core_groups = 0;  // minimum core groups (0 = let optimizer decide)
};

// ═══════════════════════════════════════════════════════════
// Solver
// ═══════════════════════════════════════════════════════════

// Solve the resource allocation problem and compute all control decisions.
// Takes observed state and config, returns target power + control decisions.
// This is the policy layer — platform-agnostic.
ResourceResult solve_resources(const ResourceInputs& inputs,
                               const ResourceConfig& config);

// Default config
extern const ResourceConfig default_resource_config;

// ── Helper functions (used by solver and externally) ──

// Thermal surrender — disabled (always returns 0.0). Kept for ABI.
double thermal_surrender_fraction(const ResourceConfig& cfg, double temp_c);

// GPU headroom: risk_tolerance * measured_variance + base margin
double compute_gpu_headroom(const ResourceConfig& cfg, double gpu_w,
                            double gpu_variance, bool gpu_throttling);
