// test_resource_domain.cpp — Unit tests for resource-domain (new generic API)
//
// Tests:
//   - ThermalSurrenderFraction: disabled (always 0.0)
//   - ComputeGpuHeadroom: measured variance model
//   - SolveResources: policy layer with various input combinations
//   - SelectChannel: greedy control channel selection
//   - ApplyChannelBudget: quantization and clamping

#include "resource-domain.h"
#include "control-channel.h"

#include <gtest/gtest.h>

// ═══════════════════════════════════════════════════════════
// Thermal surrender fraction tests (disabled — always 0.0)
// ═══════════════════════════════════════════════════════════

TEST(ThermalSurrenderFraction, AlwaysZero) {
    ResourceConfig cfg{70.0, 80.0, 90.0};

    // Thermal surrender is disabled. Temperature is ambient on laptops,
    // not a CPU power signal. The function is kept for ABI compatibility.
    EXPECT_DOUBLE_EQ(thermal_surrender_fraction(cfg, 50.0), 0.0);
    EXPECT_DOUBLE_EQ(thermal_surrender_fraction(cfg, 60.0), 0.0);
    EXPECT_DOUBLE_EQ(thermal_surrender_fraction(cfg, 70.0), 0.0);
    EXPECT_DOUBLE_EQ(thermal_surrender_fraction(cfg, 80.0), 0.0);
    EXPECT_DOUBLE_EQ(thermal_surrender_fraction(cfg, 90.0), 0.0);
    EXPECT_DOUBLE_EQ(thermal_surrender_fraction(cfg, 95.0), 0.0);
    EXPECT_DOUBLE_EQ(thermal_surrender_fraction(cfg, -1.0), 0.0);
}

// ═══════════════════════════════════════════════════════════
// GPU headroom tests
// ═══════════════════════════════════════════════════════════

TEST(ComputeGpuHeadroom, ZeroVarianceHasMinimum) {
    ResourceConfig cfg;
    cfg.risk_tolerance = 1.5;

    // variance=0 → max(0, 1.0)=1.0, spike(10*0.25=2.5) → total=3.5
    double h = compute_gpu_headroom(cfg, 10.0, 0.0, false);
    EXPECT_DOUBLE_EQ(h, 3.5);
}

TEST(ComputeGpuHeadroom, NormalVariance) {
    ResourceConfig cfg;
    cfg.risk_tolerance = 1.5;

    // variance=2.0 → max(3.0, 1.0)=3.0, spike(10*0.25=2.5) → total=5.5
    double h = compute_gpu_headroom(cfg, 10.0, 2.0, false);
    EXPECT_DOUBLE_EQ(h, 5.5);
}

TEST(ComputeGpuHeadroom, ThrottlingMultipliesHeadroom) {
    ResourceConfig cfg;
    cfg.risk_tolerance = 1.5;

    // variance=2.0 → 3.0, throttling→4.5, spike(10*0.25=2.5)→7.0
    double h = compute_gpu_headroom(cfg, 10.0, 2.0, true);
    EXPECT_DOUBLE_EQ(h, 7.0);
}

TEST(ComputeGpuHeadroom, SpikeMarginOnHighGpuPower) {
    ResourceConfig cfg;
    cfg.risk_tolerance = 1.5;

    // variance=2.0 → 3.0, gpu_w=10.0 > 3.0 → +2.5 spike = 5.5
    double h = compute_gpu_headroom(cfg, 10.0, 2.0, false);
    EXPECT_DOUBLE_EQ(h, 5.5);

    // gpu_w=2.0 < 3.0 → no spike
    h = compute_gpu_headroom(cfg, 2.0, 2.0, false);
    EXPECT_DOUBLE_EQ(h, 3.0);  // just variance
}

TEST(ComputeGpuHeadroom, ThrottlingPlusSpike) {
    ResourceConfig cfg;
    cfg.risk_tolerance = 1.5;

    // variance=2.0 → 3.0, throttling→4.5, spike(10*0.25=2.5)→7.0
    // (throttling applies BEFORE spike margin)
    double h = compute_gpu_headroom(cfg, 10.0, 2.0, true);
    EXPECT_DOUBLE_EQ(h, 7.0);
}

