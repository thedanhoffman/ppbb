# power-balance

GPU-first power balancer daemon for Intel hybrid architectures (Meteor Lake
and similar).  The **GPU must never throttle** — the CPU is the expendable
buffer.  The daemon constrains CPU frequency, turbo, RAPL budgets, and EPP so
the GPU always has full power headroom, while independently capping CPU
temperature.

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
| **throttle** (2) | Any GPU throttle flag active | CRITICAL — hammer the CPU: max_perf=20%, no_turbo=1, EPP=`power`, core RAPL budget capped at 8 W |

### RAPL budget allocation

The daemon sets uncore (PP1) budget to **0** (unlimited — GPU gets what it
needs).  Core (PP0) budget is:

```
core_budget = PL1 − gpu_draw − headroom_margin
```

On Meteor Lake the MMIO RAPL domain (`intel-rapl-mmio`) is also kept in sync
with the MSR PL1 so it does not become a hidden bottleneck.

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
accumulated in counters, printed in periodic log lines, and summarized on
exit.  Throttle detection is suppressed for the first 3 cycles to let RAPL
counters settle.

### CPU controls

| Control | Mechanism | Notes |
|---------|-----------|-------|
| Frequency cap | `intel_pstate/max_perf_pct` | Scaled by aggression level and temperature |
| Turbo | `intel_pstate/no_turbo` | Forced off when GPU draws >15 W, GPU throttling, or temp ≥82°C |
| EPP | `energy_performance_preference` per CPU | Cycles through `balance_performance` / `balance_power` / `power` — never `performance` |
| RAPL PP0 | `constraint_0_power_limit_uw` | Core budget derived from PL1 minus GPU draw |

All CPU controls are saved on startup and restored on exit (SIGINT/SIGTERM/SIGHUP).

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
sudo ./power-balance                        # use sysfs PL1
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
[active] pkg=34.5W core=10.4W gpu=18.9W(gpu_sm=18.6W) pl1=28.0W 
  core_lmt=7.4W max_perf=50% no_turbo=1 epp=balance_power  temp=85C
```

Periodic log lines (every 20 iterations = 10 s) include power readings,
applied limits, and temperature.

### power-status

A terminal monitor showing live package/core/uncore power, GPU frequency and
idle state, CPU frequency and EPP per cluster, and core temperatures.
Run with `--help` for options.
