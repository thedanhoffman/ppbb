// power-balance.cpp — GPU-first power balancer daemon
// Principle: the GPU must never throttle. Everything else (CPU frequency,
// RAPL limits, turbo, EPP) is aggressively constrained to ensure the GPU
// always has the power headroom it needs.
//
// Control logic uses linear constraint optimization (power-optimizer.h) to
// replace hardcoded thresholds with a principled weight-based scheduler.
// Compile: g++ -std=c++17 -O2 power-balance.cpp -o power-balance

#include "power-optimizer.h"
#include "power-utils.h"
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
static constexpr int      INTERVAL_MS          = 500;
// GPU activity tracking: C0 residency (c0_pct 0.0–1.0) over 500ms window
// Drives weight schedule in power-optimizer: idle/active/heavy/throttling.
// Fallback: when C0 sysfs is unavailable, gpu_w > GPU_HEAVY_W (15W) triggers
// at-least "active" weights to prevent idle CPU + high GPU power combos.
//
// GT1 power minimization (TODO #3 done):
//   GT0 (graphics) locked in power_saving via set_gt0_profile()
//   GT1 (media) locked in power_saving via set_gt1_profile() — always kept idle
//   saved_profile_gt1 tracks GT1's original profile for restore on exit
//
// Dynamic RAPL domains (TODO #2 done):
//   all_rapl_domains tracks every discovered subdomain (dram, pp0, etc.)
//   Core budget applied to all non-uncore domains; uncore left unlimited
static constexpr double   GPU_ACTIVE_W         = 3.0;
static constexpr double   SMOOTH_ALPHA         = 0.3;
static constexpr long     PP0_TIME_WINDOW_US   = 500;

// ── Data-only structs ──
// All runtime state lives in one global (SystemState).  Each sub-struct owns
// its own data; free functions below operate on the structs by reference.

// RAPL powercap domain
struct RaplDomain {
    std::string path;          // /sys/class/powercap/intel-rapl/domain-N
    bool        valid = false;
    std::string domain_type;   // "core", "uncore", "dram", "pp0", etc.
    double      pl4_w = 0;     // PL4 peak power limit (W), 0 = not supported
    double      max_w = 0;     // constraint_0_max_power_uw (W), 0 = not supported
};

// GPU throttle event tracking
struct GpuThrottleCounters {
    int events[8] = {0};
    int total_events = 0;
    int cycles_throttled = 0;
    bool prev_state[8] = {false};
};

// CPU perf-limit event tracking (MSR 0x6B0)
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
    // Replaces hardcoded GPU_ACTIVE_W/GPU_HEAVY_W thresholds with
    // residency percentage, which is much more portable across GPUs.
    bool        has_c0_residency = false;  // true when c0_residency_path is populated
    std::string c0_residency_path; // gt0/activity/c0_residency_ms
    long long   last_c0_residency_us = 0; // microseconds, last read value
    long long   _last_c0_time_us = 0;     // steady_clock time of last read (internal)
    double      c0_pct = 0.0;              // 0.0–1.0, residency fraction over measurement window
};

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
    int    keep_groups_target = -1;
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

static void rapl_set_enabled(const RaplDomain& d, bool on) {
    if (!d.valid) return;
    sysfs_write_file(d.path + "/enabled", on ? "1" : "0");
}

// Set power limit on ALL RAPL domains (pkg excluded — that's PL1, not a subdomain).
// Used during initialization to enable all domains, and during cleanup to reset them.
static void rapl_set_all(const std::vector<RaplDomain>& domains, double watts_w) {
    for (auto& d : domains) {
        double val = (watts_w == 0.0 && d.max_w > 0) ? d.max_w : watts_w;
        rapl_set_power_limit(d, val);
    }
}

// Enable all RAPL subdomains.
static void rapl_enable_all(const std::vector<RaplDomain>& domains) {
    for (auto& d : domains) rapl_set_enabled(d, true);
}

// ── RAPL discovery ──
// Discovers ALL RAPL subdomains (core, uncore, dram, pp0, pp1, etc.),
// populating both the typed fields and a flat all_rapl_domains list.

