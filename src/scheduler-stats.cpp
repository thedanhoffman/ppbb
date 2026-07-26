// scheduler-stats.cpp — Kernel scheduler statistics collector
//
// Reads CPU demand signals from /proc to feed into the power optimizer.
// All sources are available without special permissions (just read access).

#include "scheduler-stats.h"
#include "power-utils.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

// Parse a "/proc/pressure/cpu" line:
//   some avg10=0.00 avg60=0.05 avg300=0.66 total=706000000
// Values are percentages (0.0–100.0).
static void parse_pressure_line(const char* line,
                                 double& out_some10, double& out_some60,
                                 double& out_some300, long long& out_total) {
    out_some10 = 0; out_some60 = 0; out_some300 = 0; out_total = 0;

    char buf[512];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* p = buf;
    while (*p) {
        // Skip whitespace
        while (*p == ' ') p++;
        if (!*p) break;

        // Parse key=value
        char* key_start = p;
        while (*p != '=' && *p != '\0' && *p != '\n') p++;
        if (*p != '=') break;
        *p = '\0';
        p++;

        char* val = p;
        while (*p && *p != ' ' && *p != '\n' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }

        if (strcmp(key_start, "avg10") == 0)
            out_some10 = strtod(val, nullptr);
        else if (strcmp(key_start, "avg60") == 0)
            out_some60 = strtod(val, nullptr);
        else if (strcmp(key_start, "avg300") == 0)
            out_some300 = strtod(val, nullptr);
        else if (strcmp(key_start, "total") == 0)
            out_total = strtoll(val, nullptr, 10);
    }
}

// Parse /proc/loadavg: "1.45 1.55 1.49 3/1186 2482135"
static void parse_loadavg(const char* line,
                           double& out_avg1, double& out_avg5, double& out_avg15,
                           int& out_running, int& out_total) {
    out_avg1 = out_avg5 = out_avg15 = 0;
    out_running = out_total = 0;

    char buf[256];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* tok = strtok(buf, " \t");
    if (!tok) return;
    out_avg1 = strtod(tok, nullptr);

    tok = strtok(nullptr, " \t");
    if (tok) out_avg5 = strtod(tok, nullptr);

    tok = strtok(nullptr, " \t");
    if (tok) out_avg15 = strtod(tok, nullptr);

    tok = strtok(nullptr, " \t/");
    if (tok) out_running = atoi(tok);
    tok = strtok(nullptr, " \t/");
    if (tok) out_total = atoi(tok);
}

// Compute CPU utilization from two /proc/stat samples taken at time t1 and t2.
// Returns true if successful, sets cpu_active_pct and cpu_iowait_pct.
static bool compute_cpu_utilization(
    const std::string& sample1, const std::string& sample2,
    double& active_pct, double& iowait_pct)
{
    // Parse: cpu  user nice system idle iowait irq softirq steal [guest guest_nice]
    auto parse_line = [](const std::string& line) -> std::vector<long long> {
        // Find first space, skip "cpu " prefix
        std::string nums;
        auto p = line.find(' ');
        if (p == std::string::npos) return {};
        nums = line.substr(p);

        std::vector<long long> fields;
        std::istringstream iss(nums);
        long long v;
        while (iss >> v) fields.push_back(v);
        return fields;
    };

    auto f1 = parse_line(sample1);
    auto f2 = parse_line(sample2);
    if (f1.size() < 8 || f2.size() < 8) return false;

    long long u1 = f1[0], n1 = f1[1], s1 = f1[2], i1 = f1[3];
    long long w1 = f1[4], q1 = f1[5], SQ1 = f1[6], st1 = f1[7];

    long long u2 = f2[0], n2 = f2[1], s2 = f2[2], i2 = f2[3];
    long long w2 = f2[4], q2 = f2[5], SQ2 = f2[6], st2 = f2[7];

    long long du = u2 - u1, dn = n2 - n1, ds = s2 - s1, di = i2 - i1;
    long long dw = w2 - w1, dq = q2 - q1, dSQ = SQ2 - SQ1, dst = st2 - st1;

    long long total = du + dn + ds + di + dw + dq + dSQ + dst;
    if (total <= 0) return false;

    active_pct = 100.0 * (total - di) / total;
    iowait_pct = 100.0 * dw / total;
    return true;
}

