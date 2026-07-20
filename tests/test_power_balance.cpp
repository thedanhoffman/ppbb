// test_power_balance.cpp — Unit tests for power-balance production code
//
// Tests ONLY functions actually called by the daemon (power-balance.cpp).
// All other functions from the old power_balance_pure.h were dead code:
// the optimizer uses solve() as its single entry point for all control logic.
//
// Compiled tests:
//   - ComputePowerW: power calculation from RAPL energy samples
//   - RoundTo125mw: RAPL power limit rounding
//   - Solve.*: full optimizer exercise with various input combinations
//   - SmoothMaxPerf: exponential smoothing of max_perf_pct

#include "test_helpers.h"
#include "power-optimizer.h"

#include <gtest/gtest.h>

// ═══════════════════════════════════════════════════════════
// compute_power_w tests
// ═══════════════════════════════════════════════════════════

TEST(ComputePowerW, NormalCase) {
    auto t1 = std::chrono::steady_clock::now();
    auto t2 = t1 + std::chrono::microseconds(500000);  // 500ms

    Sample prev{100000000, t1};   // 100 MJ
    Sample cur{105000000, t2};   // 105 MJ

    double w = compute_power_w(prev, cur);
    EXPECT_DOUBLE_EQ(w, 10.0);  // 5MJ / 0.5s = 10 W
}

TEST(ComputePowerW, ZeroPower) {
    auto t1 = std::chrono::steady_clock::now();
    auto t2 = t1 + std::chrono::milliseconds(100);

    Sample prev{5000000, t1};
    Sample cur{5000000, t2};

    EXPECT_DOUBLE_EQ(compute_power_w(prev, cur), 0.0);
}

TEST(ComputePowerW, NegativeDeltaReturnsMinusOne) {
    auto t1 = std::chrono::steady_clock::now();
    auto t2 = t1 + std::chrono::milliseconds(100);

    Sample prev{5000000, t2};  // later time has lower energy (counter reset?)
    Sample cur{4000000, t1};

    EXPECT_DOUBLE_EQ(compute_power_w(prev, cur), -1.0);
}

TEST(ComputePowerW, InvalidPreviousReturnsZero) {
    auto t = std::chrono::steady_clock::now();

    Sample prev{-1, t};
    Sample cur{5000000, t};

    EXPECT_DOUBLE_EQ(compute_power_w(prev, cur), 0.0);
}

TEST(ComputePowerW, InvalidCurrentReturnsZero) {
    auto t = std::chrono::steady_clock::now();

    Sample prev{5000000, t};
    Sample cur{-1, t};

    EXPECT_DOUBLE_EQ(compute_power_w(prev, cur), 0.0);
}

TEST(ComputePowerW, SubSecondCalculation) {
    auto t1 = std::chrono::steady_clock::now();
    auto t2 = t1 + std::chrono::microseconds(100000);  // 100ms

    Sample prev{10000000, t1};   // 10 MJ
    Sample cur{10125000, t2};   // 10.125 MJ

    double w = compute_power_w(prev, cur);
    EXPECT_DOUBLE_EQ(w, 1.25);  // 0.125 MJ / 0.1 s = 1.25 W
}

// ═══════════════════════════════════════════════════════════
// round_to_125mw tests
// ═══════════════════════════════════════════════════════════

TEST(RoundTo125mw, ZeroStaysZero) {
    EXPECT_DOUBLE_EQ(round_to_125mw(0.0), 0.0);
}

TEST(RoundTo125mw, NegativeBecomesZero) {
    EXPECT_DOUBLE_EQ(round_to_125mw(-1.0), 0.0);
    EXPECT_DOUBLE_EQ(round_to_125mw(-0.5), 0.0);
    EXPECT_DOUBLE_EQ(round_to_125mw(-0.125), 0.0);
}

TEST(RoundTo125mw, AlreadyOnBoundaryStays) {
    EXPECT_DOUBLE_EQ(round_to_125mw(1.0), 1.0);        // 8 * 0.125
    EXPECT_DOUBLE_EQ(round_to_125mw(0.125), 0.125);
    EXPECT_DOUBLE_EQ(round_to_125mw(0.25), 0.25);
    EXPECT_DOUBLE_EQ(round_to_125mw(125.0), 125.0);
}