static bool discover_rapl(SystemState& s) {
    for (const char* root : {"intel-rapl", "intel-rapl-mmio"}) {
        std::string base = std::string("/sys/class/powercap/") + root;
        DIR* dir = opendir(base.c_str());
        if (!dir) continue;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name[0] == '.') continue;
            std::string path = base + "/" + name;
            std::string rname = sysfs_read_file(path + "/name");
            if (rname.find("package-") != std::string::npos) {
                if (std::string(root) == "intel-rapl-mmio")
                    s.pkg_mmio.path = path;
                else
                    s.pkg.path = path;
                // Scan subdomains (only for intel-rapl, not mmio)
                DIR* sdir = opendir(path.c_str());
                if (sdir) {
                    struct dirent* se;
                    while ((se = readdir(sdir)) != nullptr) {
                        std::string sn = se->d_name;
                        if (sn[0] == '.') continue;
                        std::string sp = path + "/" + sn;
                        std::string sr = sysfs_read_file(sp + "/name");
                        if (sr.empty()) continue;
                        RaplDomain sub;
                        sub.path = sp;
                        sub.domain_type = sr;
                        sub.valid = true;
                        // Read PL4 peak power limit (constraint_2) if supported
                        long long pl4_uw = 0;
                        if (sysfs_read_attr(sp, "constraint_2_power_limit_uw", pl4_uw)
                                && pl4_uw > 0)
                            sub.pl4_w = pl4_uw / 1e6;
                        // Read max power (constraint_0_max) for "unlimited" writes
                        long long max_uw = 0;
                        if (sysfs_read_attr(sp, "constraint_0_max_power_uw", max_uw)
                                && max_uw > 0)
                            sub.max_w = max_uw / 1e6;
                        // Also set typed fields for backward compat
                        if (sr == "core") { s.core = sub; }
                        else if (sr == "uncore") { s.uncore = sub; }
                        // Store in all_domains for dynamic control
                        s.all_rapl_domains.push_back(sub);
                    }
                    closedir(sdir);
                }
            }
        }
        closedir(dir);
    }
    // Mark valid
    s.pkg.valid = !s.pkg.path.empty();
    s.pkg_mmio.valid = !s.pkg_mmio.path.empty();
    s.core.valid = !s.core.path.empty();
    s.uncore.valid = !s.uncore.path.empty();

    // Read package PL4 (constraint_2) and max power if supported
    long long pkg_pl4_uw = 0;
    if (s.pkg.valid && sysfs_read_attr(s.pkg.path, "constraint_2_power_limit_uw", pkg_pl4_uw)
            && pkg_pl4_uw > 0)
        s.pkg.pl4_w = pkg_pl4_uw / 1e6;
    long long pkg_max_uw = 0;
    if (s.pkg.valid && sysfs_read_attr(s.pkg.path, "constraint_0_max_power_uw", pkg_max_uw)
            && pkg_max_uw > 0)
        s.pkg.max_w = pkg_max_uw / 1e6;
    long long pkg_mmio_max_uw = 0;
    if (s.pkg_mmio.valid && sysfs_read_attr(s.pkg_mmio.path, "constraint_0_max_power_uw", pkg_mmio_max_uw)
            && pkg_mmio_max_uw > 0)
        s.pkg_mmio.max_w = pkg_mmio_max_uw / 1e6;

    // Log discovered domains
    char log_pl4[64] = "";
    if (s.core.pl4_w > 0)
        snprintf(log_pl4, sizeof(log_pl4), " core_pl4=%.1fW", s.core.pl4_w);
    else if (s.pkg.pl4_w > 0)
        snprintf(log_pl4, sizeof(log_pl4), " pkg_pl4=%.1fW", s.pkg.pl4_w);
    syslog(LOG_INFO, "RAPL: pkg=%s pkg_mmio=%s core=%s uncore=%s dram=%s%s",
           s.pkg.path.c_str(),
           s.pkg_mmio.path.empty() ? "n/a" : s.pkg_mmio.path.c_str(),
           s.core.path.c_str(),
           s.uncore.path.empty() ? "n/a" : s.uncore.path.c_str(),
           s.core.path.empty() ? "n/a" : s.core.path.c_str(),
           log_pl4);
    if (s.all_rapl_domains.size() > 2) {
        std::string types;
        for (auto& d : s.all_rapl_domains) {
            if (!types.empty()) types += ", ";
            types += d.domain_type + "=" + d.path;
        }
        syslog(LOG_INFO, "RAPL all domains: %s", types.c_str());
    }

    return s.pkg.valid;
}