// ═══════════════════════════════════════════════════════════
// solve_resources tests — Policy layer
// ═══════════════════════════════════════════════════════════

TEST(SolveResources, IdleNoGpu) {
    ResourceConfig cfg;
    ResourceInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_power_w = 0.0;
    inputs.have_gpu = false;
    inputs.temp_c = 50.0;
    inputs.cpu_demand = 0.2;  // idle
    inputs.gpu_c0_pct = 0.0;

    auto result = solve_resources(inputs, cfg);

    // No GPU → headroom=0, demand_factor = 0.5 + 0.5*0.2 = 0.6
    // cpu_budget = 40 * 0.6 = 24.0, clamped to [2, 100]
    EXPECT_NEAR(result.cpu_target_w, 24.0, 1.0);
    EXPECT_DOUBLE_EQ(result.gpu_headroom_w, 0.0);
    EXPECT_DOUBLE_EQ(result.thermal_surrender, 0.0);
}

TEST(SolveResources, GpuActiveWithHeadroom) {
    ResourceConfig cfg;
    ResourceInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_power_w = 12.0;
    inputs.have_gpu = true;
    inputs.temp_c = 60.0;
    inputs.cpu_demand = 0.8;  // active
    inputs.gpu_c0_pct = 0.6;
    inputs.gpu_power_var_w = 2.0;  // measured variance

    auto result = solve_resources(inputs, cfg);

    // headroom: max(1.5*2.0, 1.0) = 3.0, + spike(12*0.25=3.0) = 6.0
    // cpu_budget = 40 - 12 - 6.0 = 22.0
    // demand_factor = 0.5 + 0.5*0.8 = 0.9
    // effective = 22.0 * 0.9 = 19.8
    // thermal surrender disabled (always 0.0)
    EXPECT_LT(result.cpu_target_w, 40.0 - 12.0);
    EXPECT_GT(result.cpu_target_w, 15.0);
    EXPECT_DOUBLE_EQ(result.thermal_surrender, 0.0);
}

TEST(SolveResources, GpuThrottlingCapsCpu) {
    ResourceConfig cfg;
    cfg.cpu_critical_w = 8.0;
    ResourceInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_power_w = 15.0;
    inputs.have_gpu = true;
    inputs.temp_c = 50.0;
    inputs.cpu_demand = 1.0;
    inputs.gpu_throttling = true;
    inputs.gpu_power_var_w = 3.0;

    auto result = solve_resources(inputs, cfg);

    // headroom: max(1.5*3.0, 1.0) = 4.5, spike(15*0.25=3.75) = 8.25
    // throttling → 8.25 * 1.5 = 12.375
    // cpu_budget = 40 - 15 - 12.375 = 12.625
    // throttling cap: min(12.625, 8.0) = 8.0
    EXPECT_LE(result.cpu_target_w, cfg.cpu_critical_w);
    EXPECT_LE(result.cpu_target_w, 12.0);
}

TEST(SolveResources, DemandScaling) {
    ResourceConfig cfg;
    ResourceInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_power_w = 0.0;
    inputs.have_gpu = true;
    inputs.temp_c = 50.0;
    inputs.gpu_c0_pct = 0.0;
    inputs.gpu_power_var_w = 0.0;

    // With demand=0.0 → factor=0.5, budget = 29 * 0.5 ≈ 14.5
    inputs.cpu_demand = 0.0;
    auto r1 = solve_resources(inputs, cfg);

    // With demand=1.0 → factor=1.0, budget = 29 * 1.0 ≈ 29.0
    inputs.cpu_demand = 1.0;
    auto r2 = solve_resources(inputs, cfg);

    EXPECT_LT(r1.cpu_target_w, r2.cpu_target_w);
    EXPECT_DOUBLE_EQ(r1.demand_factor, 0.5);
    EXPECT_DOUBLE_EQ(r2.demand_factor, 1.0);
}

