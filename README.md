# power-balance

GPU-first power balancer daemon for Intel hybrid architectures (Meteor Lake
and similar).  The **GPU must never throttle** — the CPU is the expendable
buffer.  The daemon constrains CPU frequency, turbo, RAPL budgets, EPP, and
CPU core topology so the GPU always has full power headroom, while
independently capping CPU temperature.

Also includes `power-status` — a terminal power monitor (self-explanatory:
`power-status --help`).

## How it works

### Aggression levels

Every 500 ms the daemon reads RAPL energy counters for the package, core
(PP0), and uncore (PP1) domains, computes instantaneous power draw, and
classifies GPU state as one of three aggression levels:

| Level | Trigger | Effect |
|-------|---------|--------|
| **idle** (0) | GPU in RC6, draw below 3 W | Relax all CPU controls: max_perf=100%, no_turbo=0, EPP=`balance_performance` |
| **active** (1) | GPU out of RC6 or draw above 3 W | Moderate CPU back-off: max_perf scales inversely with GPU power (50% minimum), turbo disabled above 15 W GPU draw, EPP=`balance_power` |
| **throttle** (2) | Any GPU throttle flag active (excluding PL4) | CRITICAL — hammer the CPU: max_perf=20%, no_turbo=1, EPP=`power`, core RAPL budget capped at 8 W |

GPU PL4 (peak current) events are **not** treated as a throttle trigger —
they are the GPU tile's normal internal peak current management handled
transparently by the GuC firmware.  PL4 events are still tracked and logged
but do not escalate aggression.

### RAPL budget allocation

The daemon sets uncore (PP1) budget to **0** (unlimited — GPU gets what it
needs).  Core (PP0) budget is:

```
core_budget = PL1 − gpu_draw − headroom_margin
```

On Meteor Lake the MMIO RAPL domain (`intel-rapl-mmio`) is also kept in sync
with the MSR PL1 so it does not become a hidden bottleneck.

### Package PL1

The daemon raises the package PL1 to a configurable value at startup (default
40 W).  While the declared `max_power_uw` may be lower (e.g., 28 W), the
hardware VR accepts higher values.  Raising PL1 gives the GPU tile's voltage
regulator more peak current headroom, dramatically reducing GPU PL4 (peak
power) events.

### Temperature-aware overlay

The daemon independently reads the hottest core temperature from `coretemp`
and applies a second set of constraints that override the aggression-based
values when the CPU is hot.  The most conservative value always wins.

| Temp | `max_perf` cap | Turbo | EPP | Extra headroom |
|------|---------------|-------|-----|----------------|
| ≤70°C | 100% (no cap) | — | (aggression-based) | 0 W |
| 70→80°C | 100→60% (linear) | — | — | 0→2.5 W |
| 80→82°C | 60→52% | — | `balance_power` | 2.5→3 W |
| 82→85°C | 52→40% | forced off | `balance_power` | 3→3.75 W |
| 85→90°C | 40→20% | forced off | forced `power` | 3.75→5 W |
| ≥90°C | 20% (floor) | forced off | forced `power` | 5 W |

The extra headroom increases the PL1 margin, starving the core budget so
package power drops and the CPU cools.

### GPU throttle monitoring

The daemon watches the Xe driver's 8 throttle reason flags (pl1, pl2, pl4,
prochot, thermal, ratl, vr_tdc, vr_thermalert).  Rising-edge events are
accumulated in counters, printed in periodic log lines as `gpu-throttle:`,
and summarized on exit.  Throttle detection is suppressed for the first 3
cycles to let RAPL counters settle.

PL4 is **excluded from aggression detection** — it is the GPU tile's normal
peak current management handled by the GuC firmware.  All 8 reasons are
still tracked and logged.

### GPU power profile

The daemon forces both GPU tiles (gt0 render, gt1 media) to the `base`
power profile at startup, overriding the `power_saving` default.  The
original profile is saved and restored on exit.  GPU performance is
non-negotiable — all CPU controls are the expendable variable.

### CPU MSR perf limit monitoring

The daemon reads MSR 0x6B0 (Package Perf Limit Reasons) each cycle and logs
currently active CPU throttle reasons as `cpu-throttle:` in the periodic log
line (e.g., `cpu-throttle: Core/Cache` when PP0 core budget is capped).
Rising-edge events are accumulated per reason and reported on exit.

PROCHOT (bit 0) is excluded from the `cpu-throttle:` line and from
aggregation — see PROCHOT handling below.

### PROCHOT handling

Many laptop ECs assert the PROCHOT# signal aggressively, even at low
temperatures and idle power.  On this Acer Meteor Lake platform the EC
asserts PROCHOT# at ~44°C / 4 W due to an insufficient charger (see below).

The daemon disables the CPU's response to external PROCHOT# by clearing
MSR 0x1FC bit 0 every cycle.  The EC/firmware continuously re-enables this
bit, so a one-time clear at startup is not sufficient — the daemon must
rewrite it each cycle.  The original MSR value is saved on startup and
restored on exit.

When PROCHOT# assertion is detected in MSR 0x6B0, the daemon checks the
power supply subsystem for signs of an inadequate charger:

