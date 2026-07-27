// power-balance.cpp — GPU-first power balancer daemon
// Principle: the GPU must never throttle. Everything else (CPU frequency,
// RAPL limits, turbo, EPP) is aggressively constrained to ensure the GPU
// always has the power headroom it needs.
//
// Control logic uses resource-domain.h (generic solver) for power allocation
// and control decisions. Platform-specific actuation via intel-actuator.h.
// Compile: cmake -B build && cmake --build build

#include "power-utils.h"
#include "msr_platform.h"
#include "scheduler-stats.h"
#include "resource-domain.h"   // generic solver: ResourceResult, EppLevel
#include "intel-actuator.h"    // Intel platform actuator
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <syslog.h>

// ── Configuration ──
static constexpr double   DEFAULT_PL1_W        = 40.0;
static constexpr int      INTERVAL_MS          = 200;
// GPU activity tracking: C0 residency (c0_pct 0.0–1.0) over 500ms window
// Drives solver weight scaling: idle/active/heavy/throttling.
//
// GT1 power minimization (TODO #3 done):
//   GT0 (graphics) profile switched via set_gt0_profile(): base (active) / power_saving (idle)
//   GT1 (media) locked in power_saving via set_gt1_profile() — always kept idle
//   saved_profile_gt1 tracks GT1's original profile for restore on exit
//
// Dynamic RAPL domains (TODO #2 done):
//   all_rapl_domains tracks every discovered subdomain (dram, pp0, etc.)
//   Core budget applied to all non-uncore domains; uncore left unlimited
static constexpr long     PP0_TIME_WINDOW_US   = 500;

// ── Data-only structs ──
// All runtime state lives in one global (SystemState).  Each sub-struct owns
// its own data; free functions below operate on the structs by reference.
// RAPL domain type is the shared RaplDomain from power-utils.h.

// GPU throttle event tracking
struct GpuThrottleCounters {
    int events[8] = {0};
    int total_events = 0;
    int cycles_throttled = 0;
    bool prev_state[8] = {false};
};

// CPU perf-limit event tracking (MSR: 0x690 Haswell-Skylake, 0x64F Meteor Lake+)
struct PerfLimitCounters {
    int events[16] = {0};
    unsigned int prev_current = 0;
};

// GPU state: paths, saved values, throttle tracking
struct GpuState {
    std::string gt0;               // /sys/class/drm/cardN/device/tile0/gt0
    std::string gt1;               // /sys/class/drm/cardN/device/tile0/gt1
    std::string idle_path;         // gt0/gtidle/idle_status
    std::string saved_profile_gt0;
    std::string saved_profile_gt1; // saved before any profile changes (for GT1)
    int         saved_max_freq_gt0 = -1;
    int         saved_max_freq_gt1 = -1;
    int         min_freq_gt1        = -1;  // GT1 min_freq for active-state cap

    // GT1 power minimization (TODO #3)
    // GT1 (media engine) is kept in power_saving at all times — it's a
    // major contributor to GT PL1/PL2/PL4 throttling events.
    bool        gt1_force_power_saving = true;
    bool        gt1_freq_capped        = false;  // true when max_freq capped to min_freq

    // C0 residency-based GPU activity tracking (TODO #1)
    // Uses residency percentage instead of power thresholds for
    // portability across GPUs.
    bool        has_c0_residency = false;  // true when c0_residency_path is populated
    std::string c0_residency_path; // gt0/activity/c0_residency_ms
    long long   last_c0_residency_us = 0; // microseconds, last read value
    long long   _last_c0_time_us = 0;     // steady_clock time of last read (internal)
    double      c0_pct = 0.0;              // 0.0–1.0, residency fraction over measurement window
    bool        gt0_profile_active = false; // true when base profile is set
    int         gt0_debounce = 0;           // cycles stable before switching profile
};
// Debounce: GT0 power_profile switches only after this many consecutive
// cycles (× 200ms) showing the same active/idle state.  Prevents rapid
// toggling when GPU flickers between c6 and active.
static constexpr int GT0_PROFILE_DEBOUNCE_CYCLES = 10;  // 2 s

// ── GT1 frequency cap ──
// When the GPU is actively being used, cap GT1 (media engine) max_freq to
// its min_freq.  GT1 is a major contributor to GT-level PL1/PL2/PL4
// throttling events — brief media decode bursts can spike GT power and
// trigger throttling that affects GT0 (render).  Capping max_freq to
// min_freq prevents these bursts while keeping GT1 functional for
// background decode tasks.

// Physical core group (for hotplug)
struct CoreGroup {
    int         id;              // group index
    bool        is_pcore;
    bool        has_ht;          // true if this group has HT siblings
    std::vector<int> cpus;       // logical CPU numbers in this group
    int         priority;        // higher = offline first
    bool        saved_online;    // initial state (for restore)
};

// CPU state: EPP paths, core groups, current settings
struct CpuState {
    // EPP paths — discovered by cluster type
    std::vector<std::string> pcore_epp_paths;
    std::vector<std::string> ecore_epp_paths;
    // Core groups
    std::vector<CoreGroup>   core_groups;
    int    keep_p_target = 0;  // last applied P-core target
    int    keep_e_target = 0;  // last applied E-core target
    int    hotplug_settle = 0;
};

// Thermal state: sensor paths, saved powerclamp state
struct ThermalState {
    std::string coretemp_dir;
    std::string powerclamp_dev;
    int saved_powerclamp_state = 0;
};

// Saved state for restore on exit
struct SavedState {
    int    max_perf      = 100;
    int    min_perf      = 8;
    int    no_turbo      = 0;
    std::string epp;
    unsigned long long msr_1fc = 0;
};

// All runtime state — the single top-level global
struct SystemState {
    RaplDomain  pkg;
    RaplDomain  pkg_mmio;
    RaplDomain  core;
    RaplDomain  uncore;
    // All RAPL domains discovered (TODO #2: enables dram, pp0, etc.)
    std::vector<RaplDomain> all_rapl_domains;
    GpuState    gpu;
    CpuState    cpu;
    ThermalState thermal;
    SavedState  saved;
};

// ── Throttle/limit reason tables ──
// ── GT1 frequency cap ──
// When the GPU is actively being used, cap GT1 (media engine) max_freq to
// its min_freq.  GT1 is a major contributor to GT-level PL1/PL2/PL4
// throttling events — brief media decode bursts can spike GT power and
// trigger throttling that affects GT0 (render).  Capping max_freq to
// min_freq prevents these bursts while keeping GT1 functional for
// background decode tasks.

static void set_gt1_freq_cap(GpuState& g, bool gpu_active) {
    if (g.gt1.empty()) return;

    if (gpu_active && !g.gt1_freq_capped) {
        // Cap GT1 max_freq to min_freq — prevents media engine bursts from
        // spiking GT power and triggering PL1/PL2/PL4 events on GT0.
        if (g.min_freq_gt1 > 0) {
            sysfs_write_int(g.gt1 + "/freq0/max_freq", g.min_freq_gt1);
            g.gt1_freq_capped = true;
            syslog(LOG_INFO, "GT1 max_freq capped to min_freq (%d MHz) — GPU active", g.min_freq_gt1);
        }
    } else if (!gpu_active && g.gt1_freq_capped) {
        // Restore saved max_freq when GPU is idle.
        if (g.saved_max_freq_gt1 > 0) {
            sysfs_write_int(g.gt1 + "/freq0/max_freq", g.saved_max_freq_gt1);
            g.gt1_freq_capped = false;
        }
    }
}

// ── RAPL helpers ──

static void rapl_set_power_limit(const RaplDomain& d, double watts_w, long long time_us = PP0_TIME_WINDOW_US) {
    if (!d.valid) return;
    sysfs_write_int(d.path + "/constraint_0_power_limit_uw", (long long)(watts_w * 1e6));
    sysfs_write_int(d.path + "/constraint_0_time_window_us", time_us);
}

// Like rapl_set_power_limit() but only writes the power limit when the value changes.
// The time window is written once at init (constant PP0_TIME_WINDOW_US = 500 us).
// This avoids flooding the kernel with RAPL driver log messages every 500 ms cycle.
static void rapl_set_power_limit_if_changed(const RaplDomain& d, double watts_w) {
    if (!d.valid) return;
    long long target_uw = (long long)(watts_w * 1e6);
    sysfs_write_int_if_changed(d.path + "/constraint_0_power_limit_uw", target_uw);
}

static void rapl_set_enabled(const RaplDomain& d, bool on) {
    if (!d.valid) return;
    sysfs_write_file(d.path + "/enabled", on ? "1" : "0");
}

// Set power limit on ALL RAPL domains (pkg excluded — that's PL1, not a subdomain).
// Used during initialization to enable all domains, and during cleanup to reset them.
static void rapl_set_all(const std::vector<RaplDomain>& domains, double watts_w) {
    for (auto& d : domains) {
        double val = (watts_w == 0.0 && d.max_w() > 0) ? d.max_w() : watts_w;
        rapl_set_power_limit(d, val);
    }
}

// Enable all RAPL subdomains.
static void rapl_enable_all(const std::vector<RaplDomain>& domains) {
    for (auto& d : domains) rapl_set_enabled(d, true);
}

// ── RAPL discovery ──
// Uses shared scan_rapl_domains() from power-utils.h to discover all domains,
// then populates daemon-specific typed fields (pkg, core, uncore, etc.).

