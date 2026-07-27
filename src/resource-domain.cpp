// resource-domain.cpp — Generic resource model implementation
//
// Platform-agnostic solver: takes observed power state and config,
// returns optimal power allocation per domain + control decisions.
// No RAPL, no intel_pstate, no GPU specifics — just watts and constraints.

#include "resource-domain.h"

#include <algorithm>
#include <cmath>
#include <limits>

// ═══════════════════════════════════════════════════════════
// Default configuration
// ═══════════════════════════════════════════════════════════

const ResourceConfig default_resource_config{};

// ═══════════════════════════════════════════════════════════
// EPP string conversion
// ═══════════════════════════════════════════════════════════

const char* epp_to_string(EppLevel e) {
    switch (e) {
        case EppLevel::Performance:         return "performance";
        case EppLevel::BalancePerformance:  return "balance_performance";
        case EppLevel::BalancePower:        return "balance_power";
        case EppLevel::Power:               return "power";
    }
    return "balance_performance";
}

// ═══════════════════════════════════════════════════════════
// Thermal surrender — disabled. Temperature is ambient on laptops,
// not a CPU power signal. The GPU-first budget is the real constraint.
// ═══════════════════════════════════════════════════════════

double thermal_surrender_fraction(const ResourceConfig& /*cfg*/, double /*temp_c*/) {
    return 0.0;
}

// ═══════════════════════════════════════════════════════════
// GPU headroom — single measured variance model
// ═══════════════════════════════════════════════════════════

double compute_gpu_headroom(const ResourceConfig& cfg, double gpu_w,
                            double gpu_variance, bool gpu_throttling) {
    // Base margin: risk_tolerance * measured_variance
    double H = cfg.risk_tolerance * gpu_variance;

    // Minimum headroom: even with zero variance, reserve a small buffer
    H = std::max(H, 1.0);  // 1 W minimum

    // When GPU is throttling, be more conservative
    if (gpu_throttling) {
        H *= 1.5;
    }

    // Spike margin: GPU power can spike above smoothed average
    if (gpu_w > 3.0) {
        H += gpu_w * 0.25;  // 25% of GPU power
    }

    return H;
}

// ═══════════════════════════════════════════════════════════
// Control decision helpers
// ═══════════════════════════════════════════════════════════

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

static int smooth_max_perf(int raw, int prev, double alpha) {
    return static_cast<int>(alpha * static_cast<double>(raw) +
           (1.0 - alpha) * static_cast<double>(prev));
}

// EPP level ordering for hysteresis: Performance(0) > BalancePerformance(1)
// > BalancePower(2) > Power(3). Higher = more power-constrained.
static constexpr int epp_index(EppLevel e) {
    switch (e) {
        case EppLevel::Performance:          return 0;
        case EppLevel::BalancePerformance:   return 1;
        case EppLevel::BalancePower:         return 2;
        case EppLevel::Power:                return 3;
    }
    return 2;
}

// Apply hysteresis to EPP selection. Only switch when the new level differs
// significantly from the previous one, preventing balance_power↔power
// flip-flopping around ratio boundaries.
static EppLevel apply_epp_hysteresis(EppLevel new_epp, EppLevel prev_epp,
                                      double cpu_power_ratio) {
    if (new_epp == prev_epp) return prev_epp;

    int diff = epp_index(new_epp) - epp_index(prev_epp);

    // >= 2 tiers difference → always switch (big signal)
    if (std::abs(diff) >= 2) return new_epp;

    // 1-tier difference → only switch if ratio has moved well past boundary.
    if (diff > 0) {
        // Going more power-constrained: ratio must be well below boundary
        if (prev_epp == EppLevel::BalancePower && new_epp == EppLevel::Power) {
            return (cpu_power_ratio < 0.35) ? new_epp : prev_epp;
        }
        if (prev_epp == EppLevel::BalancePerformance && new_epp == EppLevel::BalancePower) {
            return (cpu_power_ratio < 0.70) ? new_epp : prev_epp;
        }
    } else {
        // Going less power-constrained: ratio must be well above boundary
        if (prev_epp == EppLevel::Power && new_epp == EppLevel::BalancePower) {
            return (cpu_power_ratio > 0.65) ? new_epp : prev_epp;
        }
        if (prev_epp == EppLevel::BalancePower && new_epp == EppLevel::BalancePerformance) {
            return (cpu_power_ratio > 1.0) ? new_epp : prev_epp; // effectively never
        }
    }
    return prev_epp;
}

