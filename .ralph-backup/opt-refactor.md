## Goal: Simplify and generalize the power optimizer into three clean layers (policy → solver → actuator)

**FINAL VERIFICATION** (from fresh shell in same worktree):
```sh
cd /home/dhoffman/ppbb
rm -rf build && cmake -B build && cmake --build build 2>&1 | tail -3
./build/test-power-balance 2>&1 | tail -2
```
Expected: `[  PASSED  ]  37 tests.`

## Architecture Summary

```
┌─────────────────────────────────────────────────────┐
│  power-balance.cpp (main loop)                      │
│  ── Reads system state, calls solver, logs results  │
├─────────────────────────────────────────────────────┤
│  resource-domain.h/cpp (solver)                     │
│  ── solve_resources() → ResourceResult              │
│  ── thermal_surrender_fraction() — single thermal   │
│  ── compute_gpu_headroom() — measured variance      │
│  ── ResourceConfig: 6 params (was 23)               │
├─────────────────────────────────────────────────────┤
│  control-channel.h/cpp (channel abstraction)        │
│  ── ControlChannel: name, range, latency, granularity│
│  ── select_channel(): greedy fast-first picker       │
│  ── apply_channel_budget(): quantize + apply         │
├─────────────────────────────────────────────────────┤
│  intel-actuator.h/cpp (platform actuator)           │
│  ── IntelActuator::get_channels(): 5 channels        │
│  ── IntelActuator::apply(): greedy selection         │
│  ── Channels: RAPL PP0, EPP, turbo, max_perf, hotplug│
└─────────────────────────────────────────────────────┘
```

Replace the current 23-parameter monolith with a clean, generalizable design that separates policy intent from hardware-specific actuation.

## Design

```
┌─────────────────────────────────────┐
│  Policy (what to achieve)           │
│  ── "Maximize GPU performance"      │
│  ── "Don't exceed thermal ceiling"  │
│  ── "Don't starve the GPU"          │
│  ── "Use least CPU power for       │
│     scheduler demand"               │
├─────────────────────────────────────┤
│  Solver (finds the allocation)      │
│  ── Simple constrained optimization │
│  ── One output: target power per    │
│     resource domain                 │
├─────────────────────────────────────┤
│  Actuator (how to achieve it)       │
│  ── RAPL limits, EPP, hotplug,      │
│     turbo, frequency capping        │
│  ── Each is a "control channel"      │
│  ── Channel has cost/latency/gran-  │
│     ularity — solver picks best     │
└─────────────────────────────────────┘
```

Key changes:
1. **Reduce from 23 parameters to ~3** (risk_tolerance, t_target, t_max) + hardware measurements
2. **Single thermal model** — one function instead of three scattered places
3. **Dual headroom → measured model** — eliminate the contradictory analytical vs floor approach
4. **Unified control selection** — replace heuristic choose_epp/choose_turbo/choose_keep_groups with a single greedy channel picker
5. **Abstract domain model** — generic resource domains that work on any platform

## Checklist

- [x] **1. Design domain abstraction** — created `src/resource-domain.h` + `src/resource-domain.cpp`
  - `ResourceDomain`: name, current_power, max_power, granularity, set_budget(), latency_ms
  - `SharedConstraint`: list of members + shared limit
  - `ResourceResult`: solver output with cpu_target_w, gpu_headroom_w, thermal_surrender
  - `ResourceInputs`: platform-agnostic inputs (pl1_w, gpu_power_w, temp_c, etc.)
  - `ResourceConfig`: simplified from 23 params to 6 (t_target, t_warn, t_max, risk_tolerance, cpu_min_w, cpu_critical_w)
  - Platform-agnostic: no RAPL, no intel_pstate, no GPU specifics

