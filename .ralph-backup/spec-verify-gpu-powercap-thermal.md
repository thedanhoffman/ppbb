## Goal: Verify GPU, powercap, thermal, and remaining sysfs interfaces against specs

All items were completed in the previous "spec-verification-msr" loop. See that task file for full details.

## Checklist

- [x] **6. Xe GPU throttle registers** — VERIFIED. XE_THROTTLE_REASONS/FILES match specs/kernel/xe_regs.h GT0_PERF_LIMIT_REASONS (0x1381a8, mask 0xde3). Bits: 0=PROCHOT, 1=THERMAL_LIMIT, 5=RATL, 6=VR_THERMALERT, 7=VR_TDC, 8=POWER_LIMIT_4, 10=POWER_LIMIT_1, 11=POWER_LIMIT_2. Escalation uses indices 4-7 (thermal/ratl/vr_tdc/vr_thermalert). Comment in power-utils.h enhanced.
- [x] **7. Intel i915/GT sysfs interface** — VERIFIED. All paths are standard Xe driver interfaces: gt0/freq0/cur_freq, max_freq, min_freq, power_profile, gtidle/idle_status, activity/c0_residency_ms. GT1 follows same structure.
- [x] **8. Intel family/capability detection** — VERIFIED. All 24 CPU model IDs match specs/kernel/intel-family.h. MSR table selection: legacy (0x690) for HSW-SKL, probe-fallback for ADL/RPL, MTL/LNL use 0x64F only. Runtime probing validates MSR availability.
- [x] **9. intel_powerclamp interface** — VERIFIED. Cooling device type intel_powerclamp under /sys/class/thermal/cooling_deviceN/, interface via cur_state. Matches specs/kernel/intel-powerclamp.rst.
- [x] **10. powercap subsystem** — VERIFIED. Sysfs paths match specs/kernel/powercap.h constraint interface. Unit naming: _uw (microwatts), _uj (microjoules), _us (microseconds). Domain names: package-0, core, uncore, dram, pp0, pp1.
- [x] **11. Thermal sensor interfaces** — VERIFIED. Thermal zones: x86_pkg_temp, SOC DTS, CPU (standard Intel types per intel-thermal-throttle.rst). coretemp hwmon tempX_input = millidegrees C.
- [x] **12. CPU topology enumeration** — VERIFIED. core_cpus_list and thread_siblings_list parsing matches Linux kernel smpboot.c conventions.
- [x] **13. intel_pstate interface** — VERIFIED. max_perf_pct (0-100%), min_perf_pct (0-100%), no_turbo (0/1) match kernel intel_pstate.c.
- [x] **14. EPP (Energy Performance Preference)** — VERIFIED. Per-core path /sys/devices/system/cpu/cpuN/cpufreq/energy_performance_preference. Values: performance, balance_performance, balance_power, power. EppLevel enum maps all four.
- [x] **15. CPU hotplug (core offlining)** — VERIFIED. cpu/online (0/1). Sibling group offlining matches kernel requirement. P-cores first, then E-cores. CPU0 + min 2 P-core groups always online. 20-cycle settle period.
- [x] **17. power-status MSR usage** — VERIFIED. Uses platform_read_cpu_perf_limit(), platform_read_rapl_pkg_limit(), platform_clear_cpu_perf_limit() — all go through msr_platform.h abstraction. No hardcoded MSR addresses.

## Notes
- All spec documents are in `/home/dhoffman/ppbb/specs/`
- Source code is in `/home/dhoffman/ppbb/src/`
- When you find naming discrepancies, fix both the code comments AND the README
- The goal is consistency: spec name → code comment → README description

## Verification Summary

All 17 checklist items from the parent loop completed. Key changes made:
- Fixed MSR bit layout comment in msr_platform.h (bits 4-8 swapped/wrong names)
- Fixed README.md: CPU MSR address (was wrong 0x6B0), GPU throttle escalation, PROCHOT handling, power-save mode, GPU power profile
- Fixed code comments: power-balance.cpp MSR addresses, power-status.cpp MSR addresses
- Fixed specs/README.md: GPU/Ring perf-limit MSR addresses
- Fixed XE_THROTTLE_REASONS comment in power-utils.h

## Completion Gate Verification

**Command:** `cd /home/dhoffman/ppbb && cmake -B build_verify -DCMAKE_BUILD_TYPE=Debug && cmake --build build_verify -- -j4 && ctest --test-dir build_verify`
**Working directory:** `/home/dhoffman/ppbb`
**Environment:** GCC 15.2.0, GTest 1.17.0
**Result:** 100% tests passed (40/40) in 0.15s
**Build artifacts preserved:** `build_verify/` directory
**Modified files:** README.md, src/msr_platform.h, src/power-balance.cpp, src/power-status.cpp, src/power-utils.h