// ── GPU path discovery ──

static void discover_gpu(SystemState& s) {
    DIR* drm = opendir("/sys/class/drm");
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

// Compute residency-based aggression from C0 percentage.
// Returns aggression level: 0=idle, 1=active, 2=heavy
static int gpu_throttle_events(const GpuState& g, GpuThrottleCounters* counters) {
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

// Apply profile to GT0 (graphics engine).
// Apply power_saving profile to GT0 to reduce PL4 events.
static void set_gt0_profile(const GpuState& g, const std::string& profile) {
    if (g.gt0.empty()) return;
    std::string path = g.gt0 + "/freq0/power_profile";
    std::string cur = sysfs_read_file(path);
    if (cur.empty()) return;
    if (cur.find(profile) == std::string::npos || cur.find("[" + profile + "]") == std::string::npos)
        sysfs_write_str(path, profile);
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

// ── CPU control paths ──
static const std::string PSTATE_DIR    = "/sys/devices/system/cpu/intel_pstate";
static const std::string PSTATE_MAX    = PSTATE_DIR + "/max_perf_pct";
static const std::string PSTATE_MIN    = PSTATE_DIR + "/min_perf_pct";
static const std::string PSTATE_NOTURBO = PSTATE_DIR + "/no_turbo";

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

    DIR* dir = opendir("/sys/devices/system/cpu");
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.find("cpu") != 0) continue;
        std::string epp = "/sys/devices/system/cpu/" + name + "/cpufreq/energy_performance_preference";
        int max_freq = 0;
        if (sysfs_read_file(epp) == "") continue;
        sysfs_read_attr("/sys/devices/system/cpu/" + name + "/cpufreq", "cpuinfo_max_freq", max_freq);
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
        std::string s = sysfs_read_file("/sys/devices/system/cpu/cpu" + std::to_string(g.cpus[0]) + "/online");
        if (s != "0") n++;
    }
    return n;
}