TEST(RoundTo125mw, RoundsUp) {
    // 1.06 → nearest is 1.00 (8.48 → 8.0)
    EXPECT_DOUBLE_EQ(round_to_125mw(1.06), 1.0);
    // 1.07 → nearest is 1.125 (8.56 → 9.0)
    EXPECT_DOUBLE_EQ(round_to_125mw(1.07), 1.125);
    // 1.18 → nearest is 1.125 (9.44 → 9.0)
    EXPECT_DOUBLE_EQ(round_to_125mw(1.18), 1.125);
}

TEST(RoundTo125mw, RoundsDown) {
    // 1.24 → rounds to 1.25 (9.92 → 10.0)
    EXPECT_DOUBLE_EQ(round_to_125mw(1.24), 1.25);
    // 1.19 → rounds to 1.25 (9.52 → 10.0)
    EXPECT_DOUBLE_EQ(round_to_125mw(1.19), 1.25);
    // 1.13 → rounds to 1.125 (9.04 → 9.0)
    EXPECT_DOUBLE_EQ(round_to_125mw(1.13), 1.125);
}

TEST(RoundTo125mw, MidpointBehavior) {
    // 1.0625 is exactly between 1.0 and 1.125 → rounds to 1.125 (std::round)
    EXPECT_DOUBLE_EQ(round_to_125mw(1.0625), 1.125);
}

// ═══════════════════════════════════════════════════════════
// solve() tests — Full optimizer exercise
// ═══════════════════════════════════════════════════════════

TEST(Solve, IdleState) {
    // All inputs indicate idle — should preserve CPU performance
    OptimizerInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_w = 0.0;
    inputs.temp_c = 50.0;
    inputs.gpu_c0_pct = 0.0;
    inputs.total_core_groups = 8;
    inputs.prev_max_perf = 100;

    auto result = solve(inputs);

    // Core budget should be generous
    EXPECT_LT(result.core_power_w, 40.0);
    EXPECT_GT(result.core_power_w, 35.0);
    // max_perf should be 100 (no smoothing needed)
    EXPECT_EQ(result.max_perf_pct, 100);
    // no_turbo should be 0
    EXPECT_EQ(result.no_turbo, 0);
    // EPP should be balance_performance
    EXPECT_EQ(result.epp_p, EppLevel::BalancePerformance);
    EXPECT_EQ(result.epp_e, EppLevel::BalancePerformance);
    // MIN_CORE_GROUPS constraint means keep_groups >= 2
    EXPECT_GE(result.keep_groups, 2);
    // Weights should favor performance
    EXPECT_GT(result.weight_performance, result.weight_throttle);
}

TEST(Solve, GpuThrottlingState) {
    // GPU throttling — should prioritize GPU headroom
    OptimizerInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_w = 15.0;
    inputs.gpu_throttling = true;
    inputs.temp_c = 60.0;
    inputs.total_core_groups = 8;
    inputs.prev_max_perf = 20;

    auto result = solve(inputs);

    // Core budget should be capped
    EXPECT_LE(result.core_power_w, CRITICAL_CPU_MAX_W);
    // max_perf should be 20 (no smoothing needed since prev was also 20)
    EXPECT_EQ(result.max_perf_pct, 20);
    // no_turbo should be 1
    EXPECT_EQ(result.no_turbo, 1);
    // EPP should be power for P, balance_power for E
    EXPECT_EQ(result.epp_p, EppLevel::Power);
    EXPECT_EQ(result.epp_e, EppLevel::BalancePower);
    // Throttle weight should be high
    EXPECT_GT(result.weight_throttle, 5.0);
}

TEST(Solve, HighThermalState) {
    // High temperature — thermal constraints should dominate
    OptimizerInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_w = 5.0;
    inputs.temp_c = 88.0;
    inputs.total_core_groups = 8;
    inputs.prev_max_perf = 30;

    auto result = solve(inputs);

    // Thermal headroom should be significant
    EXPECT_GT(result.thermal_extra_w, 3.0);
    // max_perf raw should be low (thermal override at 88°C: pressure=0.9, max_perf=28)
    // smoothed with prev=30: 0.5*28 + 0.5*30 = 29
    EXPECT_LT(result.max_perf_pct, 40);
    // no_turbo should be 1 (thermal override at 88°C > 82°C)
    EXPECT_EQ(result.no_turbo, 1);
    // EPP should be power (thermal tier 2 at 88°C)
    EXPECT_EQ(result.epp_p, EppLevel::Power);
}