static bool discover_rapl(SystemState& s) {
    // Scan all RAPL domains using shared function
    std::vector<RaplDomain> all_domains = read_all_rapl_domains();

    for (auto& d : all_domains) {
        // Identify package domains
        if (d.name.find("package-") != std::string::npos) {
            if (d.path.find("intel-rapl-mmio") != std::string::npos) {
                s.pkg_mmio = d;
            } else if (s.pkg.path.empty()) {
                s.pkg = d;
            }
        }
        // Typed subdomains
        else if (d.name == "core") { s.core = d; }
        else if (d.name == "uncore") { s.uncore = d; }
        else {
            s.all_rapl_domains.push_back(d);
        }
    }

    // Mark valid
    s.pkg.valid = !s.pkg.path.empty();
    s.pkg_mmio.valid = !s.pkg_mmio.path.empty();
    s.core.valid = !s.core.path.empty();
    s.uncore.valid = !s.uncore.path.empty();

    // Log discovered domains
    char log_pl4[64] = "";
    if (s.core.pl4_w() > 0)
        snprintf(log_pl4, sizeof(log_pl4), " core_pl4=%.1fW", s.core.pl4_w());
    else if (s.pkg.pl4_w() > 0)
        snprintf(log_pl4, sizeof(log_pl4), " pkg_pl4=%.1fW", s.pkg.pl4_w());
    syslog(LOG_INFO, "RAPL: pkg=%s pkg_mmio=%s core=%s uncore=%s%s",
           s.pkg.path.c_str(),
           s.pkg_mmio.path.empty() ? "n/a" : s.pkg_mmio.path.c_str(),
           s.core.path.c_str(),
           s.uncore.path.empty() ? "n/a" : s.uncore.path.c_str(),
           log_pl4);
    if (!s.all_rapl_domains.empty()) {
        std::string types;
        for (auto& d : s.all_rapl_domains) {
            if (!types.empty()) types += ", ";
            types += d.name;
        }
        syslog(LOG_INFO, "RAPL subdomains: %s", types.c_str());
    }

    return s.pkg.valid;
}

// ── GPU path discovery ──

static void discover_gpu(SystemState& s) {
    DIR* drm = opendir(SYSFS_DRM);
    if (!drm) return;
    struct dirent* de;
    while ((de = readdir(drm)) != nullptr) {
        std::string card = de->d_name;
        if (card.find("card") != 0) continue;
        std::string base0 = "/sys/class/drm/" + card + "/device/tile0/gt0";
        if (sysfs_read_file(base0 + "/freq0/cur_freq") == "") continue;
        s.gpu.gt0 = base0;
        s.gpu.idle_path = base0 + "/gtidle/idle_status";
        // C0 residency path for activity-based thresholds (TODO #1)
        std::string c0_path = base0 + "/activity/c0_residency_ms";
        if (sysfs_read_file(c0_path) != "") {
            s.gpu.c0_residency_path = c0_path;
            s.gpu.has_c0_residency = true;
        }
        std::string base1 = "/sys/class/drm/" + card + "/device/tile0/gt1";
        if (sysfs_read_file(base1 + "/freq0/cur_freq") != "") s.gpu.gt1 = base1;
        break;
    }
    closedir(drm);
}

static bool gpu_is_active(const GpuState& g) {
    if (g.gt0.empty()) return false;
    std::string s = sysfs_read_file(g.idle_path);
    return s.find("c6") == std::string::npos;
}

// Read C0 residency and compute residency percentage over the last measurement window.
// C0 residency is a cumulative counter (microseconds in active state). By comparing
// deltas against elapsed wall-clock time, we get the fraction of time the GPU was active.
static void gpu_read_c0_residency(GpuState& g) {
    if (g.c0_residency_path.empty()) return;
    std::string val = sysfs_read_file(g.c0_residency_path);
    if (val.empty()) return;
    long long cur_us = 0;
    std::istringstream(val) >> cur_us;

    // Store current time as microseconds since epoch for delta computation
    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    if (g.last_c0_residency_us > 0 && g._last_c0_time_us > 0) {
        long long delta_residency_us = cur_us - g.last_c0_residency_us;
        if (delta_residency_us < 0) delta_residency_us = 0; // counter reset
        long long delta_time_us = now_us - g._last_c0_time_us;
        if (delta_time_us <= 0) return;
        // c0_pct = fraction of time spent in C0 (active) during this window
        g.c0_pct = std::min(1.0, (double)delta_residency_us / (double)delta_time_us);
    }
    g.last_c0_residency_us = cur_us;
    g._last_c0_time_us = now_us;
}

// Check GPU throttle reasons and accumulate event counters.
// Returns 1 if any non-power-limit throttle reason is active (i > 3), 0 otherwise.
// The name gpu_get_throttle_state() clarifies this is a status check, not a count.
static int gpu_get_throttle_state(const GpuState& g, GpuThrottleCounters* counters) {
    if (g.gt0.empty()) return 0;
    std::string throttle_dir = g.gt0 + "/freq0/throttle";
    int any = 0;
    for (int i = 0; XE_THROTTLE_FILES[i]; ++i) {
        int v = 0;
        sysfs_read_attr(throttle_dir, XE_THROTTLE_FILES[i], v);
        bool active = (v != 0);
        // indices: 0=pl1, 1=pl2, 2=pl4, 3=prochot — all excluded
        if (active && i > 3) any = 1;
        if (counters && active && !counters->prev_state[i]) {
            counters->events[i]++;
            counters->total_events++;
        }
        if (counters) counters->prev_state[i] = active;
    }
    if (counters && any) counters->cycles_throttled++;
    return any;
}

// Switch GT0 (graphics engine) power profile based on GPU activity.
// Active → "base" (full performance), Idle → "power_saving".
// Debounced: requires GT0_PROFILE_DEBOUNCE_CYCLES consecutive cycles in the
// same state before switching, preventing rapid toggling.
static void set_gt0_profile(GpuState& g, bool gpu_active) {
    if (g.gt0.empty()) return;

    bool want_active = gpu_active;
    if (want_active == g.gt0_profile_active) {
        g.gt0_debounce = 0;  // state matches, reset counter
        return;
    }
    g.gt0_debounce++;
    if (g.gt0_debounce < GT0_PROFILE_DEBOUNCE_CYCLES) return;

    // Switch after N consecutive cycles in the new state
    const char* target = want_active ? "base" : "power_saving";
    std::string path = g.gt0 + "/freq0/power_profile";
    std::string cur = sysfs_read_file(path);
    if (!cur.empty()) {
        std::string target_bracket = "[" + std::string(target) + "]";
        bool already_set = (cur.find(target) != std::string::npos &&
                            cur.find(target_bracket) != std::string::npos);
        if (!already_set) {
            sysfs_write_str(path, target);
            syslog(LOG_INFO, "GT0 power_profile -> %s (gpu %s, %d cycles)",
                    target, want_active ? "active" : "idle", g.gt0_debounce);
        }
    }
    g.gt0_profile_active = want_active;
    g.gt0_debounce = 0;
}

// Apply profile to GT1 (media engine) — only when explicitly requested.
// By default (gt1_force_power_saving=true), GT1 stays in power_saving.
// This function overrides that default for controlled profile changes.
static void set_gt1_profile(const GpuState& g, const std::string& profile) {
    if (g.gt1.empty()) return;
    std::string path = g.gt1 + "/freq0/power_profile";
    std::string cur = sysfs_read_file(path);
    if (cur.empty()) return;
    if (cur.find(profile) == std::string::npos || cur.find("[" + profile + "]") == std::string::npos)
        sysfs_write_str(path, profile);
}

// ── CPU control paths (shared constants from power-utils.h) ──
// PSTATE_DIR, PSTATE_MAX, PSTATE_MIN, PSTATE_NOTURBO are in power-utils.h

// ── EPP management ──
// Track last-set values per-cluster to avoid redundant sysfs writes.
// Static variables survive between calls — only log when values genuinely change.
static std::string last_epp_p = "";
static std::string last_epp_e = "";

static void cpu_set_epp(CpuState& c, const std::string& p_val, const std::string& e_val) {
    if (c.pcore_epp_paths.empty() && c.ecore_epp_paths.empty()) return;

    bool changed = (p_val != last_epp_p) || (e_val != last_epp_e);
    if (!changed) return;

    last_epp_p = p_val;
    last_epp_e = e_val;

    for (auto& p : c.pcore_epp_paths) sysfs_write_str(p, p_val);
    for (auto& p : c.ecore_epp_paths) sysfs_write_str(p, e_val);

    syslog(LOG_INFO, "EPP -> P:%s  E:%s", p_val.c_str(), e_val.c_str());
}

static void cpu_set_epp_all(const CpuState& c, const std::string& val) {
    for (auto& p : c.pcore_epp_paths) sysfs_write_str(p, val);
    for (auto& p : c.ecore_epp_paths) sysfs_write_str(p, val);
}

// ── Cluster discovery ──

static void discover_clusters(CpuState& c) {
    int global_max_freq = 0;
    std::vector<std::pair<std::string, int>> cpus;

    DIR* dir = opendir(SYSFS_CPU_BASE);
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.find("cpu") != 0) continue;
        std::string epp = std::string(SYSFS_CPU_BASE) + "/" + name + "/cpufreq/energy_performance_preference";
        int max_freq = 0;
        if (sysfs_read_file(epp) == "") continue;
        sysfs_read_attr(std::string(SYSFS_CPU_BASE) + "/" + name + "/cpufreq", "cpuinfo_max_freq", max_freq);
        if (max_freq > global_max_freq) global_max_freq = max_freq;
        cpus.push_back({epp, max_freq});
    }
    closedir(dir);

    if (cpus.empty()) return;
    if (global_max_freq == 0) {
        for (auto& cpy : cpus) c.pcore_epp_paths.push_back(cpy.first);
        return;
    }

    int threshold = (int)(global_max_freq * 0.9);
    for (auto& cpy : cpus) {
        if (cpy.second >= threshold)
            c.pcore_epp_paths.push_back(cpy.first);
        else
            c.ecore_epp_paths.push_back(cpy.first);
    }
    syslog(LOG_INFO, "clusters: %zu P-cores, %zu E-cores",
           c.pcore_epp_paths.size(), c.ecore_epp_paths.size());
}