static void discover_topology(CpuState& c) {
    // Build core groups: CPUs sharing the same physical core
    std::map<int, std::vector<int>> core_map;
    DIR* dir = opendir("/sys/devices/system/cpu");
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

// Target logical CPUs to keep online based on aggression + temperature.
// Returns the number of CoreGroups to keep online.
static int compute_keep_groups(int aggression, double temp_c, double /*gpu_w*/) {
    // idle/cool:  all groups online
    if (aggression <= 0 && temp_c < 70.0) return 0;  // 0 = all

    // critical:   temp >= 90°C or GPU throttling hard → 1 P-core group (2 threads)
    if (aggression >= 2 || temp_c >= 90.0) return 1;

    // throttle:   temp 85-90°C or throttling → 3 P + 1 E = 4 groups (7 threads)
    if (temp_c >= 85.0) return 4;

    // heavy:      temp 80-85°C or GPU active (via residency or agg level)
    //             → 6 P + 2 E = 8 groups (14 threads)
    if (aggression >= 1 || temp_c >= 80.0) return 8;

    // moderate:   temp 70-80°C or GPU active → 6 P + 6 E = 12 groups (18 threads)
    return 12;
}

static void apply_hotplug(CpuState& c, int aggression, double temp_c, double gpu_w) {
    if (c.core_groups.empty()) return;

    if (c.hotplug_settle > 0) { c.hotplug_settle--; return; }

    int total = (int)c.core_groups.size();
    int target = compute_keep_groups(aggression, temp_c, gpu_w);
    if (target == 0) target = total;  // 0 means "all"
    target = std::max(target, 1);
    if (target == c.keep_groups_target) return;

    int current = count_online_groups(c);

    // Build a "should_online" mask for each group:
    // 1. Start by marking the last `target` groups (lowest priority / most worth keeping)
    // 2. CPU0 group always online — mark it before any P-core minimum check
    // 3. Ensure at least 2 P-core groups remain (CPU0 counts toward this)
    std::vector<bool> should_online(total, false);
    for (int i = total - target; i < total; ++i) should_online[i] = true;

    for (int i = 0; i < total; ++i)
        for (int cp : c.core_groups[i].cpus)
            if (cp == 0) { should_online[i] = true; break; }

    // Ensure at least 2 P-core groups including CPU0
    for (int i = 0; i < total; ++i) {
        if (!should_online[i] && c.core_groups[i].is_pcore) {
            int p_would_stay = 0;
            for (int j = 0; j < total; ++j)
                if (should_online[j] && c.core_groups[j].is_pcore) p_would_stay++;
            if (p_would_stay < MIN_CORE_GROUPS) should_online[i] = true;
        }
    }

    // Apply: offline groups marked offline, online groups marked online
    bool changed = false;
    for (int i = 0; i < total; ++i) {
        auto& g = c.core_groups[i];
        std::string s = sysfs_read_file("/sys/devices/system/cpu/cpu" + std::to_string(g.cpus[0]) + "/online");
        bool is_online = (s != "0");
        if (is_online && !should_online[i]) {
            for (int cp : g.cpus)
                sysfs_write_int("/sys/devices/system/cpu/cpu" + std::to_string(cp) + "/online", 0);
            changed = true;
        } else if (!is_online && should_online[i]) {
            for (int cp : g.cpus)
                sysfs_write_int("/sys/devices/system/cpu/cpu" + std::to_string(cp) + "/online", 1);
            changed = true;
        }
    }

    if (changed) {
        int new_count = count_online_groups(c);
        syslog(LOG_INFO, "hotplug: %d groups online (target=%d, keep=%d)",
               new_count, target, current);
        c.keep_groups_target = target;
        c.hotplug_settle = 20;
    }
}

static void restore_online_state(const CpuState& c) {
    for (auto& g : c.core_groups) {
        for (int cp : g.cpus) {
            if (g.saved_online)
                sysfs_write_int("/sys/devices/system/cpu/cpu" + std::to_string(cp) + "/online", 1);
        }
    }
    syslog(LOG_INFO, "restored CPU online state");
}

// ── Saved state (restored on exit) ──

static void save_cpu_state(SystemState& s) {
    sysfs_read_attr(PSTATE_DIR, "max_perf_pct", s.saved.max_perf);
    sysfs_read_attr(PSTATE_DIR, "min_perf_pct", s.saved.min_perf);
    sysfs_read_attr(PSTATE_DIR, "no_turbo", s.saved.no_turbo);
    if (!s.cpu.pcore_epp_paths.empty())
        s.saved.epp = sysfs_read_file(s.cpu.pcore_epp_paths[0]);
    s.gpu.saved_profile_gt0 = s.gpu.gt0.empty() ? "" : sysfs_read_file(s.gpu.gt0 + "/freq0/power_profile");
    s.gpu.saved_profile_gt1 = s.gpu.gt1.empty() ? "" : sysfs_read_file(s.gpu.gt1 + "/freq0/power_profile");
    if (!s.gpu.gt0.empty()) sysfs_read_attr(s.gpu.gt0 + "/freq0", "max_freq", s.gpu.saved_max_freq_gt0);
    if (!s.gpu.gt1.empty()) sysfs_read_attr(s.gpu.gt1 + "/freq0", "max_freq", s.gpu.saved_max_freq_gt1);
    if (!s.gpu.gt1.empty()) sysfs_read_attr(s.gpu.gt1 + "/freq0", "min_freq", s.gpu.min_freq_gt1);
    s.saved.msr_1fc = msr_available() ? read_msr(0, 0x1FC) : 0;
    syslog(LOG_INFO, "saved state: max_perf=%d  min_perf=%d  no_turbo=%d  epp=%s",
           s.saved.max_perf, s.saved.min_perf, s.saved.no_turbo, s.saved.epp.c_str());
}

static void restore_cpu_state(const SystemState& s) {
    sysfs_write_int(PSTATE_MAX, s.saved.max_perf);
    sysfs_write_int(PSTATE_MIN, s.saved.min_perf);
    sysfs_write_int(PSTATE_NOTURBO, s.saved.no_turbo);
    if (!s.saved.epp.empty())
        cpu_set_epp_all(s.cpu, s.saved.epp);
    if (!s.gpu.saved_profile_gt0.empty()) sysfs_write_str(s.gpu.gt0 + "/freq0/power_profile", s.gpu.saved_profile_gt0);
    if (!s.gpu.saved_profile_gt1.empty()) sysfs_write_str(s.gpu.gt1 + "/freq0/power_profile", s.gpu.saved_profile_gt1);
    if (s.gpu.saved_max_freq_gt0 > 0) sysfs_write_int(s.gpu.gt0 + "/freq0/max_freq", s.gpu.saved_max_freq_gt0);
    if (s.gpu.saved_max_freq_gt1 > 0) sysfs_write_int(s.gpu.gt1 + "/freq0/max_freq", s.gpu.saved_max_freq_gt1);
    if (s.saved.msr_1fc) write_msr(0, 0x1FC, s.saved.msr_1fc);
    syslog(LOG_INFO, "restored CPU and GPU state");
}

static void track_perf_limits(PerfLimitCounters& pl, int cpu) {
    unsigned long long msr = read_msr(cpu, 0x6B0);
    unsigned int current = msr & 0xFFFF;
    unsigned int newly = current & ~pl.prev_current;
    for (int i = 0; PERF_LIMIT_REASONS[i].name; ++i) {
        unsigned int m = 1u << PERF_LIMIT_REASONS[i].bit;
        if (newly & m) pl.events[i]++;
    }
    pl.prev_current = current;
}

// ── Temperature ──

static void discover_coretemp(ThermalState& t) {
    DIR* hwmon = opendir("/sys/class/hwmon");
    if (!hwmon) return;
    struct dirent* entry;
    while ((entry = readdir(hwmon)) != nullptr) {
        std::string name = entry->d_name;
        if (name[0] == '.') continue;
        std::string path = std::string("/sys/class/hwmon/") + name;
        if (sysfs_read_file(path + "/name") == "coretemp") {
            t.coretemp_dir = path;
            break;
        }
    }
    closedir(hwmon);
    // Discover intel_powerclamp
    DIR* therm = opendir("/sys/class/thermal");
    if (!therm) return;
    while ((entry = readdir(therm)) != nullptr) {
        std::string name = entry->d_name;
        if (name.find("cooling_device") != 0) continue;
        std::string path = std::string("/sys/class/thermal/") + name;
        if (sysfs_read_file(path + "/type") == "intel_powerclamp") {
            t.powerclamp_dev = path;
            sysfs_read_attr(t.powerclamp_dev, "cur_state", t.saved_powerclamp_state);
            syslog(LOG_INFO, "intel_powerclamp=%s (saved state=%d)", path.c_str(), t.saved_powerclamp_state);
            break;
        }
    }
    closedir(therm);
}

static double read_max_core_temp(const ThermalState& t) {
    if (t.coretemp_dir.empty()) return -1;
    DIR* dir = opendir(t.coretemp_dir.c_str());
    if (!dir) return -1;
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
    return max_temp;
}

// ── Sampling ──
static Sample read_energy(const std::string& dir) {
    Sample s;
    sysfs_read_attr(dir, "energy_uj", s.energy_uj);
    s.time = std::chrono::steady_clock::now();
    return s;
}

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
        throttling = (gpu_throttle_events(g, &tc) != 0);
        active = gpu_is_active(g);
    }
}

