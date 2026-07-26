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
// Smoothing: keep_groups is EWMA-smoothed across cycles to prevent rapid
// online/offline transitions.  The alpha is small (0.3) so changes ramp over
// 3-4 cycles (~1.5-2s at 200ms interval).
// running_tasks: if runnable tasks >= logical CPUs, never offline (safety floor).
static int choose_keep_groups(double cpu_budget, double cpu_power_ref,
                               int total_groups, int min_core_groups,
                               double cpu_demand, double gpu_power_w,
                               int prev_keep_groups, int running_tasks) {
    // ── Hysteresis thresholds ──
    const double gpu_idle_thresh  = 2.0;  // GPU below this → safe to keep all online
    const double gpu_active_thresh = 5.0; // GPU above this → start considering offlining
    // Dead band: [2.0, 5.0] — prefer current state

    // ── GPU-driven decision with hysteresis ──
    bool gpu_is_idle = (gpu_power_w < gpu_idle_thresh);
    bool gpu_is_active = (gpu_power_w >= gpu_active_thresh);
    bool currently_all_online = (prev_keep_groups == 0);

    if (gpu_is_idle && currently_all_online) {
        // GPU is clearly idle and we're already online → stay
        return 0;
    }
    if (gpu_is_idle && !currently_all_online) {
        // GPU just went idle → bring cores back (hysteresis: wait for clear idle)
        return 0;
    }
    if (!gpu_is_active && currently_all_online) {
        // GPU in dead band, currently online → stay (hysteresis)
        return 0;
    }
    if (gpu_is_active && !currently_all_online) {
        // GPU is active, already offline → evaluate based on demand
        // (fall through to demand logic below)
    } else if (gpu_is_active) {
        // GPU just crossed into active zone → evaluate
        // (fall through to demand logic below)
    } else {
        // Dead band, currently offline → stay offline (hysteresis)
        return prev_keep_groups;
    }

    // ── GPU is active — use scheduler demand + power budget ──
    // The CPU's allocated budget (after GPU headroom) tells us how tight
    // things are.  A tight budget means we should offline cores to reduce
    // leakage and free up headroom for the GPU.

    // Safety floor: if runnable tasks are high, never offline.
    // This catches cases where a high-priority workload (e.g. llama-server)
    // is starving for cores even when pressure avg10 hasn't spiked yet.
    if (running_tasks > 0 && total_groups > 0) {
        // Conservative: if runnable tasks >= 80% of total_groups, keep all online.
        // (On Meteor Lake, 16 groups = 22 threads; 17+ runnable = keep all.)
        int threshold = (total_groups * 80) / 100;  // 80%
        if (running_tasks >= threshold) return 0;
    }

    // If scheduler demand is high, keep all cores online (CPU is the bottleneck)
    if (cpu_demand >= 0.95) return 0;

    // High demand — allow dropping a few groups only if budget is tight
    if (cpu_demand > 0.75) {
        double ratio = (cpu_power_ref > 0) ? cpu_budget / cpu_power_ref : 1.0;
        if (ratio < 0.3) return std::max(total_groups - 2, min_core_groups);
        return 0;  // budget is fine — keep all online
    }

    // Medium demand — scale groups by demand level
    if (cpu_demand > 0.4) {
        // Scale: at 0.4 keep ~90%, at 0.75 keep ~60%. Keep P-cores first.
        int target = static_cast<int>(total_groups * (1.0 - cpu_demand * 0.33));
        return std::max(min_core_groups + 1, target);
    }

    // Low demand + active GPU — keep P-cores + a few E-cores
    // Core groups are ordered P-first (0..p-1), then E (p..total-1).
    // apply_hotplug keeps the FIRST `target` groups, so target controls
    // how many P+E groups stay online from the P-end inward.
    int pcore_count = std::max(1, total_groups / 3);  // rough P-core estimate
    int ecore_count = total_groups - pcore_count;

    if (cpu_demand > 0.15) {
        // Low demand: keep all P-cores + half E-cores
        return pcore_count + ecore_count / 2;
    }

    // Very low demand + active GPU — scale P-cores based on demand.
    // GPU needs the power, CPU is barely used. Keep only what's needed.
    // Minimum: 2 P-core groups (4 physical P-cores with HT) for safety.
    if (cpu_demand > 0.02) {
        return std::max(1, pcore_count);  // keep P-cores only
    }

    // Near-zero demand + active GPU — drop to CPU0's group only.
    // apply_hotplug enforces CPU0 always online (safety floor).
    return 1;
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
    int keep_groups = choose_keep_groups(
        cpu_budget, cpu_power_ref, inputs.total_core_groups,
        config.min_core_groups, inputs.cpu_demand, inputs.gpu_power_w,
        inputs.prev_keep_groups, inputs.running_tasks);

    // Don't clamp 0 (keep all) — that's the most conservative policy.
    if (keep_groups > 0) {
        keep_groups = std::max(keep_groups, config.min_core_groups);
    }
    if (keep_groups > inputs.total_core_groups) {
        keep_groups = 0;  // not enough groups to offline
    }

    // Smoothing: if previous keep_groups differs, prefer the previous value
    // unless the difference is large. This prevents rapid online/offline
    // transitions when the solver is near a decision boundary.
    if (inputs.prev_keep_groups > 0 && keep_groups > 0) {
        // Both positive — only change if the difference is > 1 group
        if (std::abs(keep_groups - inputs.prev_keep_groups) <= 1) {
            keep_groups = inputs.prev_keep_groups;
        }
    }

    result.keep_groups = keep_groups;

    // ── Step 12: Compute aggression level (for logging) ──
    result.aggression = compute_aggression(
        inputs.gpu_throttling, inputs.cpu_demand,
        cpu_budget, cpu_power_ref);

    return result;
}