TEST(Solve, HeavyGpuLoad) {
    // Heavy GPU load (70% C0) — should prioritize GPU
    OptimizerInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_w = 12.0;
    inputs.gpu_c0_pct = 0.75;
    inputs.temp_c = 60.0;
    inputs.total_core_groups = 8;

    auto result = solve(inputs);

    // C0 > 70% → GPU priority
    EXPECT_GT(result.weight_thermal, 2.0);
    EXPECT_GT(result.weight_throttle, 4.0);
    EXPECT_LT(result.weight_performance, 3.0);
    // max_perf should be moderate
    EXPECT_LE(result.max_perf_pct, 75);
}

TEST(Solve, GpuPowerExceedsPl1) {
    // GPU consuming more than PL1 — core budget should be zero
    OptimizerInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_w = 45.0;
    inputs.temp_c = 50.0;
    inputs.gpu_c0_pct = 0.9;
    inputs.total_core_groups = 8;

    auto result = solve(inputs);

    // Core budget must be zero (can't be negative)
    EXPECT_EQ(result.core_power_w, 0.0);
    EXPECT_EQ(result.core_limit_r, 0.0);
    // Should still set aggressive weights due to high GPU power
    EXPECT_LT(result.weight_performance, 5.0);
}

TEST(Solve, LowPl1ThermalHeadroomExceedsBudget) {
    // Very low PL1 (10W) with high temp — thermal headroom may exceed budget
    OptimizerInputs inputs{};
    inputs.pl1_w = 10.0;
    inputs.gpu_w = 5.0;
    inputs.temp_c = 88.0;
    inputs.gpu_c0_pct = 0.1;
    inputs.total_core_groups = 8;

    auto result = solve(inputs);

    // Core budget should be clamped to 0 (10 - 5 - 2 - 3.75 = -0.75 → 0)
    EXPECT_EQ(result.core_power_w, 0.0);
    EXPECT_EQ(result.core_limit_r, 0.0);
    // Thermal pressure should be high
    EXPECT_GT(result.weight_thermal, 5.0);
}

TEST(Solve, NoGpuDetected) {
    // No GPU — should behave as GPU idle
    OptimizerInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_w = 0.0;
    inputs.have_gpu = false;
    inputs.temp_c = 60.0;
    inputs.gpu_c0_pct = 0.0;
    inputs.total_core_groups = 4;

    auto result = solve(inputs);

    // Should be idle behavior
    EXPECT_EQ(result.max_perf_pct, 100);
    EXPECT_EQ(result.no_turbo, 0);
    EXPECT_EQ(result.epp_p, EppLevel::BalancePerformance);
}

TEST(Solve, NoCoretempNoTemperature) {
    // No temperature sensors — should not apply thermal constraints
    OptimizerInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_w = 10.0;
    inputs.temp_c = -1.0;  // unknown temperature
    inputs.gpu_c0_pct = 0.0;
    inputs.gpu_throttling = false;
    inputs.have_coretemp = false;
    inputs.total_core_groups = 8;

    auto result = solve(inputs);

    // Thermal headroom should be zero (no temperature data)
    EXPECT_DOUBLE_EQ(result.thermal_extra_w, 0.0);
    // no_turbo should be 0 (no thermal reason)
    EXPECT_EQ(result.no_turbo, 0);
    // max_perf should be based on GPU power only
    EXPECT_GE(result.max_perf_pct, 50);
}

TEST(Solve, BoundaryTemperature70C) {
    // At exactly 70°C — thermal pressure is 0, no constraints
    OptimizerInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_w = 0.0;
    inputs.temp_c = 70.0;
    inputs.gpu_c0_pct = 0.0;
    inputs.total_core_groups = 8;

    auto result = solve(inputs);

    // Thermal pressure at 70°C is 0
    EXPECT_DOUBLE_EQ(compute_thermal_pressure(70.0), 0.0);
    EXPECT_EQ(result.no_turbo, 0);
    EXPECT_DOUBLE_EQ(result.thermal_extra_w, 0.0);
}

