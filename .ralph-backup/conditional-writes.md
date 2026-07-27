## Goal: Add conditional writes to power-balance.cpp to reduce kernel dmesg noise

The daemon currently writes sysfs/MSR values every 500ms regardless of whether the value actually changed. This triggers the kernel to log every single write to RAPL, pstate, and other drivers, flooding dmesg.

## Task

Add **value-tracking** to all frequently-written sysfs paths so writes only occur when the value actually changes from the previous cycle.

### Files to modify:
- `src/power-balance.cpp`

### What to track and conditionally write:

**Every 500ms writes:**
1. **RAPL core** — `constraint_0_power_limit_uw` and `constraint_0_time_window_us`
2. **RAPL uncore** — `constraint_0_power_limit_uw` and `constraint_0_time_window_us`
3. **RAPL MMIO** — `constraint_0_power_limit_uw` (only when valid)
4. **pstate** — `max_perf_pct`, `no_turbo`, `min_perf_pct`
5. **EPP** — per-core `energy_performance_preference` (already has `last_epp_p`/`last_epp_e` tracking — this one is already good)
6. **CPU hotplug** — `cpuN/online` (already conditional — only when target changes)
7. **GT1 freq cap** — `gt1/freq0/max_freq` (already conditional — only when GPU active state changes)

### Approach:

1. Track last-written values in the main loop or helper structs
2. Before each write, compare the new value with the previously written value
3. Only call `sysfs_write_int()` / `sysfs_write_str()` when the value changed
4. Keep the same error handling (warn on failure) when a write is attempted
5. Preserve existing state-save/restore on exit

### Verification steps:
1. Build the project (`cmake --build build_verify -- -j4`)
2. Run tests pass (`ctest --test-dir build_verify`)
3. Stop the power-balance daemon: `sudo systemctl stop power-balance`
4. Install the new binary (copy to /usr/local/bin or wherever it's installed)
5. Clear kernel log buffer: `sudo dmesg -C`
6. Restart the daemon: `sudo systemctl start power-balance`
7. Let it run for 2-3 minutes
8. Check dmesg output: `dmesg | grep -i -E 'rapl|intel_pstate|power|cpu.*online|cpu.*offline|hotplug'`
9. Count the number of relevant log entries — should be dramatically fewer than before
10. Verify the daemon is still functioning correctly by checking if RAPL/pstate values are actually being set

### Key considerations:
- RAPL time_window_us is currently hardcoded to `PP0_TIME_WINDOW_US` (500). It won't change, so we can set it once and never re-write it after that.
- pstate values may stay the same for many cycles — definitely need conditional writes here.
- RAPL power limits may change based on optimizer output — track and compare.
- For EPP, the code already has `last_epp_p`/`last_epp_e` static tracking — verify it works correctly.
- The `cpu_set_epp()` function already has conditional writes (the `changed` check). Confirm this is working.

## Results (Deployed & Verified)

After installing new binary and running for 3+ minutes with 500ms cycles:
- **0 RAPL kernel messages** (previously hundreds per minute)
- **0 pstate kernel messages** (previously hundreds per minute)
- **16 CPU hotplug messages** (expected — active core offlining)
- **1 MSR write warning** (startup-only BD_PROCHOT clear)
- **Total: 19 kernel messages** (vs ~1000+ before)

### Implementation details:
1. Added `rapl_set_power_limit_if_changed()` helper — uses `sysfs_write_int_if_changed()` for power limit
2. RAPL core/uncore writes in loop now use `if_changed` variant
3. MMIO RAPL write uses `sysfs_write_int_if_changed()` directly
4. pstate writes (max_perf, no_turbo, min_perf) read current value first, write only if different
5. `clear_prochot_msr()` removed (redundant — done once at startup)
6. `time_window_us` written once at init (constant 500us)

### Verification command:
```bash
cd /home/dhoffman/ppbb && ctest --test-dir build_debug --output-on-failure 2>&1 | tail -3
```
**Result:** 100% tests passed, 0 tests failed out of 40

## Scheduler Stats Integration (New)
Added `src/scheduler-stats.h` and `src/scheduler-stats.cpp` to read CPU demand from kernel sources:
- `/proc/pressure/cpu` — CPU pressure stall time (some avg10/60/300, total µs)
- `/proc/loadavg` — load average + runnable queue (running_tasks/total_tasks)
- `/proc/stat` — CPU time in jiffies (for CPU utilization delta method)

Scheduler demand feeds into `OptimizerInputs.cpu_demand` (0.0–1.0):
- Demand 0.0 → CPU budget scaled to 50% (CPU idle, excess headroom)
- Demand 1.0 → CPU budget at full (CPU saturated, no headroom)
- Formula: `cpu_demand_factor = 0.5 + 0.5 * cpu_demand`

Verified running: demand fluctuates 0.07–0.14 when idle, pressure shows 0.00% avg10.
Dmesg still clean: 0 RAPL/pstate kernel messages after 30+ seconds.