// Cached /proc/stat sample for inter-cycle delta (no sleep needed)
static std::string g_prev_stat_line;
static bool        g_stat_initialized = false;

// Read one line from a file, stripping newline.
static std::string read_one_line(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string line;
    std::getline(f, line);
    // Strip trailing whitespace/newline
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
        line.pop_back();
    return line;
}

void reset_scheduler_stats() {
    g_prev_stat_line.clear();
    g_stat_initialized = false;
}

SchedulerDemand read_scheduler_demand() {
    SchedulerDemand sd;

    // ── /proc/pressure/cpu ──
    {
        std::ifstream pf("/proc/pressure/cpu");
        if (pf) {
            std::string some_line, full_line;
            std::getline(pf, some_line);
            std::getline(pf, full_line);
            if (!some_line.empty()) {
                parse_pressure_line(some_line.c_str(),
                                    sd.pressure_some_avg10,
                                    sd.pressure_some_avg60,
                                    sd.pressure_some_avg300,
                                    sd.pressure_total_us);
            }
            // Also parse "full" line: fraction of time ALL CPUs had busy runqueues.
            // This is a strong signal for "system needs more cores".
            if (!full_line.empty()) {
                parse_pressure_line(full_line.c_str(),
                                    sd.pressure_full_avg10,
                                    sd.pressure_full_avg60,
                                    sd.pressure_full_avg300,
                                    sd.pressure_full_total_us);
            }
        }
    }

    // ── /proc/loadavg ──
    {
        std::string line = read_one_line("/proc/loadavg");
        if (!line.empty()) {
            parse_loadavg(line.c_str(),
                          sd.load_avg1, sd.load_avg5, sd.load_avg15,
                          sd.running_tasks, sd.total_tasks);
        }
    }

    // ── /proc/stat: CPU utilization (inter-cycle delta, no sleep) ──
    {
        std::string stat_line = read_one_line("/proc/stat");
        if (!stat_line.empty() && stat_line.substr(0, 4) == "cpu ") {
            if (g_stat_initialized) {
                sd.cpu_measured = compute_cpu_utilization(g_prev_stat_line, stat_line,
                                                           sd.cpu_active_pct,
                                                           sd.cpu_iowait_pct);
            }
            g_prev_stat_line = stat_line;
            g_stat_initialized = true;
        }
    }

    // ── /proc/stat: procs_running ──
    {
        std::ifstream sf("/proc/stat");
        if (sf) {
            std::string line;
            while (std::getline(sf, line)) {
                if (line.find("procs_running") == 0) {
                    sd.running_tasks = atoi(line.c_str() + 14);
                }
            }
        }
    }

    // ── Derive effective demand ──
    // Primary signal: PSI "some" avg10 — fraction of time at least one task
    // was waiting for CPU. This correctly captures "tasks need cores" regardless
    // of how many cores are already busy. (A single-threaded workload at 100% on
    // one core shows high "some" pressure, but low "full" pressure → it doesn't
    // need more cores.)
    //
    // Secondary signal: PSI "full" avg10 — fraction of time ALL CPUs had busy
    // runqueues. Even small amounts indicate genuine saturation → boost demand.
    //
    // We intentionally do NOT blend with /proc/stat global utilization, because
    // that averages across all CPUs and dilutes per-core reality.
    //
    // Map "some" pressure to [0, 1]:
    //   pressure  0–2%  → demand ~0.0–0.3 (idle or near-idle)
    //   pressure  2–10% → demand ~0.3–0.7 (moderate load)
    //   pressure >10%  → demand ~0.7–1.0 (saturated)
    double pressure = sd.pressure_some_avg10;
    if (pressure < 2.0)
        sd.effective_demand = pressure / 2.0 * 0.3;
    else if (pressure < 10.0)
        sd.effective_demand = 0.3 + (pressure - 2.0) / 8.0 * 0.4;
    else
        sd.effective_demand = std::min(1.0, 0.7 + (pressure - 10.0) / 20.0 * 0.3);

    // Boost from "full" pressure: system-wide saturation is a strong signal.
    // Even 1% full pressure means ALL CPUs were busy → don't offline.
    if (sd.pressure_full_avg10 > 0.1) {
        double full_boost = std::min(0.5, sd.pressure_full_avg10 / 50.0);
        sd.effective_demand = std::min(1.0, sd.effective_demand + full_boost);
    }

    return sd;
}