TEST(Solve, BoundaryTemperature82C) {
    // At exactly 82°C — no_turbo should trigger
    OptimizerInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_w = 0.0;
    inputs.temp_c = 82.0;
    inputs.gpu_c0_pct = 0.0;
    inputs.total_core_groups = 8;

    auto result = solve(inputs);

    // no_turbo should be 1 at 82°C
    EXPECT_EQ(result.no_turbo, 1);
    // max_perf should be reduced (pressure = (82-70)/20 = 0.6, max_perf = 100-48 = 52)
    EXPECT_LT(result.max_perf_pct, 80);
}

TEST(Solve, BoundaryTemperature85C) {
    // At exactly 85°C — epp_tier 2, power mode
    OptimizerInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_w = 0.0;
    inputs.temp_c = 85.0;
    inputs.gpu_c0_pct = 0.0;
    inputs.total_core_groups = 8;

    auto result = solve(inputs);

    // EPP should be power at 85°C
    EXPECT_EQ(result.epp_p, EppLevel::Power);
    // Thermal pressure at 85°C = 0.75, max_perf = 100 - 60 = 40
    auto pressure = compute_thermal_pressure(85.0);
    EXPECT_DOUBLE_EQ(pressure, 0.75);
}

TEST(Solve, BoundaryTemperature90C) {
    // At exactly 90°C — thermal pressure is 1.0, max_perf capped at 20
    OptimizerInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_w = 0.0;
    inputs.temp_c = 90.0;
    inputs.gpu_c0_pct = 0.0;
    inputs.total_core_groups = 8;
    inputs.prev_max_perf = 20;  // match expected to avoid smoothing

    auto result = solve(inputs);

    // Thermal pressure at 90°C is 1.0
    EXPECT_DOUBLE_EQ(compute_thermal_pressure(90.0), 1.0);
    // max_perf should be 20 (capped)
    EXPECT_EQ(result.max_perf_pct, 20);
    EXPECT_EQ(result.no_turbo, 1);
    EXPECT_EQ(result.epp_p, EppLevel::Power);
    EXPECT_EQ(result.epp_e, EppLevel::Power);
}

TEST(Solve, GpuThrottlingPlusHighTemp) {
    // Worst case: GPU throttling + 92°C — all constraints active
    OptimizerInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_w = 20.0;
    inputs.gpu_throttling = true;
    inputs.temp_c = 92.0;
    inputs.total_core_groups = 8;
    inputs.prev_max_perf = 20;  // match expected to avoid smoothing

    auto result = solve(inputs);

    // Core budget capped at CRITICAL_CPU_MAX_W
    EXPECT_LE(result.core_power_w, CRITICAL_CPU_MAX_W);
    // max_perf at 20%
    EXPECT_EQ(result.max_perf_pct, 20);
    // no_turbo on
    EXPECT_EQ(result.no_turbo, 1);
    // EPP both power
    EXPECT_EQ(result.epp_p, EppLevel::Power);
    EXPECT_EQ(result.epp_e, EppLevel::Power);
    // Throttle weight maxed
    EXPECT_GT(result.weight_throttle, 9.0);
}

TEST(Solve, EdgeTopologyOneGroup) {
    // System with only 1 core group — keep_groups should be capped
    OptimizerInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_w = 0.0;
    inputs.temp_c = 50.0;
    inputs.gpu_c0_pct = 0.0;
    inputs.total_core_groups = 1;

    auto result = solve(inputs);

    // MIN_CORE_GROUPS is 2, but total is only 1, so keep_groups should be 0 (all online)
    // The code handles this: keep_groups = min(2, 1) won't cap, but the cap logic
    // says: if keep_groups > total_core_groups, set to 0
    EXPECT_LE(result.keep_groups, 1);
}