// ── CPU hotplug (core offlining) ──

static int count_online_groups(const CpuState& c) {
    int n = 0;
    for (auto& g : c.core_groups) {
        std::string s = sysfs_read_file(std::string(SYSFS_CPU_BASE) + "/cpu" + std::to_string(g.cpus[0]) + "/online");
        if (s != "0") n++;
    }
    return n;
}

static void discover_topology(CpuState& c) {
    // Build core groups: CPUs sharing the same physical core
    std::map<int, std::vector<int>> core_map;
    DIR* dir = opendir(SYSFS_CPU_BASE);
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.find("cpu") != 0) continue;
        int cpu = -1;
        try { cpu = std::stoi(name.substr(3)); } catch (...) { continue; }
        if (cpu < 0) continue;
        std::string topo = "/sys/devices/system/cpu/" + name + "/topology";
        std::string siblings = sysfs_read_file(topo + "/core_cpus_list");
        if (siblings.empty()) siblings = sysfs_read_file(topo + "/thread_siblings_list");
        // Parse sibling list to find group key — use first CPU in the list
        size_t comma = siblings.find(',');
        size_t dash  = siblings.find('-');
        std::string first = siblings.substr(0, comma != std::string::npos ? comma : (dash != std::string::npos ? dash : siblings.size()));
        int key = 0;
        if (!first.empty()) key = std::stoi(first);
        core_map[key].push_back(cpu);
    }
    closedir(dir);

    // Determine P/E type using max_freq from cluster discovery
    int global_max = 0;
    for (int cpu = 0; cpu < 256; ++cpu) {
        int mf = 0;
        if (!sysfs_read_attr("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq", "cpuinfo_max_freq", mf)) continue;
        if (mf > global_max) global_max = mf;
    }
    int threshold = (int)(global_max * 0.9);

    for (auto& kv : core_map) {
        CoreGroup g;
        g.id = (int)c.core_groups.size();
        g.cpus = kv.second;
        std::sort(g.cpus.begin(), g.cpus.end());
        g.has_ht = g.cpus.size() > 1;
        // P/E classification: primary = has_ht (P-cores have HT on Meteor Lake).
        // Fallback = check cpuinfo_max_freq when cpufreq is available.
        g.is_pcore = g.has_ht;
        if (!g.is_pcore) {
            int mf = 0;
            std::string cpufreq_dir = "/sys/devices/system/cpu/cpu" + std::to_string(g.cpus[0]) + "/cpufreq";
            if (sysfs_read_attr(cpufreq_dir, "cpuinfo_max_freq", mf))
                g.is_pcore = (mf >= threshold);
        }
        // Priority: P-cores first (offlined first), then E-cores.
        // Within each class, higher CPU number = offlined first.
        // At least 2 P-cores are always kept (enforced in apply_hotplug).
        g.priority = g.is_pcore ? (1000 + g.cpus[0]) : g.cpus[0];
        g.saved_online = true;
        c.core_groups.push_back(g);
    }

    // Sort by priority descending (highest = offline first)
    std::sort(c.core_groups.begin(), c.core_groups.end(),
        [](const CoreGroup& a, const CoreGroup& b) { return a.priority > b.priority; });

    // Save initial online state
    for (auto& g : c.core_groups) {
        std::string online_str = sysfs_read_file("/sys/devices/system/cpu/cpu" + std::to_string(g.cpus[0]) + "/online");
        g.saved_online = (online_str != "0");
    }

    int p = 0, e = 0;
    for (auto& g : c.core_groups) { if (g.is_pcore) p++; else e++; }
    syslog(LOG_INFO, "topology: %d P-core groups, %d E-core groups (%zu logical CPUs)",
           p, e, c.core_groups.size());
}

// Apply hotplug from optimizer result.
// Issue #4 fix: uses result.keep_groups from solve() instead of standalone
// compute_keep_groups(). The optimizer's power-ratio-based approach is the
// single canonical source for hotplug decisions.
static void apply_hotplug(CpuState& c, int keep_p, int keep_e) {
    if (c.core_groups.empty()) return;

    if (c.hotplug_settle > 0) { c.hotplug_settle--; return; }

    int total = (int)c.core_groups.size();

    // Count P and E groups
    int p_count = 0, e_count = 0;
    for (auto& g : c.core_groups) {
        if (g.is_pcore) p_count++; else e_count++;
    }

    // Convention: {0,0} → keep all online (safety floor / saturated).
    // If only one type is 0, it means "keep 0 of that type" (offlined).
    if (keep_p == 0 && keep_e == 0) {
        keep_p = p_count;
        keep_e = e_count;
    }

    // Check if state has changed
    int cur_p = 0, cur_e = 0;
    for (int i = 0; i < total; ++i) {
        bool online = false;
        for (int cp : c.core_groups[i].cpus) {
            std::string s = sysfs_read_file(std::string(SYSFS_CPU_BASE) + "/cpu" + std::to_string(cp) + "/online");
            if (s != "0") online = true;
        }
        if (online) {
            if (c.core_groups[i].is_pcore) cur_p++; else cur_e++;
        }
    }
    if (cur_p == keep_p && cur_e == keep_e) return;

    // Build "should_online" mask: pick top N from each type
    std::vector<bool> should_online(total, false);

    // 1. CPU0's group always online
    for (int i = 0; i < total; ++i)
        for (int cp : c.core_groups[i].cpus)
            if (cp == 0) { should_online[i] = true; break; }

    // 2. Pick top keep_p P-groups (by priority, already sorted P-first)
    { int p_filled = 0;
      for (int i = 0; i < total && p_filled < keep_p; ++i) {
          if (c.core_groups[i].is_pcore) {
              should_online[i] = true;
              p_filled++;
          }
      } }

    // 3. Pick top keep_e E-groups (by priority, lowest CPU id first)
    // Groups are sorted P-first, so E-groups come after P-groups.
    // Within E, higher CPU number = lower priority, so pick from the P-end.
    { int e_filled = 0;
      for (int i = 0; i < total && e_filled < keep_e; ++i) {
          if (!c.core_groups[i].is_pcore) {
              should_online[i] = true;
              e_filled++;
          }
      } }

    // 4. Ensure at least min_core_groups P-core groups stay online
    {
        int p_staying = 0;
        for (int i = 0; i < total; ++i)
            if (should_online[i] && c.core_groups[i].is_pcore) p_staying++;
        if (p_staying < default_resource_config.min_core_groups) {
            for (int i = 0; i < total; ++i) {
                if (!should_online[i] && c.core_groups[i].is_pcore) {
                    should_online[i] = true;
                    p_staying++;
                    if (p_staying >= default_resource_config.min_core_groups) break;
                }
            }
        }
    }

    // Apply
    bool changed = false;
    for (int i = 0; i < total; ++i) {
        auto& g = c.core_groups[i];
        std::string s = sysfs_read_file(std::string(SYSFS_CPU_BASE) + "/cpu" + std::to_string(g.cpus[0]) + "/online");
        bool is_online = (s != "0");
        if (is_online && !should_online[i]) {
            for (int cp : g.cpus)
                if (!sysfs_write_int(std::string(SYSFS_CPU_BASE) + "/cpu" + std::to_string(cp) + "/online", 0))
                    syslog(LOG_WARNING, "failed to offline cpu%d", cp);
            changed = true;
        } else if (!is_online && should_online[i]) {
            for (int cp : g.cpus)
                if (!sysfs_write_int(std::string(SYSFS_CPU_BASE) + "/cpu" + std::to_string(cp) + "/online", 1))
                    syslog(LOG_WARNING, "failed to online cpu%d", cp);
            changed = true;
        }
    }

    if (changed) {
        int new_count = count_online_groups(c);
        int new_p = 0, new_e = 0;
        for (int i = 0; i < total; ++i) {
            std::string s = sysfs_read_file(std::string(SYSFS_CPU_BASE) + "/cpu" + std::to_string(c.core_groups[i].cpus[0]) + "/online");
            if (s != "0") {
                if (c.core_groups[i].is_pcore) new_p++; else new_e++;
            }
        }
        syslog(LOG_INFO, "hotplug: %d groups online (P=%d E=%d, target=P=%d E=%d, prev=P=%d E=%d)",
               new_count, new_p, new_e, keep_p, keep_e, cur_p, cur_e);
        c.hotplug_settle = 6;
    }
}

static void restore_online_state(const CpuState& c) {
    for (auto& g : c.core_groups) {
        for (int cp : g.cpus) {
            if (g.saved_online)
                sysfs_write_int(std::string(SYSFS_CPU_BASE) + "/cpu" + std::to_string(cp) + "/online", 1);
        }
    }
    syslog(LOG_INFO, "restored CPU online state");
}

// ── Saved state (restored on exit) ──

