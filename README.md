# power-balance

GPU-first power balancer daemon for Intel hybrid architectures (Meteor Lake
and similar).  The **GPU must never throttle** — the CPU is the expendable
buffer.  The daemon constrains CPU frequency, turbo, RAPL budgets, EPP, and
CPU core topology so the GPU always has full power headroom, while
independently capping CPU temperature.

Also includes `power-status` — a terminal power monitor (self-explanatory:
`power-status --help`).

## How it works

### Utility-maximizing optimizer

Every 500 ms the daemon reads RAPL energy counters for the package, core
(PP0), and uncore (PP1) domains, computes instantaneous power draw, and
feeds observed state into a constrained utility maximization solver:

```
  maximize  U = α·f_gpu(P_gpu) + β·f_cpu(P_cpu) − γ·h(T) − δ·I(throttle)
  subject to: P_cpu + P_gpu + H ≤ PL1
```

**Performance curves** — saturating exponential: `f(P) = P_max·(1 − exp(−P/P_scale))`
  CPU: `P_max=1.0`, `P_scale=25 W` GPU: `P_max=1.0`, `P_scale=20 W`

**C0 residency scaling** — GPU C0 ≥ 70% doubles the GPU weight (α→2α), giving
GPU performance higher priority during active workloads.

**Analytical headroom** — the optimal headroom `H` is found by equating marginal
utilities (water-filling), yielding a closed-form solution:
```
  H_opt = (PL1 − P_gpu − K·ln(weight_ratio)) / (1 + cpu_p_scale/gpu_p_scale)
```
This is clamped to a **probabilistic floor**: `H_base + z·H_nominal + spike_margin`,
ensuring the GPU always has reserved power headroom even when the analytical
solution would allocate too little.

**Discrete control mapping** — continuous optimal power allocations are
translated into sysfs controls:
  - `max_perf_pct`: inverse-squared mapping from CPU power budget
  - `no_turbo`: disabled when GPU is throttling or temp ≥ 82 °C
  - `EPP`: selected by CPU power utilization ratio (Power/BalancePower/BalancePerformance)
  - `hotplug`: core groups offlined as CPU power demand drops below threshold

The `aggression` label in log output is derived from computed weights
(throttle events, GPU activity, CPU demand) and is **for logging only** —
it does not drive control decisions.

### RAPL budget allocation

The daemon sets the uncore (PP1) RAPL power limit to its **max_w** value
(unlimited within hardware limits), and applies the optimizer's computed
core budget (PP0):

```
core_budget = (PL1 − gpu_draw − gpu_headroom) × demand_factor
```

where `gpu_headroom` is the larger of the analytical water-filling solution
and the probabilistic floor, and `demand_factor` (0.5–1.0) scales the CPU
budget downward when scheduler pressure is low (idle CPU has no demand).
When GPU throttling is detected, `core_budget` is hard-capped at `cpu_critical_max_w`
(default 8 W).

On Meteor Lake the MMIO RAPL domain (`intel-rapl-mmio`) is also kept in sync
with the MSR PL1 so it does not become a hidden bottleneck.

### Package PL1

The daemon sets the package PL1 to a configurable value at startup (default
40 W; override with `--pl1 <watts>`).  While the declared `constraint_0_max_power_uw`
from the RAPL hardware may be lower (e.g., 28 W), the hardware VR accepts
higher values.  Raising PL1 gives the GPU tile's voltage regulator more peak
current headroom, dramatically reducing GPU PL4 (peak power) events.  If the
specified PL1 exceeds the hardware's PL4 limit, the daemon clamps PL1 down to
the PL4 value and warns via syslog.

### Temperature monitoring (read-only)

The daemon independently reads the hottest core temperature from `coretemp`
and logs it in periodic output. Temperature is **not** used in the solver's
budget allocation — on laptops, temperature is an ambient condition (GPU heat,
VRM heat, battery heat) that doesn't reliably indicate CPU power draw. The
GPU-first power budget (`PL1 - GPU - headroom`) is the only real constraint.

