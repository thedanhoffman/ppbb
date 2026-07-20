# Project Audit Report: power-balance

## Overview
`power-balance` is a power management daemon designed for Intel hybrid architectures, specifically targeting platforms like Meteor Lake. Its primary goal is to ensure that the GPU always has sufficient power headroom by treating the CPU as the primary expendable component.

## Key Features & Implementation Details

### 1. GPU-First Power Balancing
The daemon classifies the GPU's state into three aggression levels:
- **Idle (0)**: GPU is in RC6 or drawing < 3W. CPU controls are relaxed.
- **Active (1)**: GPU is out of RC6 or drawing > 3W. CPU performance is moderated.
- **Throttle (2)**: Any serious GPU throttle reason is active. The daemon aggressively hammers the CPU to free up power.

### 2. Hardware Discovery
The system dynamically discovers:
- **RAPL Domains**: Scans `intel-rapl` and `intel-rapl-mmio` for package, core, and uncore subdomains.
- **GPU**: Identifies Xe driver tiles (`gt0`, `gt1`) via `/sys/class/drm`.
- **Temperatures**: Uses `coretemp` (hwmon) for CPU cores, and also supports NVMe and GPU thermal sensors.
- **Topology**: Analyzes `cpufreq` and `cpuinfo_max_freq` to group logical CPUs into physical core groups (P-cores and E-cores).

### 3. CPU Control Mechanisms
- **Frequency & Turbo**: Dynamically adjusts `max_perf_pct` and `no_turbo`.
- **EPP (Energy Performance Preference)**: Per-cluster EPP values are adjusted. P-cores are throttled first, while E-cores maintain higher responsiveness.
- **RAPL Budgeting**: The uncore (PP1) is set to unlimited (0), while the core (PP0) budget is calculated as `PL1 - GPU_draw - headroom`.
- **CPU Hotplug**: Offlines entire physical core groups to minimize leakage. Priority is given to offlining P-cores (highest indices first).

### 4. Specialized Power Modes
- **Temperature Overlay**: A secondary, conservative set of constraints overrides aggression levels based on core temperature.
    - 70°C+: Linear cap on `max_perf_pct`.
    - 82°C+: Forced `no_turbo`.
    - 85°C+: Shift to `balance_power` EPP.
    - 90°C+: Forced `power` EPP and minimum `max_perf_pct`.
- **Weak Charger Mode**: If a low-wattage charger (e.g., <45W USB-C) is detected while the battery is discharging/not charging, the daemon enters a "power-save" mode. This caps the package PL1 at ~50% of charger capacity and applies extreme restrictions on GPU frequency and CPU performance.
- **PROCHOT Handling**: To prevent aggressive EC-driven throttling, the daemon clears the external PROCHOT# response bit in MSR 0x1FC every cycle.

## Analysis & Tradeoffs

### Architectural Tradeoffs
- **Polling Frequency (500ms)**: A good balance between responsiveness to rapid GPU power spikes and minimizing CPU overhead from constant sysfs reads.
- **Core Offlining**: Very effective for reducing total package power but can lead to noticeable system stutters if too many groups are offlined. The "keep 2 P-core groups" rule is a necessary safety floor.
- **Hardcoded Thresholds**: While convenient, these thresholds (e.g., 82°C for turbo off) may need to be made configurable or model-specific in future iterations.

### Discrepancies & Observations
- **README vs. Code**: The implementation of `core_budget` perfectly aligns with the README's formula, with the code correctly interpreting "headroom" to include both a base margin and a temperature-dependent extra margin.
- **GPU Performance**: While the README claims GPU performance is "non-negotiable," the "weak charger" mode does cap GPU frequency. This is an acceptable trade-off as it prevents the charger from overheating or shutting down.

## Recommendations
1. **Configuration File**: Move hardcoded thresholds (temperatures, aggression levels, default PL1) to a configuration file to support diverse laptop models.
2. **Telemetry**: Add more granular logging for the "weak charger" transition to help users debug power issues.
3. **Refactoring**: The hardware discovery logic is currently quite manual (sysfs walking). Abstracting this into a cleaner "HardwareProvider" class structure would improve maintainability.