TEST(SolveResources, NoGpuFullBudget) {
    ResourceConfig cfg;
    ResourceInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_power_w = 0.0;
    inputs.have_gpu = false;  // NO GPU
    inputs.temp_c = 50.0;
    inputs.cpu_demand = 1.0;
    inputs.gpu_c0_pct = 0.0;
    inputs.gpu_power_var_w = 0.0;

    auto result = solve_resources(inputs, cfg);

    // No GPU → headroom=0, demand=1.0
    // cpu_budget = 40.0 * 1.0 = 40.0
    EXPECT_NEAR(result.cpu_target_w, 40.0, 0.5);
    EXPECT_DOUBLE_EQ(result.gpu_headroom_w, 0.0);
}

TEST(SolveResources, DomainMaxLimit) {
    ResourceConfig cfg;
    ResourceInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_power_w = 0.0;
    inputs.have_gpu = true;
    inputs.temp_c = 50.0;
    inputs.cpu_demand = 1.0;
    inputs.gpu_c0_pct = 0.0;
    inputs.gpu_power_var_w = 0.0;
    inputs.cpu_domain_max_w = 15.0;  // RAPL domain limit

    auto result = solve_resources(inputs, cfg);

    // cpu_budget = 40 - 0 - headroom(1.0) = 39.0
    // clamped to domain_max=15.0
    EXPECT_LT(result.cpu_target_w, 20.0);
    EXPECT_LT(result.cpu_target_w, inputs.cpu_domain_max_w + 1.0);
}

// ── GPU-idle hotplug: when GPU is not competing, keep all cores online ──

TEST(SolveResources, GpuIdleKeepsAllCoresOnline) {
    ResourceConfig cfg;
    ResourceInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_power_w = 1.0;   // GPU effectively idle (< 3W threshold)
    inputs.have_gpu = true;
    inputs.temp_c = 50.0;
    inputs.cpu_demand = 0.05;   // CPU also idle
    inputs.gpu_c0_pct = 0.0;
    inputs.gpu_power_var_w = 0.0;
    inputs.total_core_groups = 16;

    auto result = solve_resources(inputs, cfg);

    // GPU is idle → keep_groups = 0 (keep all online)
    // Low CPU demand doesn't matter when GPU isn't competing.
    EXPECT_EQ(result.keep_groups, 0);
}

TEST(SolveResources, GpuActiveOfflinesCores) {
    ResourceConfig cfg;
    cfg.min_core_groups = 1;
    ResourceInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_power_w = 15.0;   // GPU active (> 3W threshold)
    inputs.have_gpu = true;
    inputs.temp_c = 50.0;
    inputs.cpu_demand = 0.05;   // CPU idle
    inputs.gpu_c0_pct = 0.0;
    inputs.gpu_power_var_w = 2.0;
    inputs.total_core_groups = 16;

    auto result = solve_resources(inputs, cfg);

    // GPU active + CPU idle → keep P-cores only, offline E-cores.
    // P-core count estimated as total_groups/3 ≈ 5 (actual=6 on Meteor Lake).
    EXPECT_GT(result.keep_groups, 0);
    EXPECT_LT(result.keep_groups, inputs.total_core_groups);  // some groups offlined
    EXPECT_GT(result.keep_groups, cfg.min_core_groups);       // more than min (P-cores kept)
}

TEST(SolveResources, GpuActiveHighDemandKeepsAll) {
    ResourceConfig cfg;
    ResourceInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_power_w = 20.0;   // GPU active
    inputs.have_gpu = true;
    inputs.temp_c = 50.0;
    inputs.cpu_demand = 0.98;   // CPU saturated
    inputs.gpu_c0_pct = 0.0;
    inputs.gpu_power_var_w = 3.0;
    inputs.total_core_groups = 16;

    auto result = solve_resources(inputs, cfg);

    // High CPU demand → keep all online regardless of GPU
    EXPECT_EQ(result.keep_groups, 0);
}

// ── Hotplug hysteresis: dead band prevents ping-pong at GPU idle boundary ──

TEST(SolveResources, GpuHysteresisStaysOnlineInDeadBand) {
    ResourceConfig cfg;
    cfg.min_core_groups = 1;
    ResourceInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_power_w = 3.5;   // GPU in dead band [2.0, 5.0)
    inputs.have_gpu = true;
    inputs.temp_c = 50.0;
    inputs.cpu_demand = 0.05;   // CPU idle
    inputs.gpu_c0_pct = 0.0;
    inputs.gpu_power_var_w = 0.0;
    inputs.total_core_groups = 16;
    inputs.prev_keep_groups = 0; // currently all online

    auto result = solve_resources(inputs, cfg);

    // Dead band + currently online → stay online (hysteresis)
    EXPECT_EQ(result.keep_groups, 0);
}

