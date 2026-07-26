// scheduler-stats.h — Kernel scheduler statistics collector
//
// Collects CPU demand signals from the Linux scheduler to determine how much
// headroom the CPU actually needs. When the CPU is demand-constrained, it has
// excess power headroom; when demand is low, the CPU doesn't need much power.
//
// Sources:
//   /proc/pressure/cpu      — CPU pressure stall time (some/full avg10/60/300)
//   /proc/loadavg           — load average + runnable queue
//   /proc/stat              — CPU time in jiffies, procs_running
//
// Design notes:
//   - All values are read as text (no libperf or ftrace dependency)
//   - CPU pressure "some" is the primary demand signal: it says "at least one
//     CPU was stalled because it had work but couldn't make progress."
//   - The "full" signal is when ALL CPUs were stalled — very rare on a
//     hybrid system with GPU doing useful work.
//   - Pressure avg10 gives a short-term (10s) demand estimate.
//   - Pressure avg300 gives a longer-term trend.

#pragma once

#include <string>
#include <cstdint>

// CPU scheduler demand — computed from kernel proc filesystem sources
struct SchedulerDemand {
    // CPU pressure stall time (proc/pressure/cpu)
    // "some" = fraction of time at least one CPU was demand-constrained
    // Values are percentages (0.0–100.0), NOT hundredths.
    double pressure_some_avg10  = 0.0;   // 10s window
    double pressure_some_avg60  = 0.0;   // 60s window
    double pressure_some_avg300 = 0.0;   // 300s window
    long long pressure_total_us = 0;     // cumulative pressure microseconds

    // "full" = fraction of time ALL CPUs had busy runqueues (system saturated)
    double pressure_full_avg10  = 0.0;   // 10s window
    double pressure_full_avg60  = 0.0;   // 60s window
    double pressure_full_avg300 = 0.0;   // 300s window
    long long pressure_full_total_us = 0;

    // Load average and queue state
    double load_avg1     = 0.0;  // 1-minute load average
    double load_avg5     = 0.0;  // 5-minute load average
    double load_avg15    = 0.0;  // 15-minute load average
    int    running_tasks = 0;    // tasks currently in runnable state
    int    total_tasks   = 0;    // total tasks on the system

    // CPU utilization from /proc/stat (jiffies-based)
    // Computed as (total - idle) / total * 100 over a measurement window
    double cpu_active_pct = 0.0;  // system-wide CPU active percentage
    double cpu_iowait_pct = 0.0;  // system-wide I/O wait percentage
    bool   cpu_measured   = false; // true if we got a valid measurement

    // Derived: effective CPU demand (0.0–1.0)
    // 0.0 = no demand (CPU idle, plenty of headroom)
    // 1.0 = maximum demand (CPU fully saturated, no headroom)
    double effective_demand = 0.0;
};

// Read all scheduler demand signals from kernel sources.
// Requires no special permissions, just read access to /proc.
// Uses inter-cycle /proc/stat deltas (no sleep) for CPU utilization.
SchedulerDemand read_scheduler_demand();

// Reset the cached /proc/stat sample (call after long sleeps or boot).
void reset_scheduler_stats();