Temperature is still logged in periodic syslog lines (`temp=85C`) and
shown by `power-status` for operator awareness.

### GPU throttle monitoring

The daemon watches the Xe driver's 8 throttle reason flags:
`reason_pl1`, `reason_pl2`, `reason_pl4`, `reason_prochot`,
`reason_thermal`, `reason_ratl`, `reason_vr_tdc`, `reason_vr_thermalert`.
Sysfs path: `gt0/freq0/throttle/reason_*`. Rising-edge events are
accumulated in counters, printed in periodic log lines as `gpu-throttle:`,
and summarized on exit. Throttle detection is suppressed for the first 3
cycles to let RAPL counters settle.

PL1/PL2 are expected under load, PL4 is normal GuC peak current management,
and PROCHOT is an external pin signal — none of these affect the optimizer's
gpu_throttling flag. GPU **thermal**, **ratl** (runtime thermal limit), **vr_tdc**
(thermal current limit), and **vr_thermalert** (over-temperature) set
gpu_throttling=true which invokes the throttle penalty in the utility function.
All 8 reasons are still tracked and logged.

### GPU power profile

The daemon forces both GPU tiles (gt0 render, gt1 media) to the `power_saving`
power profile at startup, overriding the `base` default.  The
original profile is saved and restored on exit.  GPU performance is
non-negotiable — all CPU controls are the expendable variable.

### CPU MSR perf limit monitoring

The daemon reads `MSR_CORE_PERF_LIMIT_REASONS` (0x690 on Haswell–Skylake,
0x64F on Meteor Lake+) each cycle and logs currently active CPU throttle
reasons as `cpu-throttle:` in the periodic log line (e.g.,
`cpu-throttle: Core/Cache` when PP0 core budget is capped). Rising-edge
events are accumulated per reason and reported on exit.

Bit layout (lower 16 = current status): PROCHOT(0), Thermal(1),
Current(EDP)(2), Power(PL1)(3), Platform(4), Autonomous(5),
VR_Temp(6), HTC(7), Core/Cache(8), Amps(9), PROCHOT_Deassert(10),
PL4/Peak(11), PkgPwrLatch(12), Clipping(13).

PROCHOT (bit 0) is excluded from the `cpu-throttle:` line and from
aggregation — see PROCHOT handling below.

### PROCHOT handling

Many laptop ECs assert the PROCHOT# signal aggressively, even at low
temperatures and idle power.  The daemon disables the CPU's response to
external PROCHOT# by clearing MSR 0x1FC (MSR_POWER_CTL) bit 0 (BD_PROCHOT)
at startup.  This MSR write is persistent (the EC/firmware does not
re-enable this bit between reads), so a one-time clear is sufficient.
The original MSR value is saved on startup and restored on exit.

NOTE: PROCHOT# assertion detection, charger-adequacy checking, and
the weak-charger power-save mode are **not yet implemented** in the
daemon code. They are documented as planned future features.

### CPU controls

| Control | Mechanism | Notes |
|---------|-----------|-------|
| Frequency cap | `intel_pstate/max_perf_pct` | Scaled by GPU-first CPU budget; floor at 20% |
| Turbo | `intel_pstate/no_turbo` | Disabled when GPU is throttling or CPU demand ratio > 0.7 |
| EPP | `energy_performance_preference` per CPU | Per-cluster with hysteresis: E-cores get one tier less aggressive EPP than P-cores when GPU is not throttling. P-cores cycle through `balance_performance` → `balance_power` → `power`. EPP changes use hysteresis (0.15 ratio margin) to prevent flip-flopping. The `performance` EPP is never set.
| RAPL PP0 | `constraint_0_power_limit_uw` | Core budget derived from PL1 minus GPU draw |
| CPU hotplug | core offlining via `cpu/online` | P-cores offlined first (keeping at least 2), then E-cores |

All CPU controls are saved on startup and restored on exit (SIGINT/SIGTERM/SIGHUP).

### CPU hotplug (core offlining)