TEST(SolveResources, GpuHysteresisStaysOfflineInDeadBand) {
    ResourceConfig cfg;
    cfg.min_core_groups = 1;
    ResourceInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_power_w = 3.5;   // GPU in dead band [2.0, 5.0)
    inputs.have_gpu = true;
    inputs.temp_c = 50.0;
    inputs.cpu_demand = 0.05;   // CPU idle
    inputs.gpu_c0_pct = 0.0;
    inputs.gpu_power_var_w = 2.0;
    inputs.total_core_groups = 16;
    inputs.prev_keep_groups = 1; // currently offline

    auto result = solve_resources(inputs, cfg);

    // Dead band + currently offline → stay offline (hysteresis)
    EXPECT_GT(result.keep_groups, 0);
    EXPECT_EQ(result.keep_groups, cfg.min_core_groups);
}

TEST(SolveResources, GpuClearIdleBringsBackOnline) {
    ResourceConfig cfg;
    cfg.min_core_groups = 1;
    ResourceInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_power_w = 1.0;   // GPU clearly idle (< 2W)
    inputs.have_gpu = true;
    inputs.temp_c = 50.0;
    inputs.cpu_demand = 0.05;
    inputs.gpu_c0_pct = 0.0;
    inputs.gpu_power_var_w = 0.0;
    inputs.total_core_groups = 16;
    inputs.prev_keep_groups = 1; // currently offline

    auto result = solve_resources(inputs, cfg);

    // GPU clearly idle → bring all cores back online
    EXPECT_EQ(result.keep_groups, 0);
}

// ── Hotplug smoothing: small changes are suppressed ──

TEST(SolveResources, HotplugSmoothingSuppressesSmallChange) {
    ResourceConfig cfg;
    cfg.min_core_groups = 1;
    ResourceInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_power_w = 10.0;   // GPU active
    inputs.have_gpu = true;
    inputs.temp_c = 50.0;
    inputs.cpu_demand = 0.42;    // medium demand → 16*(1-0.42*0.33) = 13.76 → 13
    inputs.gpu_c0_pct = 0.0;
    inputs.gpu_power_var_w = 2.0;
    inputs.total_core_groups = 16;
    inputs.prev_keep_groups = 13;  // same as solver would compute

    auto result = solve_resources(inputs, cfg);

    // prev == 13 and solver == 13 → no change, smoothing preserves
    EXPECT_EQ(result.keep_groups, 13);

    // Now try prev = 14 (diff of 1) → smoothing keeps prev
    inputs.prev_keep_groups = 14;
    auto result2 = solve_resources(inputs, cfg);
    EXPECT_EQ(result2.keep_groups, 14);  // within 1 → keep previous

    // Now try prev = 16 (diff of 3) → smoothing allows change
    inputs.prev_keep_groups = 16;
    auto result3 = solve_resources(inputs, cfg);
    EXPECT_EQ(result3.keep_groups, 13);  // diff > 1 → use solver value
}

// ═══════════════════════════════════════════════════════════
// Resource domain type tests
// ═══════════════════════════════════════════════════════════

TEST(ResourceDomain, ConstructorWithSetter) {
    ResourceDomain domain("cpu", 10.0, 50.0,
        [](double w) { return w <= 50.0; });

    EXPECT_EQ(domain.name, "cpu");
    EXPECT_DOUBLE_EQ(domain.current_power_w, 10.0);
    EXPECT_DOUBLE_EQ(domain.max_power_w, 50.0);
    EXPECT_DOUBLE_EQ(domain.granularity_w, 0.125);
}

TEST(ResourceDomain, SetBudgetSetter) {
    bool called = false;
    double captured = -1.0;

    ResourceDomain domain("gpu", 15.0, 100.0,
        [&called, &captured](double w) {
            called = true;
            captured = w;
            return true;
        });

    bool ok = domain.set_budget(25.0);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(called);
    EXPECT_DOUBLE_EQ(captured, 25.0);
}