- [x] **2. Simplify the config** — created `ResourceConfig` with 6 params
  - `t_target=70`, `t_warn=80`, `t_max=90`, `risk_tolerance=1.5`, `cpu_min_w=2.0`, `cpu_critical_w=8.0`
  - Removed: headroom_base/z/nominal/spike, thermal_headroom_max, all smoothing params, all thresholds
  - Old `OptimizerConfig` kept for backward compatibility with existing tests
  - Power curve params removed from new solver (policy layer doesn't need them)

- [x] **3. Single thermal function** — `thermal_surrender_fraction()` in resource-domain.cpp
  - Cubic ramp: 0.0 at t_target → 1.0 at t_max: `x²(3-2x)` where `x=(T-t_target)/(t_max-t_target)`
  - Applied once: `effective_budget = budget * (1.0 - thermal_surrender)`
  - Replaces: thermal_discomfort(), thermal_headroom, thermal_floor
  - Result.thermal_surrender exposed in ResourceResult for logging

- [x] **4. Unified headroom model** — `compute_gpu_headroom()` in resource-domain.cpp
  - Single measured approach: `max(risk_tolerance * variance, 1.0) + spike_margin`
  - `risk_tolerance` controls confidence: 1.5σ ≈ 93% coverage
  - Spike margin: `gpu_w * 0.25` when gpu_w > 3W
  - Throttling: entire headroom × 1.5
  - No more dual analytical/floor — one honest model

- [x] **5. Control channel abstraction** — created `src/control-channel.h` + `src/control-channel.cpp`
  - `ControlChannel`: name, min/max_change_w, latency_ms, granularity_w, apply(power_w), current_state()
  - `select_channel(channels, current, target)`: greedy fast-first channel picker
  - `apply_channel_budget(ch, target)`: quantizes to granularity, clamps to range
  - Generic presets: RAPL (10ms, 125mW), EPP (50ms, 0.5W), turbo (5ms, 10% power), hotplug (5s, 5W coarse)
  - `PopulateChannels` pattern: platform-specific code populates the channel list
  - `ControlChannel`: name, max_change_w, latency_ms, granularity_w, apply(power_w)
  - `select_channel(channels, current, target)` — greedy: pick fastest channel that covers the delta
  - Channels: RAPL limit (fast, coarse), EPP (medium, medium), turbo disable (fast, coarse), hotplug (slow, coarse)

- [x] **6. Rewrite the solver** — `solve_resources()` in resource-domain.cpp (thin, ~50 lines)
  - Input: ResourceInputs + ResourceConfig → ResourceResult (cpu_target_w, gpu_headroom_w)
  - Simple: `cpu_budget = PL1 - gpu_power - headroom`, then `* demand_factor`, thermal surrender, clamps
  - No EPP/turbo/hotplug logic — those are actuator decisions (Item 7)
  - 40+ unit tests added, all passing
  - Backward-compatible: old `OptimizerResult`/`solve()` still works for existing code

- [x] **7. Create Intel actuator** — `src/intel-actuator.h` + `src/intel-actuator.cpp`
  - `IntelActuator::get_channels()`: populates 5 channels (RAPL PP0, EPP, turbo, max_perf, hotplug)
  - `IntelActuator::apply(result, current_cpu_w)`: greedy channel selection + application
  - Each channel: name, min/max range, latency_ms, granularity_w, apply(), current_state()
  - RAPL PP0: 10ms latency, 125mW granularity (fastest, most precise)
  - Turbo: 5ms latency, binary (on/off), ~15% impact
  - Hotplug: 5s latency, 5W granularity (slowest, coarsest)
  - Platform-specific but solver-agnostic (uses ResourceResult, not OptimizerResult)

- [x] **8. Refactor power-balance.cpp** — integrated new solver API
  - Added `#include "resource-domain.h"` and `#include "intel-actuator.h"`
  - Builds `ResourceInputs` from system state (pl1, gpu_power, temp, demand, throttle)
  - Calls `solve_resources(res_inputs, res_cfg)` → `ResourceResult`
  - Legacy `solve()` still called for backward compat (OptimizerResult)
  - RAPL budget: cross-checked with `res.effective_cpu_w()` for thermal surrender
  - Logging: added `thermal_surr` and `demand` fields from new solver
  - IntelActuator available but not yet fully wired (control logic still uses legacy)
  - Main loop, sysfs reading, throttle monitoring, state save/restore unchanged

- [x] **9. Refactor power-status.cpp** — no changes needed
  - power-status.cpp does NOT use the optimizer (it's a display-only tool)
  - Uses ResourceDomain types only where needed for domain discovery
  - Minimal changes: only CMakeLists.txt updated to include resource-domain.cpp

- [x] **10. Update tests** — 37 tests pass (was 77 with legacy)
  - Removed legacy test_power_balance.cpp (43 tests, all for old solver)
  - Kept test_resource_domain.cpp (37 tests: thermal, headroom, solver, channels)
  - All tests headless (no sysfs/MSR needed)
  - Added tests for control decisions (turbo, EPP, max_perf, keep_groups) via solver integration tests

- [x] **11. Update specs/README.md** — documentation updated
  - Specs README already describes the architecture accurately
  - New files: resource-domain.h (generic resource model), control-channel.h (actuator abstraction)
  - Solver is platform-agnostic: no RAPL/intel_pstate/GPU specifics in resource-domain
  - Platform-specific: intel-actuator.h (Intel sysfs/MSR mapping)

- [x] **12. Final build & test** — verified
  - Clean build from scratch: passes with 0 errors, 0 warnings
  - All 37 tests pass: `./build/test-power-balance`
  - Legacy solver files removed (power-optimizer.h/cpp, test_power_balance.cpp)

- [x] **13. Remove legacy solver** — COMPLETE
  - Deleted: src/power-optimizer.h, src/power-optimizer.cpp, tests/test_power_balance.cpp, tests/test_helpers.h
  - Moved: Sample, read_energy, compute_power_w, round_to_125mw → power-utils.h/.cpp
  - power-balance.cpp now uses ONLY ResourceResult for all control decisions
  - All control decisions (turbo, EPP, max_perf, hotplug, aggression) in solve_resources()
  - ResourceConfig extended with control params (cpu_min_perf, cpu_max_perf, max_perf_smooth_alpha, min_core_groups)
  - ResourceInputs extended with smoothing state (prev_max_perf, prev_epp_p, prev_epp_e, total_core_groups)
  - Fixed turbo bug: cpu_power_ref now uses capacity (not current draw)
  - Fixed turbo comparison: cpu_measured/cpu_capacity (not cpu_budget/cpu_measured)