When CPU power demand is low, the daemon offlines entire physical cores to
eliminate leakage and switching power, freeing that power for the GPU.
Cores are grouped by physical core (HT siblings share a group).  The
offlining priority is:

1. **P-cores** offlined first (highest CPU number first)
2. **E-cores** offlined next (highest CPU number first)
3. **At least `min_core_groups` (2) groups** are always kept online

The number of groups to keep is determined by the ratio of CPU power budget
to CPU reference power (computed by `choose_keep_groups()`):

| CPU power ratio | Groups kept |
|----------------|-------------|
| > 0.9 (CPU saturated) | All groups (0 = no offlining) |
| 0.7 – 0.9 | ≤ 12 groups |
| 0.5 – 0.7 | ≤ 8 groups |
| 0.3 – 0.5 | ≤ 4 groups |
| < 0.3 | 2 groups (min_core_groups floor) |

Changes are applied with a 20-cycle settle period (~10 s).  `min_perf_pct`
is dropped to 0 when any cores are offlined so remaining idle cores reach
minimum frequency and deepest C-states.  Initial online state is saved on
startup and restored on exit.

### Hardware discovery

All hardware paths are discovered dynamically at startup:

- **RAPL**: scans `intel-rapl` and `intel-rapl-mmio` under
  `/sys/class/powercap/`, discovers PP0 (core) and PP1 (uncore) subdomains
- **GPU**: scans `/sys/class/drm/card*/device/gt*/freq0/cur_freq` for
  the Xe driver (gt0 = render, gt1 = media)
- **Temperature**: scans `/sys/class/hwmon/` for the `coretemp` driver
- **CPU count**: enumerates `/sys/devices/system/cpu/cpu*`

Works on machines with or without a GPU (GPU-only mode).

## Usage

### Build

```sh
g++ -std=c++17 -O2 src/power-balance.cpp src/power-optimizer.cpp src/power-utils.cpp src/msr_platform.cpp src/scheduler-stats.cpp -o power-balance
```

Or via CMake:

```sh
cmake -B build && cmake --build build
```

### Run

```sh
sudo ./power-balance                        # default PL1 (40 W)
sudo ./power-balance --pl1 35               # override PL1 to 35 W
```

Runs in the foreground, logs to syslog (facility `LOG_DAEMON`).  Send
SIGINT, SIGTERM, or SIGHUP to stop cleanly and restore CPU state.

### systemd service

```sh
sudo cp power-balance /usr/local/bin/
sudo cp systemd/power-balance.service /etc/systemd/system/
sudo systemctl enable --now power-balance
```

Logs are available via `journalctl -u power-balance`.

### Log output

```
[active] pkg=25.4W core=8.0W gpu=12.5W(gpu_sm=11.5W) pl1=40.0W
  core_lmt=24.8W max_perf=62% no_turbo=0 epp=balance_power  temp=77C  
  c0=45% demand=0.58 gpu-throttle: pl4:2
```

```
[throttle] pkg=25.4W core=7.7W gpu=11.3W(gpu_sm=12.1W) pl1=40.0W
  core_lmt=8.0W max_perf=20% no_turbo=1 epp=power(balance_power)  temp=85C  
  demand=0.22 gpu-throttle: pl4:4  cpu-throttle: Core/Cache
```

```
PROCHOT asserted — insufficient charger: battery Not charging; 15W USB-C charger
```

PROCHOT warnings are emitted at `LOG_WARNING` priority when the daemon
detects external PROCHOT# assertion from an inadequate charger.  This is a
one-time ratelimited diagnostic — it does not appear in every periodic line.

Periodic log lines (every 20 iterations = 10 s) include power readings,
applied limits, temperature, GPU throttle event counts (`gpu-throttle:`),
and CPU MSR perf limit reasons (`cpu-throttle:`).  The `hotplug` line shows
core offlining changes.

### power-status

A terminal monitor showing live package/core/uncore power, GPU frequency and
idle state, CPU frequency and EPP per cluster, and core temperatures.
Run with `--help` for options.