static void save_cpu_state(SystemState& s) {
    sysfs_read_attr(SYSFS_PSTATE_DIR, "max_perf_pct", s.saved.max_perf);
    sysfs_read_attr(SYSFS_PSTATE_DIR, "min_perf_pct", s.saved.min_perf);
    sysfs_read_attr(SYSFS_PSTATE_DIR, "no_turbo", s.saved.no_turbo);
    if (!s.cpu.pcore_epp_paths.empty())
        s.saved.epp = sysfs_read_file(s.cpu.pcore_epp_paths[0]);
    s.gpu.saved_profile_gt0 = s.gpu.gt0.empty() ? "" : sysfs_read_file(s.gpu.gt0 + "/freq0/power_profile");
    s.gpu.saved_profile_gt1 = s.gpu.gt1.empty() ? "" : sysfs_read_file(s.gpu.gt1 + "/freq0/power_profile");
    if (!s.gpu.gt0.empty()) sysfs_read_attr(s.gpu.gt0 + "/freq0", "max_freq", s.gpu.saved_max_freq_gt0);
    if (!s.gpu.gt1.empty()) sysfs_read_attr(s.gpu.gt1 + "/freq0", "max_freq", s.gpu.saved_max_freq_gt1);
    if (!s.gpu.gt1.empty()) sysfs_read_attr(s.gpu.gt1 + "/freq0", "min_freq", s.gpu.min_freq_gt1);
    s.saved.msr_1fc = msr_available() ? probe_msr_read(get_platform_msrs().bd_prochot) : 0;
    syslog(LOG_INFO, "saved state: max_perf=%d  min_perf=%d  no_turbo=%d  epp=%s",
           s.saved.max_perf, s.saved.min_perf, s.saved.no_turbo, s.saved.epp.c_str());
}

static void restore_cpu_state(const SystemState& s) {
    sysfs_write_int(SYSFS_PSTATE_MAX, s.saved.max_perf);
    sysfs_write_int(SYSFS_PSTATE_MIN, s.saved.min_perf);
    sysfs_write_int(SYSFS_PSTATE_NOTURBO, s.saved.no_turbo);
    if (!s.saved.epp.empty())
        cpu_set_epp_all(s.cpu, s.saved.epp);
    if (!s.gpu.saved_profile_gt0.empty()) sysfs_write_str(s.gpu.gt0 + "/freq0/power_profile", s.gpu.saved_profile_gt0);
    if (!s.gpu.saved_profile_gt1.empty()) sysfs_write_str(s.gpu.gt1 + "/freq0/power_profile", s.gpu.saved_profile_gt1);
    if (s.gpu.saved_max_freq_gt0 > 0) sysfs_write_int(s.gpu.gt0 + "/freq0/max_freq", s.gpu.saved_max_freq_gt0);
    if (s.gpu.saved_max_freq_gt1 > 0) sysfs_write_int(s.gpu.gt1 + "/freq0/max_freq", s.gpu.saved_max_freq_gt1);
    if (s.saved.msr_1fc && get_platform_msrs().bd_prochot)
        probe_msr_write_with_value(get_platform_msrs().bd_prochot, s.saved.msr_1fc);
    syslog(LOG_INFO, "restored CPU and GPU state");
}

static void track_perf_limits(PerfLimitCounters& pl, int cpu) {
    unsigned long long msr = platform_read_cpu_perf_limit(cpu);
    unsigned int current = msr & 0xFFFF;
    unsigned int newly = current & ~pl.prev_current;
    for (int i = 0; PERF_LIMIT_REASONS[i].name; ++i) {
        unsigned int m = 1u << PERF_LIMIT_REASONS[i].bit;
        if (newly & m) pl.events[i]++;
    }
    pl.prev_current = current;
}

// ── Temperature ──

static void discover_thermal(ThermalState& t) {
    // Use shared temperature function — read_cpu_pkg_temp() scans thermal zones
    // and hwmon for CPU temperature.  We still discover coretemp dir for
    // per-core temperature display and intel_powerclamp for restore on exit.
    DIR* hwmon = opendir(SYSFS_HWMON);
    if (!hwmon) {
        // Discover intel_powerclamp via thermal zones
        DIR* therm = opendir(SYSFS_THERMAL);
        if (!therm) return;
        struct dirent* entry;
        while ((entry = readdir(therm)) != nullptr) {
            std::string name = entry->d_name;
            if (name.find("cooling_device") != 0) continue;
            std::string path = std::string(SYSFS_THERMAL) + "/" + name;
            if (sysfs_read_file(path + "/type") == "intel_powerclamp") {
                t.powerclamp_dev = path;
                sysfs_read_attr(t.powerclamp_dev, "cur_state", t.saved_powerclamp_state);
                syslog(LOG_INFO, "intel_powerclamp=%s (saved state=%d)", path.c_str(), t.saved_powerclamp_state);
                break;
            }
        }
        closedir(therm);
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(hwmon)) != nullptr) {
        std::string name = entry->d_name;
        if (name[0] == '.') continue;
        std::string path = std::string(SYSFS_HWMON) + "/" + name;
        if (sysfs_read_file(path + "/name") == "coretemp") {
            t.coretemp_dir = path;
            break;
        }
    }
    closedir(hwmon);
    // Discover intel_powerclamp
    DIR* therm = opendir(SYSFS_THERMAL);
    if (!therm) return;
    while ((entry = readdir(therm)) != nullptr) {
        std::string name = entry->d_name;
        if (name.find("cooling_device") != 0) continue;
        std::string path = std::string(SYSFS_THERMAL) + "/" + name;
        if (sysfs_read_file(path + "/type") == "intel_powerclamp") {
            t.powerclamp_dev = path;
            sysfs_read_attr(t.powerclamp_dev, "cur_state", t.saved_powerclamp_state);
            syslog(LOG_INFO, "intel_powerclamp=%s (saved state=%d)", path.c_str(), t.saved_powerclamp_state);
            break;
        }
    }
    closedir(therm);
}

// Read maximum temperature from coretemp hwmon (per-core granularity).
// Falls back to shared find_temperature() for thermal zone scanning.
static double read_max_core_temp(const ThermalState& t) {
    // Try coretemp hwmon first (per-core granularity)
    if (!t.coretemp_dir.empty()) {
        DIR* dir = opendir(t.coretemp_dir.c_str());
        if (dir) {
            double max_temp = -1;
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string fn = entry->d_name;
                if (fn.find("temp") == 0 && fn.size() > 5 && fn.find("_input") != std::string::npos) {
                    std::string val = sysfs_read_file(t.coretemp_dir + "/" + fn);
                    if (!val.empty()) {
                        double tval = std::stod(val) / 1000.0;
                        if (tval > max_temp) max_temp = tval;
                    }
                }
            }
            closedir(dir);
            if (max_temp >= 0) return max_temp;
        }
    }
    // Fallback: shared thermal zone scanner
    int t_c = read_cpu_pkg_temp();
    return t_c >= 0 ? (double)t_c : -1.0;
}

// ── Sampling ──
static double read_pl1_w(const RaplDomain& d) {
    long long uw = 0;
    sysfs_read_attr(d.path, "constraint_0_power_limit_uw", uw);
    if (uw > 0) return uw / 1e6;
    return DEFAULT_PL1_W;
}

// ── State ──
static volatile sig_atomic_t g_running = 1;
static void handle_signal(int) { g_running = 0; }

// ── Main loop helpers ──

// Gather RAPL power samples for this iteration
static void gather_samples(const SystemState& s,
                           Sample& prev_pkg, Sample& prev_core, Sample& prev_unc,
                           Sample& cur_pkg, Sample& cur_core, Sample& cur_unc,
                           double& pkg_w, double& core_w, double& gpu_w) {
    cur_pkg  = read_energy(s.pkg.path);
    cur_core = s.core.valid   ? read_energy(s.core.path)   : Sample{};
    cur_unc  = s.uncore.valid ? read_energy(s.uncore.path) : Sample{};

    pkg_w  = compute_power_w(prev_pkg, cur_pkg);
    core_w = s.core.valid   ? compute_power_w(prev_core, cur_core) : 0;
    gpu_w  = s.uncore.valid ? compute_power_w(prev_unc, cur_unc)   : 0;

    prev_pkg  = cur_pkg;
    if (s.core.valid)   prev_core = cur_core;
    if (s.uncore.valid) prev_unc  = cur_unc;
}

// Assess GPU state (throttling, active/idle)
static void assess_gpu_state(const GpuState& g, bool have_gpu, bool settle,
                             GpuThrottleCounters& tc,
                             bool& throttling, bool& active) {
    throttling = false;
    active = false;
    if (settle) return;
    if (have_gpu) {
        throttling = (gpu_get_throttle_state(g, &tc) != 0);
        active = gpu_is_active(g);
    }
}

// Clear Perf Limit Reasons (MSR 0x690) and Package Perf-Limit Log.
// Resets all sticky/current perf-limit bits so the daemon starts from a clean
// slate — any pre-existing throttle events from a previous session are erased.
//
//   MSR_CORE_PERF_LIMIT_REASONS / MSR_PERF_LIMIT_REASONS (per-core, current + sticky log)
//   Written to all logical CPUs at startup to erase stale throttle events.
static void clear_plr_history(const CpuState& c) {
    if (!platform_ok()) return;

    // Clear CPU perf-limit MSR on cpu0 (package-level)
    bool ok_pkg0 = platform_clear_cpu_perf_limit(0);

    // Clear per-core CPU perf-limit MSR on every online logical CPU
    int cores_cleared = 0;
    for (auto& g : c.core_groups) {
        for (int cpu : g.cpus) {
            if (platform_clear_cpu_perf_limit(cpu)) cores_cleared++;
        }
    }

    if (ok_pkg0)
        syslog(LOG_INFO, "PLR history cleared (pkg %s, %d cores)",
               msrs_to_string(get_platform_msrs()).c_str(), cores_cleared);
    else
        syslog(LOG_WARNING, "failed to clear PLR history (check permissions)");
}

