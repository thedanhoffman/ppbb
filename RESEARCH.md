# Research: Intel Power Management Features for power-balance

> **Sources:** ddgr web searches + Linux kernel source at `/home/dhoffman/xe-debug/linux`

---

## 1. RAPL Power Management

### Architecture
From `drivers/powercap/intel_rapl_common.c`:

RAPL exposes **5 domains**: `package`, `core` (PP0), `uncore` (PP1), `dram`, `psys`. Each domain supports up to 3 power limits:
- **PL1** (long_term) — always supported
- **PL2** (short_term) — burst power
- **PL4** (peak_power) — added for Meteor Lake (Sumeet Pawnikar patch, Feb 2023)

**Key insight:** PL4 was added for Meteor Lake SoC. The patch notes say PL4 is "package clamp" — it's the peak current limit for the entire package.

### Three Interfaces (MSR/TPMI/MMIO)
From the driver structure, RAPL supports:
- **MSR** (`intel_rapl_msr.c`) — reads via `RDMSR`/`WRMSR` instructions on CPU
- **TPMI** (`intel_rapl_tpmi.c`) — reads via TPMI (Thunderbolt Platform Management Interface) for non-CPU domains
- **MMIO** — reads via `intel_rapl_mmio` for memory-mapped regions

The driver abstracts these interfaces behind `read_raw`/`write_raw` callbacks in `rapl_if_priv`.

### PMU Events
RAPL exposes Perf PMU events:
```
energy-cores   (event=0x01)  — RAPL_DOMAIN_PP0 (all cores)
energy-pkg     (event=0x02)  — RAPL_DOMAIN_PACKAGE
energy-ram     (event=0x03)  — RAPL_DOMAIN_DRAM
energy-gpu     (event=0x04)  — RAPL_DOMAIN_PP1 (GPU/uncore)
energy-psys    (event=0x05)  — RAPL_DOMAIN_PLATFORM
```

**Key finding:** `energy-gpu` maps to `RAPL_DOMAIN_PP1` (uncore). On Meteor Lake, the GPU power is measured as part of the uncore domain, confirming that `gpu_w` derived from uncore RAPL delta is correct.

### Power Limit IRQ
The driver saves/restores `PACKAGE_THERM_INT_PLN_ENABLE` in MSR `MSR_IA32_PACKAGE_THERM_INTERRUPT`. When RAPL limits are set, the driver disables the PLN IRQ to avoid excessive interrupts from artificial power limits.

### Suspension Handling
RAPL power limits are saved on `PM_SUSPEND_PREPARE` and restored on `PM_POST_SUSPEND`. This is important for the daemon — if the system suspends, the RAPL limits will be restored by the kernel, not the daemon.

---

## 2. intel_pstate CPU Power Control

### Global Parameters (from `drivers/cpufreq/intel_pstate.c`)
```c
struct global_params {
    bool no_turbo;       // intel_pstate/no_turbo
    bool turbo_disabled; // hardware check
    int max_perf_pct;    // intel_pstate/max_perf_pct (0-100)
    int min_perf_pct;    // intel_pstate/min_perf_pct (0-100)
};
```

### HWP (Hardware Managed Performance States)
The driver supports two modes:
1. **Active mode** — driver directly writes `MSR_IA32_PERF_CTL` to set P-state
2. **HWP mode** — driver writes `MSR_HWP_REQUEST (0x774)` with `max/min/energy_perf_preference` hints, and hardware autonomously selects P-state

**EPP** (`energy_performance_preference`) is the sysfs interface to MSR `MSR_HWP_REQUEST_EPP` on each CPU. Values: `performance`, `balance_performance`, `balance_power`, `power`.

### Key MSRs
- **`MSR_HWP_REQUEST (0x774)`** — sets HWP max/min/EPP hints per-CPU
- **`MSR_HWP_STATUS (0x777)`** — reports current HWP status
- **`MSR_IA32_PERF_CTL (0x199)`** — direct P-state control (active mode)
- **`MSR_IA32_PACKAGE_THERM_INTERRUPT`** — thermal interrupt config
- **`MSR_IA32_PM_ENABLE (0x770)`** — HWP enable/disable
- **`MSR_IA32_ENERGY_PERF_BIAS (0x1B0)`** — global EPP policy hint (deprecated in favor of per-CPU EPP)