- Battery **Discharging** or **Not charging** while AC is online
- USB-C Power Delivery contract below 45 W

If a weak or insufficient charger is identified, a `LOG_WARNING` is emitted
(ratelimited to once per ~60 s):

```
PROCHOT asserted — insufficient charger: battery Not charging; 15W USB-C charger
```

This gives the user a direct, actionable diagnostic instead of silently
tolerating performance loss from a power-limited charge source.  The daemon
continues to clear the PROCHOT# response regardless, so normal operation
is unaffected while the warning is displayed.

### Weak charger power-save mode

When PROCHOT# from an insufficient charger is detected, the daemon goes a
step further than just tolerating it: it enters **power-save** mode and
aggressively reduces system power draw to stay within the charger's capacity.

The power-save mode applies every available control simultaneously:

| Control | Setting |
|---------|---------|
| Package PL1 | Lowered to ~70% of charger capacity (e.g., 10.5 W for a 15 W USB-C charger) |
| Turbo | Forced off (`no_turbo=1`) |
| `max_perf_pct` | Capped to 40% |
| EPP (all cores) | Forced to `power` |
| `min_perf_pct` | Forced to 0 for deepest idle states |
| GPU `max_freq` | gt0 capped to 900 MHz, gt1 capped to 600 MHz |
| Uncore (PP1) budget | Capped to 3 W (instead of unlimited) |
| CPU hotplug | Keep only 4 core groups (~8 threads) |

The charger is checked every 10 seconds.  When an adequate charger is
detected (e.g., switching from USB-C to the barrel connector), all controls
are restored to their normal aggression/temperature-derived values.

The `[power-save]` state is displayed in the log state tag:

```
[power-save] pkg=3.6W core=1.2W gpu=0.1W(gpu_sm=0.1W) pl1=10.5W core_lmt=8.4W max_perf=40% no_turbo=1 epp=power  temp=49C  gpu-throttle: prochot:1
```

A transition message is logged on entry and exit:

```
weak charger: 15W USB-C charger — enabling power saving
adequate charger detected — disabling power saving
```

### CPU controls

| Control | Mechanism | Notes |
|---------|-----------|-------|
| Frequency cap | `intel_pstate/max_perf_pct` | Scaled by aggression level and temperature |
| Turbo | `intel_pstate/no_turbo` | Forced off when GPU draws >15 W, GPU throttling, or temp ≥82°C |
| EPP | `energy_performance_preference` per CPU | Per-cluster: P-cores throttled first, E-cores 1 tier less aggressive. Cycles through `balance_performance` / `balance_power` / `power` — never `performance` |
| RAPL PP0 | `constraint_0_power_limit_uw` | Core budget derived from PL1 minus GPU draw |
| CPU hotplug | core offlining via `cpu/online` | P-cores offlined first (keeping at least 2), then E-cores |

All CPU controls are saved on startup and restored on exit (SIGINT/SIGTERM/SIGHUP).

### CPU hotplug (core offlining)

In heavy/throttling states the daemon offlines entire physical cores to
eliminate leakage and switching power.  Cores are grouped by physical core
(HT siblings share a group).  The offlining priority is:

1. **P-cores** offlined first (highest CPU number first)
2. **E-cores** offlined next (highest CPU number first)
3. **CPU0's group and at least 2 P-core groups** are always kept online

The number of groups to keep depends on aggression and temperature:

| State | Groups kept |
|-------|-------------|
| idle/cool (<70°C) | All groups |
| moderate (active, 70–80°C) | 12 groups |
| heavy (GPU >15 W, 80–85°C) | 8 groups |
| throttle (GPU throttling, 85–90°C) | 4 groups |
| critical (aggression≥2, ≥90°C) | 1 group (P-core group with CPU0) |

Changes are throttled with a 20-cycle settle period (~10 s).  `min_perf_pct`
is dropped to 0 when any cores are offlined so remaining idle cores reach
minimum frequency and deepest C-states.  Initial online state is saved on
startup and restored on exit.

### Hardware discovery

All hardware paths are discovered dynamically at startup:

- **RAPL**: scans `intel-rapl` and `intel-rapl-mmio` under
  `/sys/class/powercap/`, discovers PP0 (core) and PP1 (uncore) subdomains
- **GPU**: scans `/sys/class/drm/card*/device/tile0/gt0/freq0/cur_freq` for
  the Xe driver
- **Temperature**: scans `/sys/class/hwmon/` for the `coretemp` driver
- **CPU count**: enumerates `/sys/devices/system/cpu/cpu*`

Works on machines with or without a GPU (GPU-only mode).

## Usage

### Build

```sh
g++ -std=c++17 -O2 src/power-balance.cpp -o power-balance
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
  gpu-throttle: pl4:2
```

```
[balance-throttle] pkg=25.4W core=7.7W gpu=11.3W(gpu_sm=12.1W) pl1=40.0W 
  core_lmt=8.0W max_perf=20% no_turbo=1 epp=power(balance_power)  temp=85C  
  gpu-throttle: pl4:4  cpu-throttle: Core/Cache
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
