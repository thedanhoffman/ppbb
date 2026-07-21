// msr_platform.h — Cross-platform MSR address abstraction
//
// Intel MSRs are *model-specific*: the address for a given functional purpose
// can change between CPU generations.  This header provides a single API that
// returns the correct MSR address for the running CPU.
//
// ── Background ──
// The "perf-limit reasons" MSR moved between generations:
//   Haswell – Skylake:  0x690  (MSR_CORE_PERF_LIMIT_REASONS)
//   Haswell+ (always):  0x64F  (MSR_PERF_LIMIT_REASONS) — same bit layout
//   Meteor Lake+:       0x64F only  (0x690 removed)
//
// Both 0x64F and 0x690 share the identical bit layout (PROCHOT=bit0, Thermal=bit1,
// EDP=bit2, PL1=bit3, etc.) when they are present.  The lower 16 bits are
// "current status" (RO), the upper 16 bits are "sticky/log" (R/W to clear).
//
// This file provides:
//   1. A compile-time MSR address table keyed by CPU family/model.
//   2. Runtime probing (rdmsr-safe) to verify availability.
//   3. A unified `platform_msr_*()` API used by power-balance and power-status.

#pragma once

#include <cstdint>
#include <string>

// ═══════════════════════════════════════════════════════════
// MSR address categories — one address per conceptual purpose
// ═══════════════════════════════════════════════════════════

struct PlatformMSRs {
    // ── CPU perf-limit reasons (current status + sticky log) ──
    // Returns the MSR address that holds CPU-domain perf-limit reasons.
    // Lower 16 = current, upper 16 = sticky/log (clearable).
    //   Haswell–Skylake:  0x690
    //   Meteor Lake+:     0x64F
    // Bit layout: PROCHOT=0, Thermal=1, EDP=2, PL1=3, Platform=4,
    //             LowUtil=5, VR_Thermal=6, VR_TDC=7, TurboLimit=8,
    //             ... (see PERF_LIMIT_REASONS table in power-utils.h)
    uint32_t cpu_perf_limit = 0;   // 0 = not available

    // ── BD-PROCHOT disable ──
    // MSR_POWER_CTL bit 0.  Writing 0 disables the CPU's response to
    // the external PROCHOT# pin (asserted by laptop ECs).
    // Present on all CPUs from Core 2 (Nehalem) onward.
    uint32_t bd_prochot = 0x1FC;   // MSR_POWER_CTL — stable across gens

    // ── RAPL package power limit ──
    // MSR_PKG_POWER_LIMIT — bit 63 = PL_LOCK (BIOS lock),
    // bit 16 = clamp-enable.  Present on Haswell+.
    uint32_t rapl_pkg_limit = 0x610;

    // ── GPU perf-limit reasons (Xe / i915) ──
    // MSR_GFX_PERF_LIMIT_REASONS — GPU-domain perf-limit reasons.
    // Different bit layout than CPU (see xe_gt_regs.h).
    // Read via MMIO in the GPU driver; MSR access is per-core.
    uint32_t gpu_perf_limit = 0x6B0;

    // ── Ring/Uncore perf-limit reasons ──
    // MSR_RING_PERF_LIMIT_REASONS — uncore domain perf-limit reasons.
    uint32_t ring_perf_limit = 0x6B1;

    // ── Package thermal status ──
    // MSR_IA32_PACKAGE_THERM_STATUS — RO thermal status.
    // Not clearable.  Used for reference, not control.
    uint32_t pkg_thermal_status = 0x1B1;

    // ── Thermal control ──
    // MSR_IA32_THERM_CONTROL — thermal trip-point control.
    uint32_t therm_control = 0x19A;
};

// ═══════════════════════════════════════════════════════════
// Per-generation MSR tables
// ═══════════════════════════════════════════════════════════

// Haswell / Broadwell: 0x690 is the primary CPU perf-limit MSR.
// 0x64F also exists but 0x690 is the canonical name.
extern const PlatformMSRs msr_haswell;
extern const PlatformMSRs msr_skylake;

// Alder Lake / Meteor Lake / Lunar Lake: 0x690 removed.
// 0x64F (MSR_PERF_LIMIT_REASONS) is the only CPU perf-limit MSR.
extern const PlatformMSRs msr_adl;
extern const PlatformMSRs msr_meteor_lake;
extern const PlatformMSRs msr_lunar_lake;

// ═══════════════════════════════════════════════════════════
// Detection
// ═══════════════════════════════════════════════════════════

// Detect CPU family/model and return the matching MSR table.
// Called once at startup.  Falls back to probing if the model
// is not in the table.
PlatformMSRs detect_platform_msrs();

// Probe a single MSR to see if it is readable.
// Returns the value read, or (unsigned long long)-1 on failure.
unsigned long long probe_msr_read(uint32_t msr_addr);

// Probe a single MSR to see if it is writable.
// Returns true if the write succeeds (reads, compares, restores).
bool probe_msr_write(uint32_t msr_addr);

// Write a specific value to an MSR. Returns true on success.
bool probe_msr_write_with_value(uint32_t msr_addr, unsigned long long val);

// Validate all detected MSR addresses by probing.
// Replaces any unavailable MSR with 0.
PlatformMSRs validate_msrs(const PlatformMSRs& candidate);

// Pretty-print MSR table for logging.
std::string msrs_to_string(const PlatformMSRs& msrs);

// ═══════════════════════════════════════════════════════════
// Convenience wrappers (used by power-balance / power-status)
// ═══════════════════════════════════════════════════════════

// Read CPU perf-limit reasons from the correct MSR for this platform.
// Returns 0 if the MSR is not available.
unsigned long long platform_read_cpu_perf_limit(int cpu);

// Clear CPU perf-limit sticky/log bits by writing 0.
// Returns true if the write succeeded.
bool platform_clear_cpu_perf_limit(int cpu);

// Read GPU perf-limit reasons.
unsigned long long platform_read_gpu_perf_limit(int cpu);

// Read Ring/Uncore perf-limit reasons.
unsigned long long platform_read_ring_perf_limit(int cpu);

// Read BD-PROCHOT MSR (MSR_POWER_CTL) and clear bit 0.
// Returns true if the write succeeded.
bool platform_clear_bd_prochot();

// Read RAPL package power limit MSR (bit 63 = lock, bit 16 = clamp).
unsigned long long platform_read_rapl_pkg_limit();

// Initialize platform MSR detection (call once at startup).
// Returns the detected PlatformMSRs.
void init_platform_msrs();
PlatformMSRs get_platform_msrs();