TEST(ResourceResult, EffectiveCpuW) {
    ResourceResult result{};
    result.cpu_target_w = 20.0;
    result.thermal_surrender = 0.0; // thermal surrender disabled

    // effective_cpu_w() returns cpu_target_w (no thermal adjustment)
    EXPECT_DOUBLE_EQ(result.effective_cpu_w(), 20.0);
}

TEST(ResourceResult, EffectiveCpuWZero) {
    ResourceResult result{};
    result.cpu_target_w = 0.0;

    EXPECT_DOUBLE_EQ(result.effective_cpu_w(), 0.0);
}

// ═══════════════════════════════════════════════════════════
// Control channel tests
// ═══════════════════════════════════════════════════════════

TEST(SelectChannel, NoDeltaReturnsMinusOne) {
    std::vector<ControlChannel> channels;
    channels.emplace_back("rapl", 0.0, 100.0, 10.0, 0.125,
        [](double) { return true; }, []() { return "x"; });

    // delta of 0.01 >= threshold → picks channel
    EXPECT_EQ(select_channel(channels, 20.0, 20.01), 0);
    // exact match returns -1
    EXPECT_EQ(select_channel(channels, 20.0, 20.0), -1);
}

TEST(SelectChannel, PicksFastestChannel) {
    std::vector<ControlChannel> channels;

    // Slow channel
    channels.emplace_back("slow", 0.0, 50.0, 500.0, 5.0,
        [](double) { return true; }, []() { return "slow"; });

    // Fast channel
    channels.emplace_back("fast", 0.0, 50.0, 5.0, 0.125,
        [](double) { return true; }, []() { return "fast"; });

    int idx = select_channel(channels, 20.0, 30.0);
    EXPECT_EQ(idx, 1);  // fast channel (index 1)
}

TEST(SelectChannel, PicksChannelThatCoversDelta) {
    std::vector<ControlChannel> channels;

    // Small range channel
    channels.emplace_back("small", 0.0, 5.0, 10.0, 0.125,
        [](double) { return true; }, []() { return "small"; });

    // Large range channel
    channels.emplace_back("large", 0.0, 50.0, 50.0, 1.0,
        [](double) { return true; }, []() { return "large"; });

    int idx = select_channel(channels, 20.0, 30.0);  // delta = 10
    EXPECT_EQ(idx, 1);  // large channel covers 10W, small only covers 5W
}

TEST(SelectChannel, ReturnsMinusOneWhenNoChannelCovers) {
    std::vector<ControlChannel> channels;
    channels.emplace_back("tiny", 0.0, 2.0, 10.0, 0.125,
        [](double) { return true; }, []() { return "tiny"; });

    int idx = select_channel(channels, 20.0, 100.0);  // delta = 80
    EXPECT_EQ(idx, -1);  // no channel covers 80W
}

TEST(ApplyChannelBudget, QuantizesToGranularity) {
    ControlChannel ch("rapl", 0.0, 50.0, 10.0, 0.125,
        [](double w) { return w >= 0.0; }, []() { return ""; });

    // 10.06 → round to nearest 0.125 = 10.0
    bool ok = apply_channel_budget(ch, 10.06);
    EXPECT_TRUE(ok);  // within range

    // 10.07 → round to nearest 0.125 = 10.125
    ok = apply_channel_budget(ch, 10.07);
    EXPECT_TRUE(ok);
}

TEST(ApplyChannelBudget, ClampsToRange) {
    ControlChannel ch("epp", 2.0, 50.0, 50.0, 0.5,
        [](double w) { return w >= 2.0 && w <= 50.0; }, []() { return ""; });

    // Below min → clamped to 2.0
    bool ok = apply_channel_budget(ch, 0.5);
    EXPECT_TRUE(ok);  // should still succeed

    // Above max → clamped to 50.0
    ok = apply_channel_budget(ch, 80.0);
    EXPECT_TRUE(ok);
}