// ── Main ──

int main(int argc, char** argv) {
    double pl1_w = DEFAULT_PL1_W;
    bool verbose = false;
    bool probe_mode = false;

    for (int i = 1; i < argc; ++i) {
        if (i + 1 < argc && strcmp(argv[i], "--pl1") == 0) {
            pl1_w = std::stod(argv[++i]);
            if (pl1_w < 1) pl1_w = DEFAULT_PL1_W;
            ++i;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--probe") == 0) {
            probe_mode = true;
        }
    }

    // ── Probe mode: print hardware info without modifying anything ──
    if (probe_mode) {
        SystemState s;
        discover_rapl(s);
        discover_gpu(s);
        discover_thermal(s.thermal);
        discover_clusters(s.cpu);
        discover_topology(s.cpu);

        bool have_gpu = !s.gpu.gt0.empty();

        // ── Header ──
        printf("power-balance --probe\n");
        printf("====================\n");
        printf("NOTE: run with daemon stopped for accurate topology.\n");
        printf("      (sudo systemctl stop power-balance)\n\n");

        // ── CPU model ──
        auto result = detect_platform_msrs();
        printf("CPU platform: %s\n", result.message.c_str());
        if (result.ok) {
            printf("MSR support: OK (all MSRs readable)\n");
        } else if (result.message.find("Meteor Lake") != std::string::npos ||
                   result.message.find("Lunar Lake") != std::string::npos ||
                   result.message.find("Alder/Raptor Lake") != std::string::npos ||
                   result.message.find("Haswell-Skylake") != std::string::npos) {
            // Generation recognized but MSRs not readable — likely permissions
            printf("MSR support: CPU recognized but MSRs not readable\n");
            printf("            (run with sudo or as root to probe MSRs)\n");
        } else {
            printf("MSR support: FAILED — unsupported CPU\n");
            printf("\nERROR: CPU is not supported. Cannot proceed.\n");
            return 1;
        }

        // ── RAPL domains ──
        printf("\n--- RAPL domains ---\n");
        printf("Package (MSR):     %s\n", s.pkg.valid ? s.pkg.path.c_str() : "NOT FOUND");
        if (s.pkg.valid) {
            printf("  PL1: %.1fW (max: %.1fW)  PL2: %.1fW  PL4: %.1fW\n",
                   s.pkg.pl1_w(), s.pkg.max_w(), s.pkg.pl2_w(), s.pkg.pl4_w());
        }
        printf("Package (MMIO):    %s\n",
               s.pkg_mmio.valid ? s.pkg_mmio.path.c_str() : "NOT FOUND");
        if (s.pkg_mmio.valid) {
            printf("  PL1: %.1fW (max: %.1fW)  PL2: %.1fW\n",
                   s.pkg_mmio.pl1_w(), s.pkg_mmio.max_w(), s.pkg_mmio.pl2_w());
        }
        printf("Core:              %s\n", s.core.valid ? s.core.path.c_str() : "NOT FOUND");
        if (s.core.valid) {
            printf("  PL1: %.1fW (max: %.1fW)  PL4: %.1fW\n",
                   s.core.pl1_w(), s.core.max_w(), s.core.pl4_w());
        }
        printf("Uncore:            %s\n", s.uncore.valid ? s.uncore.path.c_str() : "NOT FOUND");
        if (s.uncore.valid) {
            printf("  PL1: %.1fW (max: %.1fW)\n", s.uncore.pl1_w(), s.uncore.max_w());
        }
        if (!s.all_rapl_domains.empty()) {
            printf("Extra domains:    ");
            for (size_t i = 0; i < s.all_rapl_domains.size(); ++i) {
                if (i > 0) printf(", ");
                printf("%s", s.all_rapl_domains[i].name.c_str());
            }
            printf("\n");
        }

        // ── GPU ──
        printf("\n--- GPU ---\n");
        if (have_gpu) {
            printf("GT0 (render):    %s\n", s.gpu.gt0.c_str());
            printf("GT1 (media):     %s\n",
                   s.gpu.gt1.empty() ? "NOT FOUND" : s.gpu.gt1.c_str());
            printf("Idle status:     %s\n",
                   s.gpu.idle_path.empty() ? "NOT FOUND" : s.gpu.idle_path.c_str());
            printf("C0 residency:    %s\n",
                   s.gpu.c0_residency_path.empty() ? "NOT FOUND" : s.gpu.c0_residency_path.c_str());

            // Current GPU state (read-only)
            std::string gt0_prof = s.gpu.gt0 + "/freq0/power_profile";
            std::string gt0_prof_val = sysfs_read_file(gt0_prof);
            printf("GT0 profile:     %s\n", gt0_prof_val.empty() ? "?" : gt0_prof_val.c_str());
            int gt0_freq = 0;
            sysfs_read_attr(s.gpu.gt0 + "/freq0", "cur_freq", gt0_freq);
            printf("GT0 cur_freq:    %d MHz\n", gt0_freq);
            int gt0_max = 0, gt0_min = 0;
            sysfs_read_attr(s.gpu.gt0 + "/freq0", "max_freq", gt0_max);
            sysfs_read_attr(s.gpu.gt0 + "/freq0", "min_freq", gt0_min);
            printf("GT0 range:       %d-%d MHz\n", gt0_min, gt0_max);
        } else {
            printf("GPU: NOT FOUND (CPU-only mode)\n");
        }

        // ── Thermal ──
        printf("\n--- Thermal ---\n");
        printf("coretemp:        %s\n",
               s.thermal.coretemp_dir.empty() ? "NOT FOUND" : s.thermal.coretemp_dir.c_str());
        printf("powerclamp:      %s\n",
               s.thermal.powerclamp_dev.empty() ? "NOT FOUND" : s.thermal.powerclamp_dev.c_str());
        double max_temp = read_max_core_temp(s.thermal);
        printf("Current temp:    %.0f°C\n",
               max_temp >= 0 ? max_temp : -1);
        printf("pkg_temp (zone): %d°C\n", read_cpu_pkg_temp());

        // ── CPU topology ──
        printf("\n--- CPU topology ---\n");
        printf("Total core groups: %d\n", (int)s.cpu.core_groups.size());
        int pcount = 0, ecount = 0;
        for (auto& g : s.cpu.core_groups) {
            if (g.is_pcore) pcount++; else ecount++;
        }
        printf("P-core groups:   %d\n", pcount);
        printf("E-core groups:   %d\n", ecount);
        printf("\n");
        printf("  %-4s %-4s %-4s %-20s %-8s %s\n",
               "Idx", "P?", "HT", "CPUs", "Pri", "Online");
        for (auto& g : s.cpu.core_groups) {
            std::string on = sysfs_read_file(
                "/sys/devices/system/cpu/cpu" + std::to_string(g.cpus[0]) + "/online");
            std::string cpu_str;
            for (size_t i = 0; i < g.cpus.size(); ++i) {
                if (i) cpu_str += ",";
                cpu_str += std::to_string(g.cpus[i]);
            }
            printf("  %-4d %-4s %-4s %-20s %-8d %s\n",
                   g.id,
                   g.is_pcore ? "P" : "E",
                   g.has_ht ? "yes" : "no",
                   cpu_str.c_str(),
                   g.priority,
                   on == "0" ? "offline" : "online");
        }

        // ── CPU clusters (EPP paths) ──
        printf("\n--- CPU clusters ---\n");
        printf("P-core EPP paths: %zu\n", s.cpu.pcore_epp_paths.size());
        if (!s.cpu.pcore_epp_paths.empty()) {
            printf("  Example: %s\n", s.cpu.pcore_epp_paths[0].c_str());
            printf("  Current: %s\n", sysfs_read_file(s.cpu.pcore_epp_paths[0]).c_str());
        }
        printf("E-core EPP paths: %zu\n", s.cpu.ecore_epp_paths.size());
        if (!s.cpu.ecore_epp_paths.empty()) {
            printf("  Example: %s\n", s.cpu.ecore_epp_paths[0].c_str());
            printf("  Current: %s\n", sysfs_read_file(s.cpu.ecore_epp_paths[0]).c_str());
        }

        // ── Current P-state settings ──
        printf("\n--- Current P-state (intel_pstate) ---\n");
        int cur_max = 0, cur_min = 0, cur_noturbo = 0;
        sysfs_read_attr(SYSFS_PSTATE_DIR, "max_perf_pct", cur_max);
        sysfs_read_attr(SYSFS_PSTATE_DIR, "min_perf_pct", cur_min);
        sysfs_read_attr(SYSFS_PSTATE_DIR, "no_turbo", cur_noturbo);
        printf("max_perf_pct:    %d%%\n", cur_max);
        printf("min_perf_pct:    %d%%\n", cur_min);
        printf("no_turbo:        %s\n", cur_noturbo ? "ON" : "OFF");

        // ── Current cpufreq settings ──
        printf("\n--- Current cpufreq ---\n");
        int global_max = 0, global_min = 0;
        sysfs_read_attr("/sys/devices/system/cpu/cpu0/cpufreq", "cpuinfo_max_freq", global_max);
        sysfs_read_attr("/sys/devices/system/cpu/cpu0/cpufreq", "cpuinfo_min_freq", global_min);
        printf("CPU0 max_freq:   %d MHz\n", global_max / 1000);
        printf("CPU0 min_freq:   %d MHz\n", global_min / 1000);
        // E-core freq range
        int e_max = 0, e_min = 0;
        if (!s.cpu.ecore_epp_paths.empty()) {
            std::string epath = s.cpu.ecore_epp_paths[0];
            // Derive cpufreq dir from epp path
            std::string cdir = epath.substr(0, epath.find("/energy_performance_preference"));
            sysfs_read_attr(cdir, "cpuinfo_max_freq", e_max);
            sysfs_read_attr(cdir, "cpuinfo_min_freq", e_min);
            printf("E-core max_freq: %d MHz\n", e_max / 1000);
            printf("E-core min_freq: %d MHz\n", e_min / 1000);
        }

        // ── Scheduler demand ──
        printf("\n--- Scheduler demand ---\n");
        SchedulerDemand sched = read_scheduler_demand();
        printf("effective_demand: %.2f\n", sched.effective_demand);
        printf("pressure some:   avg10=%.2f%% avg60=%.2f%% avg300=%.2f%%\n",
               sched.pressure_some_avg10, sched.pressure_some_avg60, sched.pressure_some_avg300);
        printf("pressure full:   avg10=%.2f%% avg60=%.2f%% avg300=%.2f%%\n",
               sched.pressure_full_avg10, sched.pressure_full_avg60, sched.pressure_full_avg300);
        printf("load avg:        %.2f / %.2f / %.2f  (runnable: %d / %d)\n",
               sched.load_avg1, sched.load_avg5, sched.load_avg15,
               sched.running_tasks, sched.total_tasks);
        if (sched.cpu_measured) {
            printf("cpu_active:      %.1f%%  iowait: %.1f%%\n",
                   sched.cpu_active_pct, sched.cpu_iowait_pct);
        }

        // ── Service conflicts ──
        printf("\n--- Service conflicts ---\n");
        auto service_active = [](const char* name) -> bool {
            std::string cmd = std::string("systemctl is-active --quiet ") + name;
            int ret = system(cmd.c_str());
            return (ret == 0);
        };
        printf("power-profiles-daemon: %s\n",
               service_active("power-profiles-daemon") ? "ACTIVE (CONFLICT!)" : "inactive");
        printf("thermald:              %s\n",
               service_active("thermald") ? "ACTIVE (CONFLICT!)" : "inactive");
        printf("power-balance:         %s\n",
               service_active("power-balance") ? "ACTIVE (stop for full probe)" : "inactive");

        // ── Solver dry-run ──
        printf("\n--- Solver dry-run (current conditions) ---\n");
        ResourceConfig res_cfg;
        ResourceInputs res_inputs{};
        res_inputs.pl1_w           = pl1_w;
        res_inputs.gpu_power_w     = 0;  // snapshot: no measurement yet
        res_inputs.have_gpu        = have_gpu;
        res_inputs.temp_c          = max_temp;
        res_inputs.cpu_demand      = sched.effective_demand;
        res_inputs.running_tasks   = sched.running_tasks;
        res_inputs.total_core_groups = (int)s.cpu.core_groups.size();
        res_inputs.pcore_count       = 0;
        for (auto& g : s.cpu.core_groups) if (g.is_pcore) res_inputs.pcore_count++;
        res_inputs.cpu_domain_max_w = s.core.valid ? s.core.max_w() : 0.0;

        ResourceResult res = solve_resources(res_inputs, res_cfg);

        printf("PL1:           %.1fW\n", pl1_w);
        printf("GPU power:     %.1fW (snapshot)\n", res_inputs.gpu_power_w);
        printf("CPU draw:      %.1fW (measured)\n", res_inputs.cpu_measured_w > 0 ? res_inputs.cpu_measured_w : res_inputs.pl1_w);
        printf("Demand:        %.2f\n", sched.effective_demand);
        printf("P-core groups: %d (E-core: %d)\n",
               res_inputs.pcore_count, res_inputs.total_core_groups - res_inputs.pcore_count);
        printf("Temperature:   %.0f°C\n", max_temp);
        printf("\nSolver would set:\n");
        printf("  cpu_target:      %.1fW\n", res.cpu_target_w);
        printf("  core_limit:      %.1fW\n", res.core_limit_w);
        printf("  gpu_headroom:    %.1fW\n", res.gpu_headroom_w);
        double cpu_draw = res_inputs.cpu_measured_w > 0 ? res_inputs.cpu_measured_w : res.cpu_target_w;
        double budget_ratio = (cpu_draw > 0) ? res.cpu_target_w / cpu_draw : 0;
        printf("  budget_ratio:    %.2f\n", budget_ratio);
        printf("  max_perf_pct:    %d%%\n", res.max_perf_pct);
        printf("  no_turbo:        %s\n", res.no_turbo ? "ON" : "OFF");
        printf("  epp_p:           %s\n", epp_to_string(res.epp_p));
        printf("  epp_e:           %s\n", epp_to_string(res.epp_e));
        printf("  keep_p:          %d (%s)\n", res.keep_p, res.keep_p == 0 ? "all P" : "keep this many P");
        printf("  keep_e:          %d (%s)\n", res.keep_e, res.keep_e == 0 ? "all E" : "keep this many E");
        printf("  demand_factor:   %.2f\n", res.demand_factor);
        printf("  aggression:      %d (%s)\n",
               res.aggression,
               res.aggression == 0 ? "idle" : (res.aggression == 1 ? "active" : "throttling"));

        printf("\n--- Config ---\n");
        printf("risk_tolerance:  %.1f\n", res_cfg.risk_tolerance);
        printf("cpu_min_w:       %.1fW\n", res_cfg.cpu_min_w);
        printf("cpu_max_w:       %.1fW\n", res_cfg.cpu_max_w);
        printf("cpu_min_perf:    %d%%\n", res_cfg.cpu_min_perf);
        printf("min_core_groups: %d\n", res_cfg.min_core_groups);

        printf("\nProbe complete. No modifications were made.\n");
        return 0;
    }

    openlog("power-balance", LOG_PID | LOG_CONS, LOG_DAEMON);

    // ── Conflict check ──
    // Warn if other daemons that also manage CPU power are running.
    // Our daemon needs exclusive control over PSTATE, EPP, RAPL, and hotplug.
    {
        auto service_active = [](const char* name) -> bool {
            int fd[2] = {-1, -1};
            if (pipe(fd) != 0) return false;
            int ret = -1;
            // Use systemd-run to check status without blocking on output
            std::string cmd = std::string("systemctl is-active --quiet ") + name;
            ret = system(cmd.c_str());
            close(fd[0]); close(fd[1]);
            return (ret == 0);  // systemctl returns 0 when active
        };
        if (service_active("power-profiles-daemon"))
            syslog(LOG_WARNING, "CONFLICT: power-profiles-daemon is running — it fights over EPP/pstate/sysfs");
        if (service_active("thermald"))
            syslog(LOG_WARNING, "CONFLICT: thermald is running — it competes for thermal/cooler control");
    }

    SystemState s;

    // Discover all hardware paths
    if (!discover_rapl(s)) {
        syslog(LOG_ERR, "RAPL package domain not found");
        return 1;
    }
    discover_gpu(s);

    // Build a compact list of additional domains beyond pkg/core/uncore
    std::string extra_domains;
    for (auto& d : s.all_rapl_domains) {
        if (d.name != "core" && d.name != "uncore") {
            if (!extra_domains.empty()) extra_domains += ", ";
            extra_domains += d.name;
        }
    }
    syslog(LOG_INFO, "RAPL pkg=%s core=%s uncore=%s mmio=%s%s",
           s.pkg.path.c_str(),
           s.core.path.empty() ? "?" : s.core.path.c_str(),
           s.uncore.path.empty() ? "?" : s.uncore.path.c_str(),
           s.pkg_mmio.path.empty() ? "?" : s.pkg_mmio.path.c_str(),
           extra_domains.empty() ? "" : (" extra(" + extra_domains + ")").c_str());
    discover_thermal(s.thermal);

    bool have_gpu = !s.gpu.gt0.empty();
    if (have_gpu) {
        syslog(LOG_INFO, "GPU gt0=%s", s.gpu.gt0.c_str());
        if (!s.gpu.gt1.empty())
            syslog(LOG_INFO, "GPU gt1=%s", s.gpu.gt1.c_str());
        // GT0 gets the operational profile; GT1 stays in power_saving (TODO #3)
        set_gt0_profile(s.gpu, false);  // start idle = power_saving
        set_gt1_profile(s.gpu, "power_saving");
    } else
        syslog(LOG_WARNING, "GPU not found — running in CPU-only mode");

    if (!s.thermal.coretemp_dir.empty())
        syslog(LOG_INFO, "coretemp=%s", s.thermal.coretemp_dir.c_str());
    else
        syslog(LOG_WARNING, "coretemp not found — no temperature-aware capping");

    discover_clusters(s.cpu);
    discover_topology(s.cpu);

    // Read system PL1 for logging reference only (we raise it below)
    double sysfs_pl1 = read_pl1_w(s.pkg);
    (void)sysfs_pl1;
    double mmio_pl1 = s.pkg_mmio.valid ? read_pl1_w(s.pkg_mmio) : pl1_w;

    // Raise hardware PL1 to desired value on both MSR and MMIO domains.
    // Cap at package PL4 if present — exceeding PL4 triggers MSR 0x690 bit 11.
    {
        double effective_pl1 = pl1_w;
        if (s.pkg.pl4_w() > 0) {
            effective_pl1 = std::min(effective_pl1, s.pkg.pl4_w());
            if (effective_pl1 < pl1_w)
                syslog(LOG_INFO, "package PL1 clamped to PL4: %.1fW → %.1fW",
                       pl1_w, effective_pl1);
        }
        rapl_set_power_limit(s.pkg, effective_pl1);
        if (s.pkg_mmio.valid)
            rapl_set_power_limit(s.pkg_mmio, effective_pl1);
    }

    // Enable all discovered RAPL domains (TODO #2: dram, pp0, etc.)
    rapl_set_enabled(s.pkg, true);
    rapl_enable_all(s.all_rapl_domains);

    // Write time window for core/uncore RAPL domains once (constant PP0_TIME_WINDOW_US = 500 us).
    // The power limits are written in the main loop using if_changed variants.
    if (s.core.valid)
        rapl_set_power_limit(s.core, s.core.pl1_w() > 0 ? s.core.pl1_w() : 0.0);
    if (s.uncore.valid)
        rapl_set_power_limit(s.uncore, s.uncore.max_w() > 0 ? s.uncore.max_w() : 0.0);

    // Save initial CPU state
    save_cpu_state(s);

    // ── Platform support check (must pass before any MSR access) ──
    init_platform_msrs();
    if (!platform_ok()) {
        // detect_platform_msrs() already set res.message with the unsupported CPU
        // details. Print the full message and exit.
        std::string msg = detect_platform_msrs().message;
        size_t pos = msg.find("\n");
        if (pos != std::string::npos) {
            syslog(LOG_ERR, "%s", msg.substr(0, pos).c_str());
            for (size_t i = pos + 1; i < msg.size(); ) {
                size_t nl = msg.find('\n', i);
                if (nl == std::string::npos) nl = msg.size();
                syslog(LOG_INFO, "%s", msg.substr(i, nl - i).c_str());
                i = nl + 1;
            }
        } else {
            syslog(LOG_ERR, "%s", msg.c_str());
        }
        return 1;
    }

    // The EC on this laptop asserts PROCHOT# even at low temperatures (44°C / 4W),
    // which conflicts with the daemon's GPU-first power management.  Disable the
    // external PROCHOT# response (MSR_POWER_CTL bit 0 — BD_PROCHOT),
    // since we handle throttling ourselves via GPU headroom, EPP, and core offlining.
    // Restored on exit.
    if (platform_clear_bd_prochot()) {
        syslog(LOG_INFO, "external PROCHOT# response disabled (MSR_POWER_CTL bit 0)");
    }

    // Clear Perf Limit Reasons so the daemon starts from a clean slate.
    // Pre-existing throttle events from a previous session or boot are erased.
    clear_plr_history(s.cpu);

    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGHUP,  handle_signal);

    bool have_core   = s.core.valid;
    bool have_uncore = s.uncore.valid;
    int settle_cycles = 3;  // skip throttle detection until RAPL counters settle

    syslog(LOG_INFO, "starting — PL1: %.1fW (mmio: %.1fW)  interval: %dms  GPU: %s",
           pl1_w, mmio_pl1, INTERVAL_MS, have_gpu ? "yes" : "no");

    Sample prev_pkg  = read_energy(s.pkg.path);
    Sample prev_core, prev_unc;
    if (have_core)   prev_core = read_energy(s.core.path);
    if (have_uncore) prev_unc  = read_energy(s.uncore.path);

    double smoothed_gpu_w = 0;
    bool first = true;
    int iterations = 0;

    // Track throttle events (GPU and CPU MSR)
    GpuThrottleCounters gpu_tc;
    PerfLimitCounters pl_tc;
    int last_aggression = -1;      // 0=idle, 1=active, 2=throttling
    int prev_max_perf = 100;       // previous max_perf_pct (for smoothing across cycles)
    EppLevel prev_epp_p = EppLevel::BalancePerformance;  // prev EPP (for hysteresis)
    EppLevel prev_epp_e = EppLevel::BalancePerformance;
    int prev_keep_p = 0;  // prev P-core target (for hysteresis)
    int prev_keep_e = 0;  // prev E-core target (for hysteresis)

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(INTERVAL_MS));
        if (!g_running) break;
        iterations++;

        Sample cur_pkg, cur_core, cur_unc;
        double pkg_w, core_w, gpu_w;
        gather_samples(s, prev_pkg, prev_core, prev_unc,
                       cur_pkg, cur_core, cur_unc, pkg_w, core_w, gpu_w);

        if (pkg_w < 0) continue;
        if (first) { smoothed_gpu_w = gpu_w; first = false; }
        smoothed_gpu_w = default_resource_config.max_perf_smooth_alpha * gpu_w + (1.0 - default_resource_config.max_perf_smooth_alpha) * smoothed_gpu_w;



        // ── Assess GPU state (skip throttle check during settle) ──
        bool throttling = false, active = false;
        assess_gpu_state(s.gpu, have_gpu, settle_cycles > 0, gpu_tc, throttling, active);
        if (settle_cycles > 0) settle_cycles--;

        // ── GPU PL1/PL2 diagnostic (ratelimited) ──
        // GPU PL1/PL2 throttle means GuC SLPC hit its internal power budget.
        // The daemon has no control over GPU-domain power limits (uncore RAPL is unlimited).
        // Log once per ~60s as a diagnostic — user should check if package PL1 is sufficient.
        {
            static int gpu_pl_warn_cycle = 0;
            if (have_gpu && (gpu_tc.events[0] > 0 || gpu_tc.events[1] > 0)
                    && iterations - gpu_pl_warn_cycle > 120) {
                gpu_pl_warn_cycle = iterations;
                const char* reason = (gpu_tc.events[0] > 0) ? "PL1" : "PL2";
                syslog(LOG_WARNING,
                    "GPU %s throttle detected — GPU RAPL domain hit internal power limit. "
                    "Daemon has no control over GPU-domain power. "
                    "gpu=%.1fW pl1=%.1fW — consider raising package PL1 or reducing GPU workload.",
                    reason, smoothed_gpu_w, pl1_w);
            }
        }

        // ── C0 residency (activity-based thresholds, TODO #1) ──
        gpu_read_c0_residency(s.gpu);

        // ── Track CPU MSR perf limit reasons ──
        if (platform_ok()) track_perf_limits(pl_tc, 0);

        // ── Temperature reading ──
        double max_temp = read_max_core_temp(s.thermal);

        // ── Scheduler demand (CPU utilization + pressure from kernel) ──
        // Tells the optimizer how much the CPU actually needs.  Low demand →
        // the CPU has excess power headroom; high demand → CPU is saturated.
        // Read before optimizer inputs so cpu_demand is set.
        SchedulerDemand sched = read_scheduler_demand();

        // ── Solve optimization problem ──
        ResourceConfig res_cfg;
        ResourceInputs res_inputs{};
        res_inputs.pl1_w            = pl1_w;
        res_inputs.gpu_power_w      = smoothed_gpu_w;
        res_inputs.have_gpu         = have_gpu;
        res_inputs.temp_c           = max_temp;
        res_inputs.cpu_demand       = sched.effective_demand;
        res_inputs.gpu_c0_pct       = s.gpu.c0_pct;
        res_inputs.gpu_throttling   = throttling;
        res_inputs.gpu_power_var_w  = 2.0;  // placeholder: measured variance to be added
        res_inputs.cpu_measured_w   = core_w;
        res_inputs.cpu_domain_max_w = s.core.valid ? s.core.max_w() : 0.0;
        res_inputs.total_core_groups = (int)s.cpu.core_groups.size();
        res_inputs.pcore_count       = 0;
        for (auto& g : s.cpu.core_groups) if (g.is_pcore) res_inputs.pcore_count++;
        res_inputs.running_tasks     = sched.running_tasks;
        // Pass previous state for smoothing/hysteresis
        res_inputs.prev_max_perf    = prev_max_perf;
        res_inputs.prev_epp_p       = prev_epp_p;
        res_inputs.prev_epp_e       = prev_epp_e;
        res_inputs.prev_keep_p = prev_keep_p;
        res_inputs.prev_keep_e = prev_keep_e;

        ResourceResult res = solve_resources(res_inputs, res_cfg);

        // ── Aggression level (for logging) ──
        int aggression = res.aggression;

        // ── Debug: print solver inputs + output (LOG_DEBUG, only with --verbose) ──
        if (verbose) {
            syslog(LOG_DEBUG, "OPT-INPUTS: pl1=%.1fW gpu=%.1fW gpu_sm=%.1fW gpu_c0=%d%% "
                   "gpu_thr=%d temp=%.1fC cpu_measured=%.1fW core_max=%.1fW demand=%.2f "
                   "groups=%d running=%d",
                   pl1_w, smoothed_gpu_w, gpu_w,
                   (int)(s.gpu.c0_pct * 100.0), throttling, max_temp,
                   core_w, s.core.valid ? s.core.max_w() : 0.0,
                   sched.effective_demand, (int)s.cpu.core_groups.size(),
                   sched.running_tasks);
            syslog(LOG_DEBUG, "OPT-RESULT: cpu_target=%.1fW effective=%.1fW gpu_headroom=%.1fW "
                   "demand_factor=%.2f keep_p=%d keep_e=%d "
                   "core_lmt=%.1fW max_perf=%d%% no_turbo=%d epp=%s epp_e=%s",
                   res.cpu_target_w, res.effective_cpu_w(), res.gpu_headroom_w,
                   res.demand_factor, res.keep_p, res.keep_e,
                   res.core_limit_w, res.max_perf_pct, res.no_turbo,
                   epp_to_string(res.epp_p), epp_to_string(res.epp_e));
        }

        // ── Apply RAPL limits (core budget from solver) ──
        // Use if_changed — only write sysfs when values differ. Time window written once at init.
        if (s.core.valid) {
            double core_limit = res.core_limit_w;
            // Thermal surrender removed — budget is pure GPU-first allocation.
            if (s.core.pl4_w() > 0)
                core_limit = std::min(core_limit, s.core.pl4_w());
            rapl_set_power_limit_if_changed(s.core, core_limit);
        }
        if (s.uncore.valid)
            rapl_set_power_limit_if_changed(s.uncore, s.uncore.max_w() > 0 ? s.uncore.max_w() : 0.0);

        // ── MMIO package PL1 ──
        // Raise MMIO PL1 to match MSR PL1 so it doesn't become a bottleneck.
        // Cap at package PL4 if present — exceeding PL4 triggers MSR 0x690 bit 11.
        {
            double effective_pl1 = pl1_w;
            if (s.pkg.pl4_w() > 0)
                effective_pl1 = std::min(effective_pl1, s.pkg.pl4_w());
            if (s.pkg_mmio.valid) {
                sysfs_write_int_if_changed(s.pkg_mmio.path + "/constraint_0_power_limit_uw",
                                           (long long)(effective_pl1 * 1e6));
            }
        }

        // ── PROCHOT# response — already cleared at startup (MSR is persistent) ──

        // ── CPU frequency control (from solver) ──
        // Use if_changed — only write pstate sysfs when values differ from what's set.
        { // max_perf_pct
            int cur = 0;
            if (!sysfs_read_attr(SYSFS_PSTATE_DIR, "max_perf_pct", cur) || cur != res.max_perf_pct) {
                if (!sysfs_write_int(SYSFS_PSTATE_MAX, res.max_perf_pct))
                    syslog(LOG_WARNING, "failed to write max_perf_pct=%d", res.max_perf_pct);
            }
        }
        { // no_turbo
            int cur = 0;
            if (!sysfs_read_attr(SYSFS_PSTATE_DIR, "no_turbo", cur) || cur != res.no_turbo) {
                if (!sysfs_write_int(SYSFS_PSTATE_NOTURBO, res.no_turbo))
                    syslog(LOG_WARNING, "failed to write no_turbo=%d", res.no_turbo);
            }
        }
        { // min_perf_pct
            // If any cores are offlined (keep_p or keep_e > 0 and < total of that type),
            // set min_perf to 0 to allow downclocking.
            int p_total = 0, e_total = 0;
            for (auto& g : s.cpu.core_groups) {
                if (g.is_pcore) p_total++; else e_total++;
            }
            bool p_offlined = (res.keep_p > 0 && res.keep_p < p_total);
            bool e_offlined = (res.keep_e > 0 && res.keep_e < e_total);
            int min_perf = (p_offlined || e_offlined) ? 0 : s.saved.min_perf;
            int cur = 0;
            if (!sysfs_read_attr(SYSFS_PSTATE_DIR, "min_perf_pct", cur) || cur != min_perf) {
                if (!sysfs_write_int(SYSFS_PSTATE_MIN, min_perf))
                    syslog(LOG_WARNING, "failed to write min_perf_pct=%d", min_perf);
            }
        }

        // ── EPP (from solver) ──
        cpu_set_epp(s.cpu, epp_to_string(res.epp_p), epp_to_string(res.epp_e));

        // ── CPU hotplug (core offlining) ──
        apply_hotplug(s.cpu, res.keep_p, res.keep_e);

        // ── GT0 power profile ──
        // Active GPU → "base" profile (full performance).
        // Idle GPU → "power_saving" (minimises idle draw).
        set_gt0_profile(s.gpu, active);

        // ── GT1 frequency cap ──
        // When GPU is active (aggression >= 1), cap GT1 (media engine) max_freq
        // to min_freq to prevent media decode bursts from spiking GT power and
        // triggering PL1/PL2/PL4 events that throttle GT0 (render).
        set_gt1_freq_cap(s.gpu, aggression >= 1);

        // ── Log state changes ──
        if (aggression != last_aggression || iterations % 20 == 0) {
            const char* state = "idle";
            if (aggression == 1) state = "active";
            if (aggression == 2) state = "balance-throttle";
            // Build GPU throttle event summary
            std::string gpu_thr;
            if (have_gpu && gpu_tc.total_events > 0) {
                for (int i = 0; XE_THROTTLE_REASONS[i]; ++i) {
                    if (gpu_tc.events[i] > 0) {
                        if (!gpu_thr.empty()) gpu_thr += " ";
                        gpu_thr += std::string(XE_THROTTLE_REASONS[i]) + ":" + std::to_string(gpu_tc.events[i]);
                    }
                }
            }
            // Build CPU MSR perf limit summary (currently active reasons)
            std::string cpu_thr;
            if (platform_ok()) {
                unsigned long long msr = platform_read_cpu_perf_limit(0);
                unsigned int current = msr & 0xFFFF;
                for (int i = 0; PERF_LIMIT_REASONS[i].name; ++i) {
                    if (i == 0) continue;  // PROCHOT handled separately below
                    if (current & (1u << PERF_LIMIT_REASONS[i].bit)) {
                        if (!cpu_thr.empty()) cpu_thr += " ";
                        cpu_thr += PERF_LIMIT_REASONS[i].name;
                    }
                }
            }
            std::string throttle_summary;
            if (!gpu_thr.empty()) throttle_summary += "  gpu-throttle: " + gpu_thr;
            if (!cpu_thr.empty()) throttle_summary += "  cpu-throttle: " + cpu_thr;
            char temp_buf[32] = "";
            if (max_temp >= 0)
                snprintf(temp_buf, sizeof(temp_buf), "  temp=%.0fC", max_temp);
            // C0 residency percentage (only when path is available)
            char c0_buf[32] = "";
            if (!s.gpu.c0_residency_path.empty())
                snprintf(c0_buf, sizeof(c0_buf), "  c0=%d%%", (int)(s.gpu.c0_pct * 100.0));
            std::string epp_str = epp_to_string(res.epp_p);
            if (res.epp_e != res.epp_p) epp_str += "(" + std::string(epp_to_string(res.epp_e)) + ")";
            // Solver diagnostics
            char res_buf[64] = "";
            if (res.demand_factor != 1.0) {
                snprintf(res_buf, sizeof(res_buf), "  demand=%.2f",
                         res.demand_factor);
            }
            syslog(LOG_INFO, "[%s] pkg=%.1fW core=%.1fW gpu=%.1fW(gpu_sm=%.1fW) "
                   "pl1=%.1fW core_lmt=%.1fW max_perf=%d%% no_turbo=%d epp=%s%s%s%s%s kp_P=%d kp_E=%d",
                   state, pkg_w, core_w, gpu_w, smoothed_gpu_w,
                   pl1_w, res.core_limit_w, res.max_perf_pct, res.no_turbo, epp_str.c_str(),
                   temp_buf, res_buf, throttle_summary.c_str(), c0_buf,
                   res.keep_p, res.keep_e);
            last_aggression = aggression;
        }

        // ── Save state for next cycle smoothing ──
        prev_max_perf = res.max_perf_pct;
        prev_epp_p = res.epp_p;
        prev_epp_e = res.epp_e;
        prev_keep_p = res.keep_p;
        prev_keep_e = res.keep_e;
    }

    // ── Report final throttle statistics ──
    if (have_gpu && gpu_tc.total_events > 0) {
        std::string summary;
        for (int i = 0; XE_THROTTLE_REASONS[i]; ++i) {
            if (gpu_tc.events[i] > 0) {
                if (!summary.empty()) summary += ", ";
                summary += std::string(XE_THROTTLE_REASONS[i]) + "=" + std::to_string(gpu_tc.events[i]);
            }
        }
        syslog(LOG_INFO, "GPU hardware throttle events: %d total (%s)  cycles_throttled=%d",
               gpu_tc.total_events, summary.c_str(), gpu_tc.cycles_throttled);
    } else if (have_gpu) {
        syslog(LOG_INFO, "GPU hardware throttle events: none");
    }
    if (platform_ok()) {
        std::string cpu_summary;
        int cpu_total = 0;
        for (int i = 0; PERF_LIMIT_REASONS[i].name; ++i) {
            if (i == 0) continue;  // PROCHOT — external signal we disabled via MSR 0x1FC
            if (pl_tc.events[i] > 0) {
                if (!cpu_summary.empty()) cpu_summary += ", ";
                cpu_summary += std::string(PERF_LIMIT_REASONS[i].name) + "=" + std::to_string(pl_tc.events[i]);
                cpu_total += pl_tc.events[i];
            }
        }
        if (cpu_total > 0) {
            syslog(LOG_INFO, "CPU perf limit events: %d total (%s)", cpu_total, cpu_summary.c_str());
        } else {
            syslog(LOG_INFO, "CPU perf limit events: none");
        }
    }

    // ── Restore everything on exit ──
    rapl_set_all(s.all_rapl_domains, 0.0);  // reset all subdomains to unlimited
    if (!s.thermal.powerclamp_dev.empty())
        sysfs_write_int(s.thermal.powerclamp_dev + "/cur_state", s.thermal.saved_powerclamp_state);
    restore_online_state(s.cpu);
    restore_cpu_state(s);

    syslog(LOG_INFO, "stopped");
    closelog();
    return 0;
}
