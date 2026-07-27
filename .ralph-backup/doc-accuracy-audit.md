## Goal: Audit and correct all project documentation against the current codebase

Every section of README.md and specs/README.md must be verified against the actual implementation. Inaccurate descriptions, stale algorithm references, wrong MSR mappings, and outdated behavior must be corrected.

## Checklist

- [x] **1. README.md — Aggression level section** (lines 12–35) — FIXED
  - Replaced discrete aggression level table with utility-maximizing optimizer description
  - Added analytical water-filling math, C0 residency scaling, discrete control mapping
  - Clarified that `aggression_from_weights` is logging-only

- [x] **2. README.md — RAPL budget allocation** (lines 37–44) — FIXED
  - Fixed uncore budget: now correctly says `max_w()` not "0 (unlimited)"
  - Added demand_factor scaling, analytical headroom, GPU throttling cap

- [x] **3. README.md — Package PL1 section** (lines 46–51) — FIXED
  - Verified default 40W is correct
  - Added PL4 clamping behavior that the code actually implements

- [x] **4. README.md — Temperature-aware overlay** (lines 53–70) — FIXED
  - Replaced piecewise-linear table with actual cubic ramp from t_warn to t_max
  - Configurable thresholds documented (t_target=70, t_warn=80, t_max=90)
  - Thermal headroom computed from discomfort * thermal_headroom_max
  - Hard thermal floor described as safety net

- [x] **5. README.md — GPU throttle monitoring** (lines 72–87) — VERIFIED
  - 8 throttle reasons match XE_THROTTLE_REASONS[]
  - PL1/PL2/PL4/PROCHOT correctly described as NOT escalation triggers
  - thermal/ratl/vr_tdc/vr_thermalert correctly described as escalators

- [x] **6. README.md — GPU power profile** (lines 89–94) — VERIFIED
  - Both GT0 and GT1 set to power_saving confirmed in code

- [x] **7. README.md — CPU MSR perf limit monitoring** (lines 96–110) — VERIFIED
  - MSR addresses (0x690 for HSW-SKL, 0x64F for MTL/LL) match msr_platform.cpp
  - Bit layout matches PERF_LIMIT_REASONS[] array
  - PROCHOT bit 0 correctly excluded

- [x] **8. README.md — PROCHOT handling** (lines 112–128) — FIXED
  - Changed "rewrites it each cycle" to "cleared at startup (MSR is persistent)"
  - Save/restore on exit behavior confirmed correct

- [x] **9. README.md — CPU controls table** (lines 130–140) — FIXED
  - EPP cluster assignment clarified: E-cores 1 tier less aggressive ONLY when not under stress

- [x] **10. README.md — CPU hotplug section** (lines 142–175) — FIXED
  - Replaced aggression/temperature-based triggers with CPU power ratio thresholds
  - Groups kept: ≤12 (>0.7), ≤8 (>0.5), ≤4 (>0.3), 2 min floor (<0.3)
  - min_core_groups=2 verified in code

- [x] **11. README.md — Hardware discovery** (lines 177–188) — FIXED
  - GPU path corrected from `tile0/gt0/freq0/cur_freq` to `gt*/freq0/cur_freq`

- [x] **12. README.md — Usage section** (build, run, systemd) — FIXED
  - Manual build command now includes all required source files
  - `--pl1` flag verified working as documented
  - systemd service file confirmed to exist

- [x] **13. README.md — Log output examples** — FIXED
  - Added `demand=` and `c0=` fields that actual code outputs
  - State label changed from `[active]`/`[balance-throttle]` to `[active]`/`[throttle]`

- [x] **14. specs/README.md — MSR index table** — FIXED
  - Removed duplicate 0x6B0 and 0x6B1 entries
  - Fixed PP0: 0x638/0x639 (was 0x620/0x621)
  - Fixed PP1: 0x640/0x641 (was 0x630/0x631)
  - Fixed DRAM: 0x618/0x619 (was 0x640/0x641)
  - Fixed PKG_POWER_INFO: 0x614 (was 0x619)
  - Fixed Platform Energy: 0x64D (was 0x639)
  - Added MSR_PLATFORM_POWER_LIMIT 0x65C
  - Fixed CPU Perf Limit MSR notes (0x64F for Alder-Lunar, 0x690 for HSW-SKL)

- [x] **15. specs/README.md — RAPL domains table** — FIXED
  - All MSR addresses corrected against kernel source

- [x] **16. specs/README.md — Key CPU generation coverage** — FIXED
  - Model IDs match msr_platform.cpp
  - Updated fallback notes to match actual probe behavior
  - Added hard rejection note for unsupported platforms

- [x] **17. In-code documentation** — verified header file comments:
  - `msr_platform.h` — struct comments, function docs all accurate
  - `power-optimizer.h` — algorithm description matches solver
  - `power-utils.h` — RAPL/sysfs path comments, RAPL domain struct accurate
  - `power-status.cpp` — compile comment fixed to include all source files

---

## Final Verification Command

From a fresh shell in the same worktree, verify the documentation audit:

```sh
cd /home/dhoffman/ppbb
cmake -B build && cmake --build build 2>&1 | tail -3
./build/test-power-balance 2>&1 | tail -2
```

Expected output:
```
[100%] Built target test-power-balance
[  PASSED  ] 40 tests.
```

All 17 documentation items have been audited and corrected. The README.md and specs/README.md now accurately describe the utility-maximizing optimizer, RAPL budget allocation, temperature-aware capping, GPU throttle monitoring, PROCHOT handling, CPU hotplug, and MSR addresses.