// Disable EC PROCHOT# response via MSR 0x1FC bit 0
static void clear_prochot_msr(bool msr_ok) {
    if (!msr_ok) return;
    unsigned long long msr_1fc = read_msr(0, 0x1FC);
    if (msr_1fc & 1)
        write_msr(0, 0x1FC, msr_1fc & ~1ULL);
}

// ── Main ──

int main(int argc, char** argv) {
    double pl1_w = DEFAULT_PL1_W;

    for (int i = 1; i < argc; ++i) {
        if (i + 1 < argc && strcmp(argv[i], "--pl1") == 0) {
            pl1_w = std::stod(argv[++i]);
            if (pl1_w < 1) pl1_w = DEFAULT_PL1_W;
        }
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
        if (d.domain_type != "core" && d.domain_type != "uncore") {
            if (!extra_domains.empty()) extra_domains += ", ";
            extra_domains += d.domain_type;
        }
    }
    syslog(LOG_INFO, "RAPL pkg=%s core=%s uncore=%s mmio=%s%s",
           s.pkg.path.c_str(),
           s.core.path.empty() ? "?" : s.core.path.c_str(),
           s.uncore.path.empty() ? "?" : s.uncore.path.c_str(),
           s.pkg_mmio.path.empty() ? "?" : s.pkg_mmio.path.c_str(),
           extra_domains.empty() ? "" : (" extra(" + extra_domains + ")").c_str());
    discover_coretemp(s.thermal);

    bool have_gpu = !s.gpu.gt0.empty();
    if (have_gpu) {
        syslog(LOG_INFO, "GPU gt0=%s", s.gpu.gt0.c_str());
        if (!s.gpu.gt1.empty())
            syslog(LOG_INFO, "GPU gt1=%s", s.gpu.gt1.c_str());
        // GT0 gets the operational profile; GT1 stays in power_saving (TODO #3)
        set_gt0_profile(s.gpu, "power_saving");
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
    // Cap at package PL4 if present — exceeding PL4 triggers MSR 0x6B0 bit 11.
    {
        double effective_pl1 = pl1_w;
        if (s.pkg.pl4_w > 0) {
            effective_pl1 = std::min(effective_pl1, s.pkg.pl4_w);
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

    // Save initial CPU state
    save_cpu_state(s);

    // The EC on this laptop asserts PROCHOT# even at low temperatures (44°C / 4W),
    // which conflicts with the daemon's own thermal management.  Disable the
    // external PROCHOT# response (MSR 0x1FC bit 0), since we handle throttling
    // ourselves via aggression levels, temperature overlay, and core offlining.
    // This is a standard Intel MSR — restored on exit.
    bool msr_ok = msr_available();
    if (msr_ok) {
        unsigned long long msr_1fc = read_msr(0, 0x1FC);
        if (msr_1fc & 1) {
            write_msr(0, 0x1FC, msr_1fc & ~1ULL);
            syslog(LOG_INFO, "external PROCHOT# response disabled (MSR 0x1FC bit 0)");
        }
    }

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
    int prev_max_perf = 100;       // previous max_perf_pct (for smoothing)
    EppLevel prev_epp_p = EppLevel::BalancePerformance;  // prev EPP (for hysteresis)
    EppLevel prev_epp_e = EppLevel::BalancePerformance;

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
        smoothed_gpu_w = SMOOTH_ALPHA * gpu_w + (1.0 - SMOOTH_ALPHA) * smoothed_gpu_w;



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
        if (msr_ok) track_perf_limits(pl_tc, 0);

        // ── Temperature reading ──
        double max_temp = read_max_core_temp(s.thermal);

        // ── Build optimizer inputs ──
        OptimizerInputs opt_inputs{};
        opt_inputs.pl1_w              = pl1_w;
        opt_inputs.gpu_w              = smoothed_gpu_w;
        opt_inputs.have_gpu           = have_gpu;
        opt_inputs.temp_c             = max_temp;
        opt_inputs.gpu_c0_pct         = s.gpu.c0_pct;
        opt_inputs.gpu_throttling     = throttling;

        opt_inputs.total_core_groups  = (int)s.cpu.core_groups.size();
        opt_inputs.have_coretemp      = !s.thermal.coretemp_dir.empty();
        opt_inputs.config             = &default_config;
        // Pass previous state for smoothing/hysteresis
        opt_inputs.prev_max_perf      = prev_max_perf;
        opt_inputs.prev_epp_p         = prev_epp_p;
        opt_inputs.prev_epp_e         = prev_epp_e;

        // ── Debug: log optimizer inputs and utility for diagnostics ──
        if (opt_inputs.temp_c >= 0) {
            syslog(LOG_DEBUG, "OPT-IN: pl1=%.1f gpu=%.1f temp=%.1f c0=%.1f throttle=%d",
                   opt_inputs.pl1_w, opt_inputs.gpu_w, opt_inputs.temp_c,
                   opt_inputs.gpu_c0_pct, opt_inputs.gpu_throttling);
        }

        // ── Solve optimization problem ──
        OptimizerResult opt = solve(opt_inputs);
        int aggression = aggression_from_weights(opt.weight_thermal,
                                                 opt.weight_throttle,
                                                 opt.weight_performance);

        // ── Apply RAPL limits (core budget from optimization) ──
        if (s.core.valid) {
            // PL4 is a per-domain peak power clamp. If the core domain has a PL4 limit,
            // the core RAPL budget must not exceed it — otherwise the core hits PL4
            // and triggers MSR 0x6B0 bit 11 (Package PL4/Peak perf limit reason).
            double core_limit = opt.core_limit_r;
            if (s.core.pl4_w > 0)
                core_limit = std::min(core_limit, s.core.pl4_w);
            rapl_set_power_limit(s.core, core_limit);
        }
        if (s.uncore.valid)
            rapl_set_power_limit(s.uncore, s.uncore.max_w > 0 ? s.uncore.max_w : 0.0);

        // ── MMIO package PL1 ──
        // Raise MMIO PL1 to match MSR PL1 so it doesn't become a bottleneck.
        // Cap at package PL4 if present — exceeding PL4 triggers MSR 0x6B0 bit 11.
        {
            double effective_pl1 = pl1_w;
            if (s.pkg.pl4_w > 0)
                effective_pl1 = std::min(effective_pl1, s.pkg.pl4_w);
            if (s.pkg_mmio.valid) {
                long long mmio_now = 0;
                sysfs_read_attr(s.pkg_mmio.path, "constraint_0_power_limit_uw", mmio_now);
                long long target = (long long)(effective_pl1 * 1e6);
                if (mmio_now < target)
                    rapl_set_power_limit(s.pkg_mmio, effective_pl1);
            }
        }

        // ── PROCHOT# response (clear every cycle) ──
        clear_prochot_msr(msr_ok);

        // ── CPU frequency control (from optimizer) ──
        sysfs_write_int(PSTATE_MAX, opt.max_perf_pct);
        sysfs_write_int(PSTATE_NOTURBO, opt.no_turbo);
        int min_perf = (opt.keep_groups > 0 && opt.keep_groups < (int)s.cpu.core_groups.size())
                       ? 0 : s.saved.min_perf;
        sysfs_write_int(PSTATE_MIN, min_perf);

        // ── EPP (from optimizer) ──
        cpu_set_epp(s.cpu, epp_to_string(opt.epp_p), epp_to_string(opt.epp_e));

        // ── CPU hotplug (core offlining) ──
        apply_hotplug(s.cpu, aggression, opt.temp_c, smoothed_gpu_w);

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
            if (msr_ok) {
                unsigned long long msr = read_msr(0, 0x6B0);
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
            if (opt.temp_c >= 0)
                snprintf(temp_buf, sizeof(temp_buf), "  temp=%.0fC", opt.temp_c);
            // C0 residency percentage (only when path is available)
            char c0_buf[32] = "";
            if (!s.gpu.c0_residency_path.empty())
                snprintf(c0_buf, sizeof(c0_buf), "  c0=%d%%", (int)(s.gpu.c0_pct * 100.0));
            std::string epp_str = epp_to_string(opt.epp_p);
            if (opt.epp_e != opt.epp_p) epp_str += "(" + std::string(epp_to_string(opt.epp_e)) + ")";
            syslog(LOG_INFO, "[%s] pkg=%.1fW core=%.1fW gpu=%.1fW(gpu_sm=%.1fW) "
                   "pl1=%.1fW core_lmt=%.1fW max_perf=%d%% no_turbo=%d epp=%s%s%s%s",
                   state, pkg_w, core_w, gpu_w, smoothed_gpu_w,
                   pl1_w, opt.core_limit_r, opt.max_perf_pct, opt.no_turbo, epp_str.c_str(),
                   temp_buf, throttle_summary.c_str(), c0_buf);
            last_aggression = aggression;
        }

        // ── Save state for next cycle smoothing ──
        prev_max_perf = opt.max_perf_pct;
        prev_epp_p = opt.epp_p;
        prev_epp_e = opt.epp_e;
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
    if (msr_ok) {
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