### Sampling
The driver samples APERF/MPERF every 10ms (`INTEL_PSTATE_SAMPLING_INTERVAL = 10ms`) to determine current performance and select next P-state.

---

## 3. PROCHOT Handling

### BD PROCHOT (MSR 0x1FC)
From ddgr research, `MSR_IA32_POWER_CTL (0x1FC)` bit 0 enables/disables external PROCHOT# response:
- `bit 0 = 1` → CPU responds to external PROCHOT# (throttles to minimum)
- `bit 0 = 0` → CPU ignores external PROCHOT#

The daemon correctly clears this bit every cycle because the EC/firmware continuously re-enables it.

### Internal vs External PROCHOT
`MSR_IA32_PACKAGE_THERM_STATUS (0x1B1)`:
- Bits 0-1: Internal temperature (TM1/TM2)
- Bits 2-3: External PROCHOT signal

This can distinguish between CPU thermal throttling and external (VRM, EC) thermal signals.

### Weak Charger Detection
The daemon detects weak chargers by:
1. Battery status = `Discharging` or `Not charging` while AC is online
2. USB-C PD contract < 45W (from `current_max * voltage_max` in sysfs)

The weak charger path enters power-save mode with aggressive constraints.

---

## 4. Xe GPU Power Management

### GuC SLPC (Single Loop Power Controller)
From `drivers/gpu/drm/xe/xe_guc_pc.c` and `abi/guc_actions_slpc_abi.h`:

SLPC is the firmware-based power management controller in GuC. Key parameters (from `enum slpc_param_id`):

| Param | Description |
|-------|-------------|
| `SLPC_PARAM_GLOBAL_MIN_GT_UNSLICE_FREQ_MHZ` (6) | Min freq |
| `SLPC_PARAM_GLOBAL_MAX_GT_UNSLICE_FREQ_MHZ` (7) | Max freq |
| `SLPC_PARAM_POWER_PROFILE` (27) | Base (0) or Power Saving (1) |
| `SLPC_PARAM_STRATEGIES` (26) | Optimized strategy for compute |
| `SLPC_PARAM_ENABLE_IA_FREQ_LIMITING` (25) | IA/GT balancing |
| `SLPC_PARAM_GLOBAL_ENABLE_ADAPTIVE_BURST_TURBO` (17) | Adaptive burst turbo |
| `SLPC_PARAM_GLOBAL_ENABLE_EVAL_MODE` (18) | Evaluation mode |
| `SLPC_PARAM_GLOBAL_ENABLE_BALANCER_IN_NON_GAMING_MODE` (19) | IA/GT balancer |
| `SLPC_PARAM_TASK_ENABLE_BALANCER` (2) | Enable IA/GT balancer task |
| `SLPC_PARAM_IGNORE_EFFICIENT_FREQUENCY` (28) | Bypass RPe frequency floor |

### SLPC Shared Data (2 pages = 8KB)
The shared data structure (`struct slpc_shared_data`) provides:
- `header.global_state` — `NOT_RUNNING`/`INITIALIZING`/`RUNNING`/`SHUTTING_DOWN`/`ERROR`
- `task_state_data.status` — GTPERF, DCC, Balancer, IBC, Balancer IA LMT status
- `task_state_data.freq` — current min/max unslice and slice frequencies
- `override_params` — 256 override parameters for fine-tuning

### Frequency Levels
From `xe_gt_freq.c` sysfs attributes:
- `act_freq` — Actual resolved frequency (PCODE decision)
- `cur_freq` — Current frequency requested by GuC PC
- `rpn_freq` — Render Performance N (minimum, RP0)
- `rpa_freq` — Render Performance A (achievable, runtime-calculated)
- `rpe_freq` — Render Performance E (efficient, runtime-calculated)
- `rp0_freq` — Render Performance 0 (maximum, fused)

**Key finding:** PCODE is the ultimate frequency decision-maker, not GuC SLPC. SLPC sets min/max bounds, but PCODE decides actual frequency based on thermal/power conditions.

### Power Profiles
Two profiles: `SLPC_POWER_PROFILE_BASE (0)` and `SLPC_POWER_PROFILE_POWER_SAVING (1)`.

Power saving mode:
- Uses conservative up/down frequency thresholds
- Disables waitboosts
- Suitable for power-efficient workloads

### Engine Activity Stats
From `drivers/gpu/drm/xe/xe_guc_engine_activity.c` (GuC interface v1.14.1+):