TEST(Solve, ContradictoryInputsZeroGpuPowerHighC0) {
    // GPU power = 0 but C0 residency = 1.0 — shouldn't crash
    OptimizerInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_w = 0.0;
    inputs.gpu_c0_pct = 1.0;
    inputs.temp_c = 50.0;
    inputs.total_core_groups = 8;

    auto result = solve(inputs);

    // Should not crash, should produce reasonable outputs
    EXPECT_GT(result.core_power_w, 0.0);
    EXPECT_GE(result.max_perf_pct, 0);
    EXPECT_LE(result.max_perf_pct, 100);
}

TEST(Solve, LowPl1NormalConditions) {
    // Low PL1 (15W) — should still function correctly
    OptimizerInputs inputs{};
    inputs.pl1_w = 15.0;
    inputs.gpu_w = 5.0;
    inputs.temp_c = 55.0;
    inputs.gpu_c0_pct = 0.0;
    inputs.total_core_groups = 8;

    auto result = solve(inputs);

    // Core budget = 15 - 5 - 2 = 8W (with some rounding)
    EXPECT_GT(result.core_power_w, 5.0);
    EXPECT_LT(result.core_power_w, 10.0);
    EXPECT_EQ(result.max_perf_pct, 100);
}

// ═══════════════════════════════════════════════════════════
// Weight schedule tests
// ═══════════════════════════════════════════════════════════

TEST(Weights, IdleState) {
    // Idle: GPU idle, cool → performance-oriented weights
    OptimizerInputs inputs{};
    inputs.temp_c = 50.0;
    inputs.gpu_c0_pct = 0.0;
    inputs.gpu_throttling = false;

    double wt, wthr, wp;
    compute_weights(inputs, wt, wthr, wp);

    EXPECT_DOUBLE_EQ(wt, 1.0);
    EXPECT_DOUBLE_EQ(wthr, 0.0);
    EXPECT_DOUBLE_EQ(wp, 10.0);
}

TEST(Weights, GpuActiveC0Between30and70) {
    // GPU active (C0 30-70%) → balanced weights
    OptimizerInputs inputs{};
    inputs.temp_c = 55.0;
    inputs.gpu_c0_pct = 0.5;
    inputs.gpu_throttling = false;

    double wt, wthr, wp;
    compute_weights(inputs, wt, wthr, wp);

    EXPECT_GE(wt, 2.0);
    EXPECT_GE(wthr, 3.0);
    EXPECT_LE(wp, 5.0);
}

TEST(Weights, GpuHeavyC0Above70) {
    // GPU heavy (C0 > 70%) → GPU headroom priority
    OptimizerInputs inputs{};
    inputs.temp_c = 55.0;
    inputs.gpu_c0_pct = 0.8;
    inputs.gpu_throttling = false;

    double wt, wthr, wp;
    compute_weights(inputs, wt, wthr, wp);

    EXPECT_GE(wt, 3.0);
    EXPECT_GE(wthr, 5.0);
    EXPECT_LE(wp, 2.0);
}

TEST(Weights, GpuThrottling) {
    // GPU throttling → CPU sacrifice priority
    OptimizerInputs inputs{};
    inputs.temp_c = 55.0;
    inputs.gpu_c0_pct = 0.5;
    inputs.gpu_throttling = true;

    double wt, wthr, wp;
    compute_weights(inputs, wt, wthr, wp);

    EXPECT_GE(wt, 5.0);
    EXPECT_GE(wthr, 10.0);
    EXPECT_DOUBLE_EQ(wp, 0.5);
}

TEST(Weights, HighTemperature) {
    // High temp (88°C) → thermal safety priority
    OptimizerInputs inputs{};
    inputs.temp_c = 88.0;
    inputs.gpu_c0_pct = 0.0;
    inputs.gpu_throttling = false;

    double wt, wthr, wp;
    compute_weights(inputs, wt, wthr, wp);

    // At 88°C: pressure = (88-70)/20 = 0.9, w_thermal = 1.0 + 0.9*9 = 9.1
    EXPECT_GT(wt, 8.0);
    // w_throttle should still be 0 (no GPU throttle), but thermal pressure elevates
    // Actually at 88°C, w_thermal goes up but w_throttle stays 0.0 unless GPU throttling
    // But the weight clamping ensures w_perf stays high when w_throttle is 0
    EXPECT_GE(wp, 5.0);
}

