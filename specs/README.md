# Intel Spec Documents — Power Management & MSRs

## Overview

This directory contains Intel architecture specification documents, kernel source files, and technical references covering the MSRs and power management features used by the `ppbb` project.

The documents support development of a standardized power management driver/interface across Intel CPUs, iGPUs, and dGPUs.

---

## Directory Structure

```
specs/
├── cpu/       — Intel SDM volumes, turbo boost documentation
├── gpu/       — iGPU/Xe architecture, register definitions
├── rapl/      — RAPL kernel internals, powercap documentation
├── kernel/    — Linux kernel source: MSR headers, RAPL drivers, thermal
├── thermal/   — Intel chipset thermal design guides
└── README.md  — This catalog
```

---

## CPU (`specs/cpu/`)

| File | Size | Description | Source |
|------|------|-------------|--------|
| `intel-sdm-vol3a.pdf` | — | SDM Vol 3A: System Programming, Part 1 (APIC, interrupts) | GitHub mirror (hititou/Intel_IA-32-Developers-Manual) |
| `intel-sdm-vol3b.pdf` | — | SDM Vol 3B: System Programming, Part 2 (MP, cache, bus) | GitHub mirror |
| `intel-sdm-vol-3b-sysprog-2.pdf` | 2.7 MB | SDM Vol 3B: System Programming, 2nd Edition (updated) | intel.com (Order #326018) |
| `intel-sdm-vol-3c-vmx.pdf` | 1.9 MB | SDM Vol 3C: VMX (Virtualization Technology) | intel.com (Order #326019) |
| `intel-sdm-vol-3d-sgx.pdf` | 1.8 MB | SDM Vol 3D: SGX (Software Guard Extensions) | intel.com (Order #332831) |
| `intel-sdm-vol4-msr.pdf` | — | SDM Vol 4: 64-bit IA-32 Architecture, MSR Index | GitHub mirror |
| `intel-tbt-config-per-core-turbo.pdf` | 203 KB | Turbo Boost Max Technology 3.1 — Per-Core Configuration Guide (Xeon Scalable) | intel.com |

**Key MSRs documented in SDM Vol 3/4:**
- `0x1FC` — MSR_IA32_POWER_CTL (BD_PROCHOT control)
- `0x19A` — MSR_IA32_THERM_CONTROL
- `0x1B1` — MSR_IA32_PACKAGE_THERM_STATUS
- `0x1B2` — MSR_IA32_PACKAGE_THERM_INTERRUPT
- `0x1A2` — MSR_IA32_THERM_STATUS
- `0x1B0` — MSR_IA32_PACKAGE_POWER_STATUS
- `0x600` — MSR_PLATFORM_POWER_LIMIT
- `0x601` — MSR_VR_CURRENT_CONFIG
- `0x610` — MSR_PKG_POWER_LIMIT (RAPL package)
- `0x611` — MSR_PKG_ENERGY_STATUS
- `0x614` — MSR_PKG_POWER_INFO
- `0x618` — MSR_DRAM_POWER_LIMIT
- `0x619` — MSR_DRAM_ENERGY_STATUS
- `0x638` — MSR_PP0_POWER_LIMIT (RAPL core)
- `0x639` — MSR_PP0_ENERGY_STATUS
- `0x640` — MSR_PP1_POWER_LIMIT (RAPL platform/uncore)
- `0x641` — MSR_PP1_ENERGY_STATUS
- `0x64D` — MSR_PLATFORM_ENERGY_STATUS
- `0x64F` — MSR_PERF_LIMIT_REASONS (Alder–Lunar Lake; MTL/LL only)
- `0x650` — MSR_PERF_STATUS
- `0x656` — MSR_TURBO_POWER_CURRENT_LIMIT
- `0x658` — MSR_TURBO_RATIO_LIMIT
- `0x659` — MSR_TURBO_RATIO_LIMIT_1
- `0x65A` — MSR_TURBO_RATIO_LIMIT_2
- `0x65C` — MSR_PLATFORM_POWER_LIMIT (platform)
- `0x660` — MSR_TIMEBASED_BINNING_THERMAL_REPORT
- `0x668` — MSR_HDCP_TOLUD
- `0x66D` — MSR_RAPL_POWER_UNIT
- `0x689` — MSR_CORE_POWER_STATUS
- `0x68F` — MSR_CORE_PERF_STATUS
- `0x690` — MSR_CORE_PERF_LIMIT_REASONS (Haswell–Skylake; MTL/LL use 0x64F)
- `0x6B0` — MSR_GFX_PERF_LIMIT_REASONS
- `0x6B1` — MSR_RING_PERF_LIMIT_REASONS

---

## GPU (`specs/gpu/`)

| File | Description | Source |
|------|-------------|--------|
| `intel-graphics-gen11-architecture.pdf` | Intel Graphics Architecture Gen11 (Tiger Lake/Ice Lake) | GitHub mirror |
| `i915_reg_defs.h` | i915 register definitions (hardware headers) | `spec.intel.com` / kernel source |

---

## RAPL (`specs/rapl/`)

| File | Type | Description | Source |
|------|------|-------------|--------|
| `intel-psst-rapl-deepwiki.html` | Wiki | Intel PSST RAPL deep-dive documentation | deepwiki.com |
| `intel-rapl-kernel-internals.html` | Article | Linux kernel RAPL internals overview | Various |
| `linux-kernel-powercap.rst` | RST | Powercap ReStructuredText source | kernel.org |
| `linux-kernel-rapl-doc.rst` | RST | RAPL ReStructuredText source | kernel.org |

**RAPL domains:**
- **PKG** (package) — MSR 0x610/0x611 — CPU package power
- **PP0** (core) — MSR 0x638/0x639 — CPU core power
- **PP1** (platform/uncore) — MSR 0x640/0x641 — CPU uncore + integrated GPU power
- **DRAM** — MSR 0x618/0x619 — DRAM power
- **Platform** — MSR 0x64D — Platform-wide energy

**RAPL constraints (sysfs):**
- `constraint_0_power_limit_uw` — PL1 (long-term power limit)
- `constraint_0_time_window_us` — PL1 time window (typically 256ms)
- `constraint_0_max_power_uw` — Maximum allowed PL1
- `constraint_1_power_limit_uw` — PL2 (short-term power limit)
- `constraint_1_time_window_us` — PL2 time window (typically 100ms)
- `constraint_2_power_limit_uw` — PL4 (peak/long-term limit)
- `constraint_2_time_window_us` — PL4 time window (typically 100s)

---

## Kernel (`specs/kernel/`)

### MSR Headers

| File | Description |
|------|-------------|
| `msr.h` | Main MSR I/O header |
| `msr-index.h` | MSR address constants |
| `msr-index-blob.h` | MSR index (standalone) |
| `linus-msr.h` | Linus's MSR header from kernel tree |
| `linus-msr-index.h` | Linus's MSR index |
| `msr-tools-lib-msr.h` | msr-tools library MSR header |

### Kernel Source — RAPL

| File | Description |
|------|-------------|
| `intel_rapl.h` | Intel RAPL kernel module header |
| `intel_rapl_common.c` | Shared RAPL code |
| `intel_rapl_msr.c` | MSR-based RAPL driver |
| `intel_rapl_tpmi.c` | TPMI-based RAPL driver (newer platforms) |
| `linus-intel_rapl.h` | RAPL header from kernel tree |

### Kernel Source — GPU/i915/Xe

| File | Description |
|------|-------------|
| `i915_regs.h` | Intel i915 register definitions |
| `i915_reg-v2.h` | i915 register definitions v2 |
| `igt-i915_reg.h` | IGT i915 register header |
| `igt-intel_regs.h` | IGT shared Intel registers |
| `igt-pcu_reg.h` | IGT power control unit registers |
| `igt-soc-intel-regs.h` | IGT SoC Intel registers |
| `xe_regs.h` | Intel Xe (Arc) GPU register definitions |
| `x15_reg_defs.h` | i915 register definitions |

### Kernel Source — MSR Tools

| File | Description |
|------|-------------|
| `rdmsr.c` | rdmsr utility source |
| `rdmsr-blob.c` | Minimal rdmsr blob |
| `wrmsr.c` | wrmsr utility source |
| `wrmsr-blob.c` | Minimal wrmsr blob |

### Kernel Documentation

| File | Description |
|------|-------------|
| `dtpm.rst` | Dynamic Thermal Power Management docs |
| `events-msr.rst` | Performance event MSR documentation |
| `intel-powerclamp.rst` | intel_powerclamp thermal throttling driver |
| `intel-thermal-throttle.rst` | Intel thermal throttle driver |
| `kvm-msr.rst` | KVM MSR passthrough documentation |
| `powercap.h` | Powercap kernel header |
| `powercap.rst` | Powercap framework documentation |
| `README-msr-tools.md` | msr-tools usage guide |

### CPU Family Definitions

| File | Description |
|------|-------------|
| `intel-family.h` | Intel CPU family/model ID definitions (kernel `arch/x86/include/asm/intel-family.h`) |

---

## Thermal (`specs/thermal/`)

| File | Description | Source |
|------|-------------|--------|
| `intel-8series-chipset-pch-thermal-guide.pdf` | 8 Series Chipset PCH Thermal Design Guide | intel.com |
| `intel-c600-chipset-thermal-guide.pdf` | C600 Chipset Thermal Design Guide | intel.com |
| `intel-xeon-3600-thermal-guide.pdf` | Xeon E5-2600 v3/v4 Thermal Guide | intel.com |
| `intel-xeon-e5-2400-thermal-guide.pdf` | Xeon E5-2400 Thermal Design Guide | intel.com |
| `intel-xeon-scalable-thermal-guide.pdf` | Xeon Scalable Platform Thermal Design Guide | intel.com |

---

## Project Source — MSR Platform Abstraction

These files are part of the `ppbb` project and use the spec documents above:

| File | Purpose |
|------|---------|
| `src/msr_platform.h` | Cross-platform MSR address abstraction, per-generation CPU model tables |
| `src/msr_platform.cpp` | MSR probing, validation, per-platform MSR selection |
| `src/power-utils.h` | Sysfs I/O, RAPL domain type, thermal/gpu temperature discovery |
| `src/power-utils.cpp` | Sysfs utilities, RAPL scanning, throttle reason tables |
| `src/power-balance.cpp` | GPU-first power balancer daemon |
| `src/power-optimizer.h` | Utility-maximizing power budget allocator (analytical solver) |
| `src/power-optimizer.cpp` | Solver implementation, performance curves, EPP/turbo/hotplug mapping |

---

## Key CPU Generation Coverage

The `msr_platform` layer handles these Intel generations:

| Generation | Family:Model | CPU Perf Limit MSR | Notes |
|------------|-------------|-------------------|-------|
| Haswell | 0x3C/0x3F/0x45/0x46 | 0x690 | Canonical MSR |
| Broadwell | 0x3D/0x47/0x4F/0x56 | 0x690 | |
| Skylake | 0x4E/0x55/0x5E | 0x690 | |
| Kaby Lake | 0x8E/0x9E | 0x690 | |
| Coffee Lake | 0x9E/0xA5 | 0x690 | |
| Comet Lake | 0xA6 | 0x690 | |
| Ice Lake | 0x7D/0x6A/0x6C/0x7E/0x9D | 0x690 | |
| Alder Lake | 0x97/0x9A | 0x690→0x64F | Probes 0x690 first, falls back to 0x64F |
| Raptor Lake | 0xB7/0xBA/0xBF | 0x690→0x64F | Probes 0x690 first, falls back to 0x64F |
| Meteor Lake | 0xAC/0xAA | 0x64F | 0x690 removed; only 0x64F |
| Lunar Lake | 0xBD | 0x64F | 0x690 removed; only 0x64F |

Unsupported platforms (non-Family-6 CPUs, unrecognized Family-6 models) are
**hard-rejected** with a clear error message — no silent fallback to safe defaults.

---

## Notes

- Official Intel download servers (`spec.intel.com`, `downloadmirror.intel.com`, `cdrdv2.intel.com`) were unreachable; documents sourced from GitHub mirrors and intel.com.
- SDM Vol 3C covers VMX (virtualization) and Vol 3D covers SGX — neither contains core MSR definitions needed for power management.
- Model-specific iGPU GT power management MSRs (e.g., 0xDE3) are not in the public SDM and require kernel source or vendor documentation.
- The Turbo Boost per-core guide applies to Xeon Scalable processors only.