static EppLevel choose_epp(double cpu_power_ratio,
                            EppLevel prev_epp) {
    // EPP based purely on power ratio — no thermal overrides.
    // Temperature is an ambient laptop condition; the GPU-first budget
    // allocation (PL1 - gpu - headroom) is the only real power signal.
    EppLevel raw;
    if (cpu_power_ratio > 0.85) {
        raw = EppLevel::BalancePerformance;
    } else if (cpu_power_ratio > 0.5) {
        raw = EppLevel::BalancePower;
    } else {
        raw = EppLevel::Power;
    }
    return apply_epp_hysteresis(raw, prev_epp, cpu_power_ratio);
}

// Choose whether turbo should be enabled.
// Returns true = turbo ON, false = turbo OFF.
static bool choose_turbo(double cpu_measured_w,
                          double cpu_power_ref, bool gpu_throttling) {
    // GPU throttling → disable turbo (give CPU power to GPU)
    if (gpu_throttling) return false;

    // CPU's actual draw is less than 70% of its capacity —
    // there's headroom for occasional turbo bursts.
    if (cpu_power_ref > 0 && cpu_measured_w / cpu_power_ref < 0.7) return true;

    return false;
}

// Choose core groups to keep online based on CPU power needs, scheduler demand,
// and GPU activity. Fewer groups = less leakage = more headroom for GPU.
// Returns 0 (keep all online) or N > 0 (keep exactly N groups).
//
// Hysteresis: when the GPU is near the idle/active boundary, we prefer the
// current state to avoid ping-pong.  The offline threshold is higher than the
// online threshold (gpu_active_thresh > gpu_idle_thresh), creating a dead band.
//
// Budget-driven hotplug: map CPU power budget to core count.
// GPU activity is implicit: GPU power draw is already subtracted from cpu_budget
// (via gpu_headroom).  The budget/draw ratio tells us how much excess we have.
// P-cores are always preferred over E-cores (apply_hotplug keeps first `target` groups).
//
// Policy:
//   ratio > 1.5 → plenty of headroom → keep P-cores only (always shed E leakage)
//   ratio 0.8-1.5 → scale E-cores proportionally
//   ratio < 0.8  → budget tight → scale all cores aggressively
//   demand floors act as safety nets (never drop below what scheduler needs)
static int choose_keep_groups(double cpu_budget, double cpu_power_ref,
                               int total_groups, int pcore_count,
                               double cpu_demand, int running_tasks,
                               int prev_keep_groups) {
    // ── Safety floors ──

    // Runnable tasks: if system is oversubscribed, keep all online.
    if (running_tasks > 0 && total_groups > 0) {
        int threshold = (total_groups * 80) / 100;  // 80%
        if (running_tasks >= threshold) return 0;
    }

    // Saturated demand: keep all online (CPU is the bottleneck).
    if (cpu_demand >= 0.95) return 0;

    // ── Budget-vs-draw core mapping ──
    double ratio = (cpu_power_ref > 0) ? cpu_budget / cpu_power_ref : 1.0;
    int ecore_count = total_groups - pcore_count;

    // Clamp ratio to [0.1, 2.0] to avoid wild swings
    ratio = std::max(0.1, std::min(2.0, ratio));

    int target;
    if (ratio > 1.5) {
        // Ample budget: keep P-cores only, always shed E-core leakage.
        // Even idle systems don't need E-core leakage burning.
        target = pcore_count;
    } else if (ratio > 0.8) {
        // Moderate headroom: scale E-cores in proportion to excess budget.
        // At ratio=1.5 → pcore_count + ecore_count (all online)
        // At ratio=0.8 → pcore_count + 0 (P-only)
        double e_fraction = (ratio - 0.8) / 0.7;  // 0.0–1.0
        target = pcore_count + static_cast<int>(ecore_count * e_fraction);
    } else {
        // Tight budget: scale all cores by ratio, min 1.
        target = std::max(1, static_cast<int>(total_groups * ratio));
    }

    // ── Demand safety floors ──
    // Scheduler demand acts as a minimum — never drop below what the
    // scheduler says is needed.  Budget decides the ceiling; demand is the floor.
    if (cpu_demand > 0.75) {
        target = std::max(target, total_groups - 2);
    } else if (cpu_demand > 0.4) {
        target = std::max(target, total_groups / 2);
    } else if (cpu_demand > 0.15) {
        target = std::max(target, pcore_count);
    }

    // Clamp to valid range
    target = std::max(1, std::min(target, total_groups));

    // ── Smoothing: prefer previous value if close ──
    // Prevents rapid online/offline transitions near decision boundaries.
    if (prev_keep_groups > 0 && target > 0) {
        if (std::abs(target - prev_keep_groups) <= 1) {
            target = prev_keep_groups;
        }
    }

    return target;
}