TEST(Weights, GpuPowerFallback) {
    // GPU power > 15W but C0 low → fallback should activate throttle weight
    // This tests the gpu_w fallback path in compute_weights()
    OptimizerInputs inputs{};
    inputs.temp_c = 55.0;
    inputs.gpu_c0_pct = 0.0;  // C0 says idle
    inputs.gpu_throttling = false;
    inputs.gpu_w = 20.0;  // But GPU power is high!

    double wt, wthr, wp;
    compute_weights(inputs, wt, wthr, wp);

    // With gpu_w > GPU_HEAVY_W and C0 low, fallback should set at least active level
    EXPECT_GE(wthr, 3.0);  // At minimum "active" level
    EXPECT_LE(wp, 5.0);     // At minimum performance reduction
}

TEST(Weights, GpuThrottlingPlusHighTemp) {
    // GPU throttling + high temp → both thermal and throttle maxed
    OptimizerInputs inputs{};
    inputs.temp_c = 90.0;
    inputs.gpu_c0_pct = 0.0;
    inputs.gpu_throttling = true;

    double wt, wthr, wp;
    compute_weights(inputs, wt, wthr, wp);

    // Thermal at 90°C: pressure = 1.0, w_thermal = 10.0
    // GPU throttling: w_thermal >= 5.0 (already met), w_throttle = 10.0
    EXPECT_DOUBLE_EQ(wt, 10.0);
    EXPECT_DOUBLE_EQ(wthr, 10.0);
    EXPECT_DOUBLE_EQ(wp, 0.5);
}

TEST(Weights, AllWeightsClampedPositive) {
    // Weights should never go negative or zero
    OptimizerInputs inputs{};
    inputs.temp_c = 50.0;
    inputs.gpu_c0_pct = 0.0;
    inputs.gpu_throttling = false;

    double wt, wthr, wp;
    compute_weights(inputs, wt, wthr, wp);

    EXPECT_GT(wt, 0.0);
    EXPECT_GE(wthr, 0.0);
    EXPECT_GT(wp, 0.0);
}

// ═══════════════════════════════════════════════════════════
// SmoothMaxPerf tests
// ═══════════════════════════════════════════════════════════

TEST(SmoothMaxPerf, BasicSmoothing) {
    // Test that smoothing reduces oscillation
    OptimizerResult result{};
    result.raw_max_perf_pct = 100;

    // First cycle: raw 100, prev 100 → smoothed 100
    EXPECT_EQ(result.smooth_max_perf(100), 100);

    // Second cycle: raw 20, prev 100 → smoothed = 0.5*20 + 0.5*100 = 60
    result.raw_max_perf_pct = 20;
    EXPECT_EQ(result.smooth_max_perf(100), 60);

    // Third cycle: raw 100, prev 60 (from above) → smoothed = 0.5*100 + 0.5*60 = 80
    result.raw_max_perf_pct = 100;
    EXPECT_EQ(result.smooth_max_perf(60), 80);
}

TEST(SmoothMaxPerf, Convergence) {
    // Repeated smoothing should converge to raw value
    OptimizerResult result{};
    result.raw_max_perf_pct = 50;

    int prev = 100;
    for (int i = 0; i < 20; i++) {
        prev = result.smooth_max_perf(prev);
    }
    // After 20 iterations, should be very close to 50
    EXPECT_LT(std::abs((double)prev - 50.0), 1.0);
}

TEST(SmoothMaxPerf, OscillationSuppression) {
    // Rapid alternation should be smoothed out
    OptimizerResult result{};

    // Simulate rapid alternation: raw=100, raw=20, raw=100, raw=20...
    int prev = 100;
    for (int i = 0; i < 8; i++) {
        result.raw_max_perf_pct = (i % 2 == 0) ? 100 : 20;
        prev = result.smooth_max_perf(prev);
    }
    // After 8 alternations, should be closer to middle than extremes
    EXPECT_GT(prev, 20);
    EXPECT_LT(prev, 100);
}

TEST(SmoothMaxPerf, StaysAtSameValue) {
    // If raw value stays the same, smoothed should too
    OptimizerResult result{};
    result.raw_max_perf_pct = 75;

    int prev = 75;
    for (int i = 0; i < 5; i++) {
        prev = result.smooth_max_perf(prev);
    }
    EXPECT_EQ(prev, 75);
}