The engine activity API provides per-engine (render/compute/media) active tick counts and total tick allocations. This is a **much more accurate** measure of GPU activity than C0 residency.

API functions:
- `xe_guc_engine_activity_active_ticks()` — accumulated active ticks
- `xe_guc_engine_activity_total_ticks()` — total allocated quanta ticks

**Opportunity:** This could replace the C0 residency approach for determining GPU activity. The ratio `active_ticks / total_ticks` gives exact utilization.

---

## 5. Xe GPU Throttle Reasons

From `drivers/gpu/drm/xe/xe_gt_throttle.c` and `regs/xe_gt_regs.h`:

### GT0 Throttle Reasons (MSR 0x1381A8, mask 0xDE3)
| Bit | Name | Description |
|-----|------|-------------|
| 0 | `prochot` | External PROCHOT assertion |
| 1 | `thermal` | GPU thermal limit |
| 5 | `ratl` | RATL thermal algorithm |
| 6 | `vr_thermalert` | VR thermal alert |
| 7 | `vr_tdc` | VR TDC (thermal design current) |
| 8 | `pl4` | Package PL4 (peak current) |
| 10 | `pl1` | Package PL1 (long-term power) |
| 11 | `pl2` | Package PL2 (short-term power) |

### Crescent Island (next-gen) Throttle Reasons (mask 0xFDFF)
Additional reasons on CRI:
| Bit | Name | Description |
|-----|------|-------------|
| 1 | `soc_thermal` | SoC thermal limit |
| 2 | `mem_thermal` | Memory thermal limit |
| 3 | `vr_thermal` | VR thermal (replaces vr_thermalert) |
| 4 | `iccmax` | ICCMAX (peak current) |
| 6 | `soc_avg_thermal` | SoC average temperature |
| 7 | `fastvmode` | VR FastVMode |
| 12 | `psys_pl1` | PSYS PL1 |
| 13 | `psys_pl2` | PSYS PL2 |
| 14 | `p0_freq` | P0 frequency limit |
| 15 | `psys_crit` | PSYS critical |

**Key finding:** PL4 is at bit 8 on GT0. The daemon correctly excludes PL4 from aggression detection because it's GuC firmware's internal peak current management.

**Key finding:** On Meteor Lake, GT1 (media) uses a different register: `MTL_MEDIA_PERF_LIMIT_REASONS (0x138030)`.

---

## 6. Idle Injection

From `drivers/powercap/idle_inject.c`:

The idle injection framework:
1. Creates per-CPU kthreads with RT priority (`sched_set_fifo`)
2. Injects idle time via `play_idle_precise(idle_duration_us, latency_us)`
3. Runs on a configurable cpumask
4. Timer-based periodic injection cycles (idle + run duration)

The daemon uses `intel_powerclamp` sysfs interface (not the low-level idle injection API). This is correct for the use case.

---

## 7. Turbo Ratio Limits

From `MSR_TURBO_RATIO_LIMIT (0x198-0x1BB)`:

- Each MSR holds 4 turbo ratios (8 bits each)
- Ratio = base frequency × N (where base = 100MHz)
- Can be locked/unlocked per-core
- Per-core turbo limits available on Rocket Lake+ (MSR 0x1B3+)

**Opportunity:** Fine-grained per-core turbo limits could replace the binary `no_turbo` with graduated turbo reduction.

---

## 8. Key Findings and Recommendations

### Findings Confirming Current Design
1. **GPU power from uncore RAPL is correct** — `energy-gpu` PMU event maps to `RAPL_DOMAIN_PP1`
2. **PL4 exclusion is correct** — PL4 is GuC firmware internal management
3. **PROCHOT handling is correct** — MSR 0x1FC bit 0 is the standard mechanism
4. **SLPC power profiles** have exactly two options: `base` and `power_saving`
5. **PCODE is ultimate frequency arbiter** — daemon sets bounds, hardware decides

### Critical Gap: hwmon/iGPU Temperature
6. **hwmon is dGPU-only** — The daemon's `hwmon/gpu_temp` path does NOT work on Meteor Lake iGPU. The daemon should fall back to thermal zone sensors for GPU temperature on integrated platforms.
7. **SLPC balancer strategies** — The kernel intentionally disables SLPC Balancer/DCC strategies because the daemon wants to set explicit CPU/GPU power bounds (SLPC balancer tries to do the same internally).