TEST(ApplyChannelBudget, UsesDefaultGranularity) {
    ControlChannel ch("custom", 0.0, 50.0, 10.0, 0.0,  // granularity=0 → default
        [](double /* w */) { return true; }, []() { return ""; });

    // 10.06 → default gran=0.125 → round to 10.0
    bool ok = apply_channel_budget(ch, 10.06);
    EXPECT_TRUE(ok);
}

// ═══════════════════════════════════════════════════════════
// Generic channel population tests
// ═══════════════════════════════════════════════════════════

TEST(PopulateChannels, GenericRapls) {
    std::vector<ControlChannel> channels;
    populate_generic_rapl_channel(channels, 50.0);

    EXPECT_EQ(channels.size(), 1u);
    EXPECT_EQ(channels[0].name, "rapl_budget");
    EXPECT_DOUBLE_EQ(channels[0].latency_ms, 10.0);
    EXPECT_DOUBLE_EQ(channels[0].granularity_w, 0.125);
    EXPECT_DOUBLE_EQ(channels[0].max_change_w, 50.0);
}

TEST(PopulateChannels, GenericEpp) {
    std::vector<ControlChannel> channels;
    populate_generic_epp_channel(channels, 5.0, 40.0);

    EXPECT_EQ(channels.size(), 1u);
    EXPECT_EQ(channels[0].name, "epp");
    EXPECT_DOUBLE_EQ(channels[0].latency_ms, 50.0);
    EXPECT_DOUBLE_EQ(channels[0].granularity_w, 0.5);
}

TEST(PopulateChannels, GenericTurbo) {
    std::vector<ControlChannel> channels;
    populate_generic_turbo_channel(channels, 50.0);

    EXPECT_EQ(channels.size(), 1u);
    EXPECT_EQ(channels[0].name, "turbo");
    EXPECT_DOUBLE_EQ(channels[0].latency_ms, 5.0);
    EXPECT_DOUBLE_EQ(channels[0].max_change_w, 5.0);  // 10% of 50
}

TEST(PopulateChannels, GenericHotplug) {
    std::vector<ControlChannel> channels;
    populate_generic_hotplug_channel(channels, 5.0, 40.0);

    EXPECT_EQ(channels.size(), 1u);
    EXPECT_EQ(channels[0].name, "hotplug");
    EXPECT_DOUBLE_EQ(channels[0].latency_ms, 5000.0);
    EXPECT_DOUBLE_EQ(channels[0].granularity_w, 5.0);
}

// ═══════════════════════════════════════════════════════════
// Edge cases
// ═══════════════════════════════════════════════════════════

TEST(SolveResources, NegativeGpuPower) {
    ResourceConfig cfg;
    ResourceInputs inputs{};
    inputs.pl1_w = 40.0;
    inputs.gpu_power_w = -1.0;  // shouldn't happen, but handle it
    inputs.have_gpu = true;
    inputs.temp_c = 50.0;
    inputs.cpu_demand = 1.0;
    inputs.gpu_power_var_w = 0.0;

    auto result = solve_resources(inputs, cfg);

    // gpu_power is negative → budget increases, but still clamped to domain_max
    EXPECT_GT(result.cpu_target_w, 30.0);
}

TEST(SolveResources, ZeroPl1) {
    ResourceConfig cfg;
    ResourceInputs inputs{};
    inputs.pl1_w = 0.0;
    inputs.gpu_power_w = 0.0;
    inputs.have_gpu = true;
    inputs.temp_c = 50.0;
    inputs.cpu_demand = 1.0;
    inputs.gpu_power_var_w = 0.0;

    auto result = solve_resources(inputs, cfg);

    // headroom = max(0, 1.0) = 1.0
    // cpu_budget = 0 - 0 - 1.0 = -1.0 → clamped to 0
    // then min_w floor = 2.0
    EXPECT_DOUBLE_EQ(result.cpu_target_w, cfg.cpu_min_w);
}

TEST(ThermalSurrenderFraction, FloatPrecision) {
    ResourceConfig cfg{70.0, 80.0, 90.0};

    // Thermal surrender is disabled — always 0.0 regardless of input
    double val = thermal_surrender_fraction(cfg, 70.001);
    EXPECT_DOUBLE_EQ(val, 0.0);

    val = thermal_surrender_fraction(cfg, 89.999);
    EXPECT_DOUBLE_EQ(val, 0.0);
}