// Aggression level from GPU throttle state and demand.
// 0 = idle, 1 = active, 2 = aggressive throttle
static int compute_aggression(bool gpu_throttling, double cpu_demand,
                               double cpu_budget, double cpu_power_ref) {
    if (gpu_throttling) return 2;

    double ratio = (cpu_power_ref > 0) ? cpu_budget / cpu_power_ref : 0;
    if (cpu_demand > 0.75 || ratio > 0.7) return 1;
    return 0;
}

// ═══════════════════════════════════════════════════════════
// Core solver — policy layer
// ═══════════════════════════════════════════════════════════

ResourceResult solve_resources(const ResourceInputs& inputs,
                               const ResourceConfig& config) {
    ResourceResult result{};

    // ── Step 1: Compute GPU headroom ──
    double headroom = compute_gpu_headroom(
        config, inputs.gpu_power_w, inputs.gpu_power_var_w,
        inputs.gpu_throttling);

    // If no GPU, zero headroom — full PL1 available for CPU
    if (!inputs.have_gpu) {
        headroom = 0.0;
    }

    result.gpu_headroom_w = headroom;
    result.risk_margin_w = config.risk_tolerance * inputs.gpu_power_var_w;

    // ── Step 2: Initial CPU budget ──
    double cpu_budget = inputs.pl1_w - inputs.gpu_power_w - headroom;

    // ── Step 3: Thermal surrender removed ──
    // Temperature is an ambient laptop condition, not a CPU power signal.
    // The GPU-first budget (PL1 - gpu - headroom) is the only real constraint.
    result.thermal_surrender = 0.0;

    // ── Step 4: Scheduler demand scaling ──
    // demand_factor: 0.5 at demand=0 (idle), 1.0 at demand=1 (saturated)
    double demand_factor = 0.5 + 0.5 * inputs.cpu_demand;
    cpu_budget *= demand_factor;
    result.demand_factor = demand_factor;

    // ── Step 5: Safety clamps ──
    // GPU throttling → hard cap on CPU power
    if (inputs.gpu_throttling) {
        cpu_budget = std::min(cpu_budget, config.cpu_critical_w);
    }

    // CPU domain hard limit (RAPL constraint_0_max_power_uw)
    if (inputs.cpu_domain_max_w > 0) {
        cpu_budget = std::min(cpu_budget, inputs.cpu_domain_max_w);
    }

    // Floor and ceiling
    cpu_budget = std::max(cpu_budget, config.cpu_min_w);
    cpu_budget = std::min(cpu_budget, config.cpu_max_w);
    cpu_budget = std::max(cpu_budget, 0.0);

    result.cpu_target_w = cpu_budget;

    // GPU target is the measured GPU power (we don't control it directly)
    result.gpu_target_w = inputs.gpu_power_w;

    // ── Step 6: Core limit (rounded to RAPL granularity) ──
    result.core_limit_w = std::round(cpu_budget / 0.125) * 0.125;

    // ── Step 7: Compute CPU power reference (capacity-based) ──
    // Reference power: use RAPL domain max (capacity) when available.
    // Falls back to a fraction of PL1 since cpu_domain_max_w may not be exposed
    // on all platforms (e.g. Meteor Lake core domain has empty max_power).
    double cpu_power_ref;
    if (inputs.cpu_domain_max_w > 0) {
        cpu_power_ref = inputs.cpu_domain_max_w;
    } else {
        // Fallback: CPU can use up to ~half of PL1 when GPU is light, less when
        // GPU is heavy. This is a reasonable worst-case capacity estimate.
        double gpu_contribution = inputs.have_gpu ? inputs.gpu_power_w * 0.15 : 0.0;
        cpu_power_ref = inputs.pl1_w - gpu_contribution;
    }

    // ── Step 8: Map CPU budget to max_perf_pct ──
    int raw_max_perf = map_cpu_power_to_perf_pct(
        cpu_budget, cpu_power_ref, config.cpu_min_perf, config.cpu_max_perf);

    // Smooth max_perf across cycles
    result.max_perf_pct = smooth_max_perf(
        raw_max_perf, inputs.prev_max_perf, config.max_perf_smooth_alpha);

    // ── Step 9: Choose turbo ──
    // Turbo is safe when the CPU's actual draw is well below its capacity.
    result.no_turbo = choose_turbo(
        inputs.cpu_measured_w, cpu_power_ref,
        inputs.gpu_throttling) ? 0 : 1;

    // ── Step 10: Choose EPP levels (with hysteresis) ──
    double cpu_ratio = (cpu_power_ref > 0) ? cpu_budget / cpu_power_ref : 0;
    result.epp_p = choose_epp(cpu_ratio, inputs.prev_epp_p);

    // Relaxed state (no GPU throttle) → E-cores 1 tier less aggressive.
    // Thermal overrides removed — EPP is power-ratio driven.
    if (!inputs.gpu_throttling) {
        if (result.epp_p == EppLevel::Power) {
            result.epp_e = EppLevel::BalancePower;
        } else if (result.epp_p == EppLevel::BalancePower) {
            result.epp_e = EppLevel::BalancePerformance;
        } else {
            result.epp_e = result.epp_p;
        }
    } else {
        result.epp_e = result.epp_p;
    }

    // Apply E-core hysteresis independently
    result.epp_e = apply_epp_hysteresis(result.epp_e, inputs.prev_epp_e, cpu_ratio);

    // ── Step 11: Choose core groups (hotplug) ──
    // Pass actual measured CPU power (not theoretical capacity) so the
    // budget/draw ratio reflects real utilization, not theoretical limits.
    // Falls back to cpu_power_ref (capacity) if no measurement available.
    double cpu_draw = (inputs.cpu_measured_w > 0) ? inputs.cpu_measured_w : cpu_power_ref;
    int keep_groups = choose_keep_groups(
        cpu_budget, cpu_draw, inputs.total_core_groups,
        inputs.pcore_count, inputs.cpu_demand,
        inputs.running_tasks, inputs.prev_keep_groups);

    // Don't clamp 0 (keep all) — that's the most conservative policy.
    if (keep_groups > 0) {
        keep_groups = std::max(keep_groups, config.min_core_groups);
    }
    if (keep_groups > inputs.total_core_groups) {
        keep_groups = 0;  // not enough groups to offline
    }

    result.keep_groups = keep_groups;

    // ── Step 12: Compute aggression level (for logging) ──
    result.aggression = compute_aggression(
        inputs.gpu_throttling, inputs.cpu_demand,
        cpu_budget, cpu_power_ref);

    return result;
}