### New Opportunities
1. **Engine Activity API** (v1.14.1+) — Provides per-engine utilization via `active_ticks/total_ticks`. More accurate than C0 residency.
2. **Per-core turbo limits** — MSRs 0x198-0x1BB allow graduated turbo reduction instead of binary on/off.
3. **SLPC IA/GT Balancer** — `SLPC_PARAM_ENABLE_IA_FREQ_LIMITING` and `SLPC_PARAM_TASK_ENABLE_BALANCER` let GuC firmware handle IA/GT power balancing internally.
4. **SLPC Adaptive Burst Turbo** — `SLPC_PARAM_GLOBAL_ENABLE_ADAPTIVE_BURST_TURBO` allows firmware to adaptively manage GPU turbo.
5. **HWP EPP per-CPU** — Each CPU has independent EPP in MSR_HWP_REQUEST, allowing finer-grained control than current per-cluster approach.

### Platform-Specific Notes
- **Meteor Lake (MTL)**: GT0 at 0x1381A8, GT1 at 0x138030, RP_CAP at MTL_RP_STATE_CAP
- **Crescent Island (CRI)**: Extended throttle reasons (16 reasons vs 10), additional thermal domains
- **Panther Lake (PL)**: SLPC defaults modified (DCC disabled)
- **BattleMage (BMG)**: Min freq default set to 1.2GHz for GT0

---

## 9. Xe hwmon Interface

### Discrete GPU Only
The `xe_hwmon.c` driver **only registers hwmon for discrete GPUs** (`IS_DGFX(xe)` check at line 1550).

**This means on Meteor Lake iGPU (the primary target platform), the hwmon interface for GPU temperature is NOT available.** The daemon's `hwmon/gpu_temp` path may not exist on Meteor Lake.

### hwmon Interface (dGPU only)
The hwmon driver provides:
- **Temperature**: `temp1_input` (pkg), `temp2_input` (vram), `temp3_input` (mctrl), `temp4_input` (pcie)
- **Power limits**: `power1_max` (PSYS PL1), `power2_max` (PKG PL1), `power1_cap` (PSYS PL2), `power2_cap` (PKG PL2)
- **Energy**: `energy1_input` (card/platform), `energy2_input` (package)
- **Fan speed**: `fan1_input`, `fan2_input`, `fan3_input`
- **Current**: `curr1_crit` (peak current limit)

### Power Limit Write Path
hwmon supports writing power limits via two paths:
1. **PCODE mailbox commands** — for platforms with `has_mbx_power_limits` (Crescent Island, etc.)
2. **Direct MMIO writes** — for older platforms (PVC, DG2)

Power limits are clamped to GPU firmware defaults as maximum (additional protection).

### Thermal Zone Interface (iGPU)
For integrated GPUs on Meteor Lake, GPU temperature is available via:
- `/sys/class/thermal/thermal_zone*/type` — identifies sensor type (e.g., `x86_pkg_temp`, `SOC DTS`, `CPU`)
- `/sys/class/thermal/thermal_zone*/temp` — temperature in millidegrees Celsius

The daemon already reads thermal zones, but GPU-specific thermal zones may be labeled differently than expected.

---

## 10. SLPC Tuning Interface

### sysfs Interface
SLPC parameters are exposed via `drivers/gpu/drm/xe/xe_gt_freq.c` sysfs:
- `power_profile` — read/write: `base` or `power_saving`
- `min_freq_mhz`, `max_freq_mhz` — frequency bounds
- Various `*_freq` attributes — frequency state readings

### Shared Data Structure
The 2-page shared data buffer provides:
- `header.global_state` — SLPC state machine (`NOT_RUNNING`/`INITIALIZING`/`RUNNING`/`SHUTTING_DOWN`/`ERROR`)
- `task_state_data.status` — bit flags for GTPERF, DCC, Balancer, IBC, Balancer IA LMT
- `task_state_data.freq` — current min/max frequencies (unslice + slice)
- `override_params` — 256 override parameters for fine-tuning

### SLPC Default Strategies
From i915 driver research, the Balancer and DCC strategies:
- Only activate when system is TDP-limited
- Do not conflict with waitboost (they guarantee minimum GT frequency under TDP pressure)
- Are controlled by `SLPC_PARAM_STRATEGIES` parameter
