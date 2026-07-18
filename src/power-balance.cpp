// power-balance.cpp — GPU-first power balancer daemon
// Principle: the GPU must never throttle. Everything else (CPU frequency,
// RAPL limits, turbo, EPP) is aggressively constrained to ensure the GPU
// always has the power headroom it needs.
// Compile: g++ -std=c++17 -O2 power-balance.cpp -o power-balance

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
// GPU activity thresholds (TODO #1 done):
// Primary: C0 residency → c0_pct (0.0–1.0) over each 500ms window
//   c0_pct >= 0.70 → aggression 2 (disable turbo, hard power cap)
//   c0_pct >= 0.30 → aggression 1 (throttle EPP, cap max_perf)
//   c0_pct  <  0.30 → aggression 0 (idle — all cores online, full turbo)
// Fallback (platforms without C0 residency sysfs):
//   GPU_ACTIVE_W  (3 W)  → aggression 1 (throttle EPP)
//   GPU_HEAVY_W (15 W)  → aggression 2 (disable turbo)
// When residency is available, GPU_HEAVY_W is only used for max_perf
// scaling (proportional load) — it no longer drives aggression or
// no-turbo as an independent threshold.
//
// GT1 power minimization (TODO #3 done):
//   GT0 (graphics) gets normal profiles (base, power_saving) via set_gt0_profile()
//   GT1 (media) locked in power_saving via set_gt1_profile() — always kept idle
//   saved_profile_gt1 tracks GT1's original profile for restore on exit
//
// Dynamic RAPL domains (TODO #2 done):
//   all_rapl_domains tracks every discovered subdomain (dram, pp0, etc.)
//   Core budget applied to all non-uncore domains; uncore left unlimited
static constexpr double   GPU_ACTIVE_W         = 3.0;
static constexpr double   GPU_HEAVY_W          = 15.0;
static constexpr double   SMOOTH_ALPHA         = 0.3;
static constexpr double   HEADROOM_MARGIN_W    = 2.0;   // PL1 headroom always kept
static constexpr double   CRITICAL_CPU_MAX_W   = 8.0;   // max core budget when GPU is throttling
static constexpr double   CRITICAL_MAX_PERF    = 20.0;  // max_perf_pct when GPU throttling
static constexpr double   ACTIVE_MAX_PERF      = 50.0;  // min max_perf_pct when GPU active
static constexpr long     PP0_TIME_WINDOW_US   = 500;
static constexpr int      MIN_CORE_GROUPS      = 2;     // minimum core groups to keep online
static constexpr double   WEAK_CHARGER_RATIO   = 0.5;   // PL1 = charger_max_watts * ratio

// ── Sysfs helpers ──

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string content;
    std::getline(f, content);
    content.erase(0, content.find_first_not_of(" \t\n\r"));
    content.erase(content.find_last_not_of(" \t\n\r") + 1);
    return content;
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (f.is_open()) f << content;
}

template<typename T>
static bool read_attr(const std::string& dir, const std::string& name, T& out) {
    std::string val = read_file(dir + "/" + name);
    if (val.empty()) return false;
    std::istringstream iss(val);
    return (bool)(iss >> out);
}

static void write_str(const std::string& path, const std::string& val) {
    write_file(path, val);
}

static void write_int(const std::string& path, long long val) {
    write_file(path, std::to_string(val));
}

// ── Data-only structs ──
// All runtime state lives in one global (SystemState).  Each sub-struct owns
// its own data; free functions below operate on the structs by reference.

// RAPL powercap domain
struct RaplDomain {
    std::string path;          // /sys/class/powercap/intel-rapl/domain-N
    bool        valid = false;
    std::string domain_type;   // "core", "uncore", "dram", "pp0", etc.
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

    // GT1 power minimization (TODO #3)
    // GT1 (media engine) is kept in power_saving at all times — it's a
    // major contributor to GT PL1/PL2/PL4 throttling events.
    bool        gt1_force_power_saving = true;

    // C0 residency-based GPU activity tracking (TODO #1)
    // Replaces hardcoded GPU_ACTIVE_W/GPU_HEAVY_W thresholds with
    // residency percentage, which is much more portable across GPUs.
    bool        has_c0_residency = false;  // true when c0_residency_path is populated
    std::string c0_residency_path; // gt0/activity/c0_residency_ms
    long long   last_c0_residency_us = 0; // microseconds, last read value
    long long   _last_c0_time_us = 0;     // steady_clock time of last read (internal)
    double      c0_pct = 0.0;              // 0.0–1.0, residency fraction over measurement window
};

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

// Charger state: weak-charger detection cache
struct ChargerState {
    bool   weak        = false;
    double max_watts   = 0;
    std::string note;
    bool   gpu_freq_capped = false;
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
    ChargerState charger;
    SavedState  saved;
};

// ── Throttle/limit reason tables ──
static const char* THROTTLE_REASONS[] = {
    "pl1", "pl2", "pl4", "prochot", "thermal", "ratl", "vr_tdc", "vr_thermalert", nullptr
};
static const char* THROTTLE_FILES[] = {
    "reason_pl1", "reason_pl2", "reason_pl4",
    "reason_prochot", "reason_thermal", "reason_ratl",
    "reason_vr_tdc", "reason_vr_thermalert", nullptr
};

static const struct { const char* name; int bit; } PERF_LIMIT_REASONS[] = {
    {"PROCHOT",          0},
    {"Thermal",          1},
    {"Current(EDP)",     2},
    {"Power(PL1)",       3},
    {"Platform",         4},
    {"Autonomous",       5},
    {"VR_Thermal",       6},
    {"HTC",              7},
    {"Core/Cache",       8},
    {"Amps",             9},
    {"PROCHOT_Deassert",10},
    {"PL4/Peak",        11},
    {"PkgPwrLatch",     12},
    {"Clipping",        13},
    {nullptr, 0}
};

// ── RAPL helpers ──

static void rapl_set_power_limit(const RaplDomain& d, double watts_w, long long time_us = PP0_TIME_WINDOW_US) {
    if (!d.valid) return;
    write_int(d.path + "/constraint_0_power_limit_uw", (long long)(watts_w * 1e6));
    write_int(d.path + "/constraint_0_time_window_us", time_us);
}

static void rapl_set_enabled(const RaplDomain& d, bool on) {
    if (!d.valid) return;
    write_file(d.path + "/enabled", on ? "1" : "0");
}

// Set power limit on ALL RAPL domains (pkg excluded — that's PL1, not a subdomain).
// Used during initialization to enable all domains, and during cleanup to reset them.
static void rapl_set_all(const std::vector<RaplDomain>& domains, double watts_w) {
    for (auto& d : domains) rapl_set_power_limit(d, watts_w);
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
            std::string rname = read_file(path + "/name");
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
                        std::string sr = read_file(sp + "/name");
                        if (sr.empty()) continue;
                        RaplDomain sub;
                        sub.path = sp;
                        sub.domain_type = sr;
                        sub.valid = true;
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

    // Log discovered domains
    syslog(LOG_INFO, "RAPL: pkg=%s pkg_mmio=%s core=%s uncore=%s dram=%s",
           s.pkg.path.c_str(),
           s.pkg_mmio.path.empty() ? "n/a" : s.pkg_mmio.path.c_str(),
           s.core.path.c_str(),
           s.uncore.path.empty() ? "n/a" : s.uncore.path.c_str(),
           s.core.path.empty() ? "n/a" : s.core.path.c_str());
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
        if (read_file(base0 + "/freq0/cur_freq") == "") continue;
        s.gpu.gt0 = base0;
        s.gpu.idle_path = base0 + "/gtidle/idle_status";
        // C0 residency path for activity-based thresholds (TODO #1)
        std::string c0_path = base0 + "/activity/c0_residency_ms";
        if (read_file(c0_path) != "") {
            s.gpu.c0_residency_path = c0_path;
            s.gpu.has_c0_residency = true;
        }
        std::string base1 = "/sys/class/drm/" + card + "/device/tile0/gt1";
        if (read_file(base1 + "/freq0/cur_freq") != "") s.gpu.gt1 = base1;
        break;
    }
    closedir(drm);
}

static bool gpu_is_active(const GpuState& g) {
    if (g.gt0.empty()) return false;
    std::string s = read_file(g.idle_path);
    return s.find("c6") == std::string::npos;
}

// Read C0 residency and compute residency percentage over the last measurement window.
// C0 residency is a cumulative counter (microseconds in active state). By comparing
// deltas against elapsed wall-clock time, we get the fraction of time the GPU was active.
static void gpu_read_c0_residency(GpuState& g) {
    if (g.c0_residency_path.empty()) return;
    std::string val = read_file(g.c0_residency_path);
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
static int gpu_c0_aggression(double c0_pct) {
    if (c0_pct >= 0.70) return 2;     // >= 70% active → heavy (disable turbo)
    if (c0_pct >= 0.30) return 1;     // >= 30% active → active (throttle EPP)
    return 0;                          // < 30% → idle
}

static int gpu_throttle_events(const GpuState& g, GpuThrottleCounters* counters) {
    if (g.gt0.empty()) return 0;
    std::string throttle_dir = g.gt0 + "/freq0/throttle";
    int any = 0;
    for (int i = 0; THROTTLE_FILES[i]; ++i) {
        int v = 0;
        read_attr(throttle_dir, THROTTLE_FILES[i], v);
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
// This is the "normal" GPU profile — base, performance, power_saving, etc.
static void set_gt0_profile(const GpuState& g, const std::string& profile) {
    if (g.gt0.empty()) return;
    std::string path = g.gt0 + "/freq0/power_profile";
    std::string cur = read_file(path);
    if (cur.empty()) return;
    if (cur.find(profile) == std::string::npos || cur.find("[" + profile + "]") == std::string::npos)
        write_str(path, profile);
}

// Apply profile to GT1 (media engine) — only when explicitly requested.
// By default (gt1_force_power_saving=true), GT1 stays in power_saving.
// This function overrides that default for controlled profile changes.
static void set_gt1_profile(const GpuState& g, const std::string& profile) {
    if (g.gt1.empty()) return;
    std::string path = g.gt1 + "/freq0/power_profile";
    std::string cur = read_file(path);
    if (cur.empty()) return;
    if (cur.find(profile) == std::string::npos || cur.find("[" + profile + "]") == std::string::npos)
        write_str(path, profile);
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

    for (auto& p : c.pcore_epp_paths) write_str(p, p_val);
    for (auto& p : c.ecore_epp_paths) write_str(p, e_val);

    syslog(LOG_INFO, "EPP -> P:%s  E:%s", p_val.c_str(), e_val.c_str());
}

static void cpu_set_epp_all(const CpuState& c, const std::string& val) {
    for (auto& p : c.pcore_epp_paths) write_str(p, val);
    for (auto& p : c.ecore_epp_paths) write_str(p, val);
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
        if (read_file(epp) == "") continue;
        read_attr("/sys/devices/system/cpu/" + name + "/cpufreq", "cpuinfo_max_freq", max_freq);
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
        std::string s = read_file("/sys/devices/system/cpu/cpu" + std::to_string(g.cpus[0]) + "/online");
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
        std::string siblings = read_file(topo + "/core_cpus_list");
        if (siblings.empty()) siblings = read_file(topo + "/thread_siblings_list");
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
        if (!read_attr("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq", "cpuinfo_max_freq", mf)) continue;
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
            if (read_attr(cpufreq_dir, "cpuinfo_max_freq", mf))
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
        std::string online_str = read_file("/sys/devices/system/cpu/cpu" + std::to_string(g.cpus[0]) + "/online");
        g.saved_online = (online_str != "0");
    }

    int p = 0, e = 0;
    for (auto& g : c.core_groups) { if (g.is_pcore) p++; else e++; }
    syslog(LOG_INFO, "topology: %d P-core groups, %d E-core groups (%zu logical CPUs)",
           p, e, c.core_groups.size());
}

// Target logical CPUs to keep online based on aggression + temperature.
// Returns the number of CoreGroups to keep online.
static int compute_keep_groups(int aggression, double temp_c, double /*gpu_w*/, bool weak_charger = false) {
    // If charger is weak, cap aggressively
    if (weak_charger) return 2;
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

static void apply_hotplug(CpuState& c, int aggression, double temp_c, double gpu_w, bool weak_charger) {
    if (c.core_groups.empty()) return;

    if (c.hotplug_settle > 0) { c.hotplug_settle--; return; }

    int total = (int)c.core_groups.size();
    int target = compute_keep_groups(aggression, temp_c, gpu_w, weak_charger);
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
        std::string s = read_file("/sys/devices/system/cpu/cpu" + std::to_string(g.cpus[0]) + "/online");
        bool is_online = (s != "0");
        if (is_online && !should_online[i]) {
            for (int cp : g.cpus)
                write_int("/sys/devices/system/cpu/cpu" + std::to_string(cp) + "/online", 0);
            changed = true;
        } else if (!is_online && should_online[i]) {
            for (int cp : g.cpus)
                write_int("/sys/devices/system/cpu/cpu" + std::to_string(cp) + "/online", 1);
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
                write_int("/sys/devices/system/cpu/cpu" + std::to_string(cp) + "/online", 1);
        }
    }
    syslog(LOG_INFO, "restored CPU online state");
}

// ── Saved state (restored on exit) ──

static bool msr_available();
static unsigned long long read_msr(int cpu, unsigned int msr_addr);
static bool write_msr(int cpu, unsigned int msr_addr, unsigned long long val);

static void save_cpu_state(SystemState& s) {
    read_attr(PSTATE_DIR, "max_perf_pct", s.saved.max_perf);
    read_attr(PSTATE_DIR, "min_perf_pct", s.saved.min_perf);
    read_attr(PSTATE_DIR, "no_turbo", s.saved.no_turbo);
    if (!s.cpu.pcore_epp_paths.empty())
        s.saved.epp = read_file(s.cpu.pcore_epp_paths[0]);
    s.gpu.saved_profile_gt0 = s.gpu.gt0.empty() ? "" : read_file(s.gpu.gt0 + "/freq0/power_profile");
    s.gpu.saved_profile_gt1 = s.gpu.gt1.empty() ? "" : read_file(s.gpu.gt1 + "/freq0/power_profile");
    if (!s.gpu.gt0.empty()) read_attr(s.gpu.gt0 + "/freq0", "max_freq", s.gpu.saved_max_freq_gt0);
    if (!s.gpu.gt1.empty()) read_attr(s.gpu.gt1 + "/freq0", "max_freq", s.gpu.saved_max_freq_gt1);
    s.saved.msr_1fc = msr_available() ? read_msr(0, 0x1FC) : 0;
    syslog(LOG_INFO, "saved state: max_perf=%d  min_perf=%d  no_turbo=%d  epp=%s",
           s.saved.max_perf, s.saved.min_perf, s.saved.no_turbo, s.saved.epp.c_str());
}

static void restore_cpu_state(const SystemState& s) {
    write_int(PSTATE_MAX, s.saved.max_perf);
    write_int(PSTATE_MIN, s.saved.min_perf);
    write_int(PSTATE_NOTURBO, s.saved.no_turbo);
    if (!s.saved.epp.empty())
        cpu_set_epp_all(s.cpu, s.saved.epp);
    if (!s.gpu.saved_profile_gt0.empty()) write_str(s.gpu.gt0 + "/freq0/power_profile", s.gpu.saved_profile_gt0);
    if (!s.gpu.saved_profile_gt1.empty()) write_str(s.gpu.gt1 + "/freq0/power_profile", s.gpu.saved_profile_gt1);
    if (s.gpu.saved_max_freq_gt0 > 0) write_int(s.gpu.gt0 + "/freq0/max_freq", s.gpu.saved_max_freq_gt0);
    if (s.gpu.saved_max_freq_gt1 > 0) write_int(s.gpu.gt1 + "/freq0/max_freq", s.gpu.saved_max_freq_gt1);
    if (s.saved.msr_1fc) write_msr(0, 0x1FC, s.saved.msr_1fc);
    syslog(LOG_INFO, "restored CPU and GPU state");
}

// ── MSR helpers ──

static bool msr_available() {
    int fd = open("/dev/cpu/0/msr", O_RDONLY);
    if (fd < 0) return false;
    close(fd);
    return true;
}

static unsigned long long read_msr(int cpu, unsigned int msr_addr) {
    char path[64];
    std::snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned long long val = 0;
    if (lseek(fd, msr_addr, SEEK_SET) == (off_t)msr_addr) {
        if (read(fd, &val, sizeof(val)) != sizeof(val)) val = 0;
    }
    close(fd);
    return val;
}

static bool write_msr(int cpu, unsigned int msr_addr, unsigned long long val) {
    char path[64];
    std::snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return false;
    bool ok = false;
    if (lseek(fd, msr_addr, SEEK_SET) == (off_t)msr_addr) {
        ok = (write(fd, &val, sizeof(val)) == sizeof(val));
    }
    close(fd);
    return ok;
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

// ── Charger health ──
// Detect insufficient charger conditions that cause EC PROCHOT assertion.
struct ChargerInfo {
    bool   weak       = false;
    double max_watts  = 0;    // max delivered watts from USB-C PD contract
    std::string note;          // human-readable description
};

static ChargerInfo check_charger() {
    bool ac_online = false;
    ChargerInfo ci;

    DIR* dir = opendir("/sys/class/power_supply");
    if (!dir) return ci;
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        std::string base = "/sys/class/power_supply/" + name;
        std::string type = read_file(base + "/type");
        if (type.empty()) continue;

        if (type.find("Mains") != std::string::npos) {
            if (read_file(base + "/online") == "1") ac_online = true;
        } else if (type.find("BAT") != std::string::npos) {
            std::string st = read_file(base + "/status");
            if (ac_online && (st == "Discharging" || st == "Not charging")) {
                ci.note = "insufficient charger: battery " + st;
                ci.weak = true;
            }
        } else if (type.find("USB") != std::string::npos) {
            if (read_file(base + "/online") == "1") {
                long long cur_max = 0, vol_max = 0;
                read_attr(base, "current_max", cur_max);
                read_attr(base, "voltage_max", vol_max);
                if (vol_max > 0 && cur_max > 0) {
                    double max_w = (double)vol_max * cur_max / 1e12;
                    ci.max_watts = std::max(ci.max_watts, max_w);
                    if (max_w > 0 && max_w < 45.0) {
                        if (!ci.note.empty()) ci.note += "; ";
                        ci.note += std::to_string((int)max_w) + "W USB-C charger";
                        ci.weak = true;
                    }
                }
            }
        }
    }
    closedir(dir);
    return ci;
}

static void charger_check(ChargerState& c) {
    ChargerInfo chg = check_charger();
    if (chg.weak != c.weak) {
        if (chg.weak)
            syslog(LOG_WARNING, "weak charger: %s — enabling power saving", chg.note.c_str());
        else
            syslog(LOG_INFO, "adequate charger detected — disabling power saving");
    }
    c.weak = chg.weak;
    if (chg.max_watts > 0) c.max_watts = chg.max_watts;
    if (chg.weak) c.note = chg.note;
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
        if (read_file(path + "/name") == "coretemp") {
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
        if (read_file(path + "/type") == "intel_powerclamp") {
            t.powerclamp_dev = path;
            read_attr(t.powerclamp_dev, "cur_state", t.saved_powerclamp_state);
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
            std::string val = read_file(t.coretemp_dir + "/" + fn);
            if (!val.empty()) {
                double tval = std::stod(val) / 1000.0;
                if (tval > max_temp) max_temp = tval;
            }
        }
    }
    closedir(dir);
    return max_temp;
}

// ── Thermal constraints ──
// Temperature-aware overlay: independently constrains CPU controls based on
// the hottest core.  These are applied as the most-conservative-or-equal
// override of the aggression-based values.
struct ThermalConstraints {
    int    max_perf  = 100;       // thermal cap on max_perf_pct
    int    no_turbo  = 0;         // force no_turbo?
    int    epp_tier  = 0;         // 0=aggression-only, 1=balance_power, 2=power
    double headroom_extra = 0;    // extra PL1 headroom (W) to let CPU cool
    double temp_c    = -1;        // measured max core temp
};

static ThermalConstraints compute_thermal(double temp_c) {
    ThermalConstraints tc;
    tc.temp_c = temp_c;
    if (temp_c < 0) return tc;  // no sensor — no constraints

    // thermal_pressure: 0.0 (cold) → 1.0 (at 90°C or above)
    double pressure = 0.0;
    if (temp_c >= 90.0) {
        pressure = 1.0;
    } else if (temp_c > 70.0) {
        pressure = (temp_c - 70.0) / 20.0;  // linear from 70→90
    }

    // ── max_perf cap ──
    // At 90°C: max 20%.  At 70°C: no cap.  Linear in between.
    tc.max_perf = 100 - (int)(pressure * 80.0);
    tc.max_perf = std::max(tc.max_perf, 20);

    // ── no_turbo ──
    // Force no_turbo at 82°C+
    if (temp_c >= 82.0) tc.no_turbo = 1;

    // ── EPP tier ──
    if      (temp_c >= 85.0) tc.epp_tier = 2;  // power
    else if (temp_c >= 80.0) tc.epp_tier = 1;  // balance_power

    // ── Extra headroom margin ──
    // Add up to 5W of extra headroom as temperature rises, starving the core
    // budget so the package power drops and cores cool down.
    tc.headroom_extra = pressure * 5.0;

    return tc;
}

// ── Sampling ──
struct Sample {
    long long energy_uj = -1;
    std::chrono::steady_clock::time_point time;
};

static Sample read_energy(const std::string& dir) {
    Sample s;
    read_attr(dir, "energy_uj", s.energy_uj);
    s.time = std::chrono::steady_clock::now();
    return s;
}

static double compute_power_w(const Sample& prev, const Sample& cur) {
    if (prev.energy_uj < 0 || cur.energy_uj < 0) return 0;
    long long delta_uj = cur.energy_uj - prev.energy_uj;
    if (delta_uj < 0) return -1;
    auto delta_us = std::chrono::duration_cast<std::chrono::microseconds>(cur.time - prev.time).count();
    if (delta_us <= 0) return -1;
    return (double)delta_uj / delta_us;
}

static double read_pl1_w(const RaplDomain& d) {
    long long uw = 0;
    read_attr(d.path, "constraint_0_power_limit_uw", uw);
    if (uw > 0) return uw / 1e6;
    return DEFAULT_PL1_W;
}

static double round_to_125mw(double watts) {
    if (watts < 0) return 0;
    return std::round(watts / 0.125) * 0.125;
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

// Compute aggression level: combine throttle-based and C0-residency-based signals.
// Returns the higher of the two aggression levels so that either signal can
// escalate CPU throttling independently.
static int compute_aggression(double smoothed_gpu_w, bool throttling, bool gpu_active,
                              int residency_agg) {
    int agg = 0;
    if (throttling || (gpu_active && smoothed_gpu_w > GPU_ACTIVE_W)) agg = 1;
    if (throttling) agg = 2;
    // Residency-based aggression can independently escalate (no reliance on wattage)
    if (residency_agg > agg) agg = residency_agg;
    return agg;
}

// Compute the core power budget and apply RAPL limits
static void apply_rapl(const SystemState& s, double pl1_w, double smoothed_gpu_w,
                       double core_budget_w, int aggression,
                       const ThermalConstraints& thermal) {
    double headroom = HEADROOM_MARGIN_W + thermal.headroom_extra;

    // Cap budget at CRITICAL_CPU_MAX_W when GPU is throttling
    if (aggression >= 2)
        core_budget_w = std::min(CRITICAL_CPU_MAX_W, pl1_w - smoothed_gpu_w - headroom);
    else
        core_budget_w = pl1_w - smoothed_gpu_w - headroom;
    core_budget_w = std::max(core_budget_w, 0.0);
    double core_limit_r = round_to_125mw(core_budget_w);

    // Apply RAPL limits:
    //   - All non-uncore subdomains (core, dram, pp0, etc.) → core budget
    //   - Uncore → unlimited (0 = disabled) for GPU headroom
    for (auto& d : s.all_rapl_domains) {
        if (d.domain_type == "uncore")
            rapl_set_power_limit(d, 0.0);  // unlimited
        else
            rapl_set_power_limit(d, core_limit_r);
    }

    return;  // caller uses core_budget_w / core_limit_r
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
        set_gt0_profile(s.gpu, "base");
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
    // The hardware max_power_uw is conservative — actual VR can handle more.
    // Higher PL1 gives the GPU VR more peak current headroom, avoiding PL4 events.
    rapl_set_power_limit(s.pkg, pl1_w);
    if (s.pkg_mmio.valid)
        rapl_set_power_limit(s.pkg_mmio, pl1_w);

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
    int last_aggression = -1; // 0=idle, 1=active, 2=throttling

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

        // ── C0 residency (activity-based thresholds, TODO #1) ──
        gpu_read_c0_residency(s.gpu);
        int residency_agg = gpu_c0_aggression(s.gpu.c0_pct);

        // ── Track CPU MSR perf limit reasons ──
        if (msr_ok) track_perf_limits(pl_tc, 0);

        // ── Determine aggression level ──
        int aggression = compute_aggression(smoothed_gpu_w, throttling, active, residency_agg);

        // ── Temperature reading ──
        double max_temp = read_max_core_temp(s.thermal);
        ThermalConstraints thermal = compute_thermal(max_temp);

        // ── Weak charger detection ──
        if (iterations % 20 == 10) {
            charger_check(s.charger);
        }
        bool weak_charger = s.charger.weak;

        // ── Calculate CPU core budget and apply RAPL ──
        apply_rapl(s, pl1_w, smoothed_gpu_w, 0, aggression, thermal);
        // Recalculate core_budget_w inside apply_rapl; need to redo here for logging
        double headroom = HEADROOM_MARGIN_W + thermal.headroom_extra;
        double core_budget_w;
        if (aggression >= 2)
            core_budget_w = std::min(CRITICAL_CPU_MAX_W, pl1_w - smoothed_gpu_w - headroom);
        else
            core_budget_w = pl1_w - smoothed_gpu_w - headroom;
        core_budget_w = std::max(core_budget_w, 0.0);
        double core_limit_r = round_to_125mw(core_budget_w);

        // Re-apply with correct budget (gather_samples already computed it)
        if (s.core.valid)
            rapl_set_power_limit(s.core, core_limit_r);
        if (s.uncore.valid)
            rapl_set_power_limit(s.uncore, 0.0);

        // ── MMIO package PL1 ──
        // Raise MMIO PL1 to match MSR PL1 so it doesn't become a bottleneck
        if (s.pkg_mmio.valid) {
            long long mmio_now = 0;
            read_attr(s.pkg_mmio.path, "constraint_0_power_limit_uw", mmio_now);
            long long target = (long long)(pl1_w * 1e6);
            if (mmio_now < target)
                rapl_set_power_limit(s.pkg_mmio, pl1_w);
        }

        // ── PROCHOT# response (clear every cycle) ──
        clear_prochot_msr(msr_ok);

        // ── CPU frequency capping ──
        // Combine aggression-based cap with thermal cap (most conservative wins).
        // When C0 residency is available, GPU_HEAVY_W is used only as a fallback
        // for max_perf scaling (proportional to actual power).  Without residency,
        // it drives the aggression level directly (wattage-threshold model).
        int max_perf = 100;
        if (aggression >= 2) {
            max_perf = (int)CRITICAL_MAX_PERF;
        } else if (aggression >= 1) {
            double gpu_load_w = smoothed_gpu_w;
            // If residency is available, scale max_perf by actual GPU power.
            // If residency is absent, GPU_HEAVY_W was already used in compute_aggression()
            // to raise agg→1, so we apply the same proportional cap for smooth transition.
            double ratio = std::min(gpu_load_w / GPU_HEAVY_W, 1.0);
            max_perf = 100 - (int)(ratio * (100.0 - ACTIVE_MAX_PERF));
            max_perf = std::max(max_perf, (int)ACTIVE_MAX_PERF);
        }
        max_perf = std::min(max_perf, thermal.max_perf);
        write_int(PSTATE_MAX, max_perf);

        // ── CPU turbo control ──
        // No-turbo is driven by aggression level (which comes from residency
        // when available, or wattage threshold when residency is absent).
        // GPU_HEAVY_W no longer acts as an independent hard cutoff.
        int no_turbo = 0;
        if (aggression >= 2) no_turbo = 1;
        no_turbo = std::max(no_turbo, thermal.no_turbo);
        write_int(PSTATE_NOTURBO, no_turbo);

        // ── EPP — per-cluster: P-cores get throttled first, E-cores stay responsive ──
        std::string p_val = "balance_performance";
        std::string e_val = "balance_performance";
        if (weak_charger) {
            p_val = "power"; e_val = "power";
        } else if (aggression >= 2)       { p_val = "power";             e_val = "balance_power"; }
        else if (aggression >= 1)  { p_val = "balance_power";     e_val = "balance_performance"; }
        if (thermal.epp_tier >= 2) { p_val = "power";             if (thermal.temp_c < 90.0) e_val = "balance_power"; else e_val = "power"; }
        else if (thermal.epp_tier >= 1) { p_val = "balance_power"; e_val = "balance_performance"; }
        cpu_set_epp(s.cpu, p_val, e_val);

        // ── CPU hotplug (core offlining) ──
        apply_hotplug(s.cpu, aggression, thermal.temp_c, smoothed_gpu_w, weak_charger);
        int min_perf = (s.cpu.keep_groups_target > 0 && s.cpu.keep_groups_target < (int)s.cpu.core_groups.size())
                       ? 0 : s.saved.min_perf;
        write_int(PSTATE_MIN, min_perf);

        // ── Weak charger constraints ──
        // Apply aggressive power saving when the charger can't keep up.
        // These are applied AFTER all normal controls so they always win.
        // On transition out of weak charger mode, restore GPU freq, profile, uncore, powerclamp.
        double effective_pl1_w = pl1_w;
        if (weak_charger && s.charger.max_watts > 0) {
            if (!s.charger.gpu_freq_capped) {
                s.charger.gpu_freq_capped = true;
                // Switch GPU to power_saving on entry (GT1 stays in power_saving)
                set_gt0_profile(s.gpu, "power_saving");
                set_gt1_profile(s.gpu, "power_saving");
                // Set intel_powerclamp to 20% idle injection
                if (!s.thermal.powerclamp_dev.empty())
                    write_int(s.thermal.powerclamp_dev + "/cur_state", 20);
            }

            // Lower package PL1 to ~50% of charger capacity
            double weak_pl1 = s.charger.max_watts * WEAK_CHARGER_RATIO;
            if (weak_pl1 < pl1_w) {
                effective_pl1_w = weak_pl1;
                rapl_set_power_limit(s.pkg, weak_pl1);
                if (s.pkg_mmio.valid)
                    rapl_set_power_limit(s.pkg_mmio, weak_pl1);
                double weak_budget = weak_pl1 - smoothed_gpu_w - headroom;
                weak_budget = std::max(weak_budget, 0.0);
                weak_budget = std::min(weak_budget, core_budget_w);
                core_budget_w = weak_budget;
                core_limit_r = round_to_125mw(core_budget_w);
                // Apply weak budget to all non-uncore domains, uncore limited to 3W
                for (auto& d : s.all_rapl_domains) {
                    if (d.domain_type == "uncore")
                        rapl_set_power_limit(d, 3.0);
                    else
                        rapl_set_power_limit(d, core_limit_r);
                }
            }

            no_turbo = 1;
            write_int(PSTATE_NOTURBO, no_turbo);

            max_perf = std::min(max_perf, 20);
            write_int(PSTATE_MAX, max_perf);

            write_int(PSTATE_MIN, 0);

            write_int(s.gpu.gt0 + "/freq0/max_freq", 800);
            if (!s.gpu.gt1.empty())
                write_int(s.gpu.gt1 + "/freq0/max_freq", 100);

            if (s.uncore.valid)
                rapl_set_power_limit(s.uncore, 3.0);
        } else if (s.charger.gpu_freq_capped) {
            s.charger.gpu_freq_capped = false;
            // Restore GPU: GT0 to base, GT1 from saved profile, freq, RAPL, powerclamp
            set_gt0_profile(s.gpu, "base");
            if (!s.gpu.saved_profile_gt1.empty())
                set_gt1_profile(s.gpu, s.gpu.saved_profile_gt1);
            if (s.gpu.saved_max_freq_gt0 > 0) write_int(s.gpu.gt0 + "/freq0/max_freq", s.gpu.saved_max_freq_gt0);
            if (s.gpu.saved_max_freq_gt1 > 0) write_int(s.gpu.gt1 + "/freq0/max_freq", s.gpu.saved_max_freq_gt1);
            rapl_set_all(s.all_rapl_domains, 0.0);  // unlimited
            if (!s.thermal.powerclamp_dev.empty())
                write_int(s.thermal.powerclamp_dev + "/cur_state", s.thermal.saved_powerclamp_state);
        }

        // ── Log state changes ──
        if (aggression != last_aggression || iterations % 20 == 0) {
            const char* state = "idle";
            if (aggression == 1) state = "active";
            if (aggression == 2) state = "balance-throttle";
            if (weak_charger) state = "power-save";
            // Build GPU throttle event summary
            std::string gpu_thr;
            if (have_gpu && gpu_tc.total_events > 0) {
                for (int i = 0; THROTTLE_REASONS[i]; ++i) {
                    if (gpu_tc.events[i] > 0) {
                        if (!gpu_thr.empty()) gpu_thr += " ";
                        gpu_thr += std::string(THROTTLE_REASONS[i]) + ":" + std::to_string(gpu_tc.events[i]);
                    }
                }
            }
            // Build CPU MSR perf limit summary (currently active reasons)
            std::string cpu_thr;
            static int prochot_warn_cycle = 0;
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
                // PROCHOT active — check for root cause and warn if charger weak
                if (current & 1) {
                    ChargerInfo chg = check_charger();
                    if (!chg.note.empty() && iterations - prochot_warn_cycle > 120) {
                        syslog(LOG_WARNING, "PROCHOT asserted — %s", chg.note.c_str());
                        prochot_warn_cycle = iterations;
                    }
                }
            }
            std::string throttle_summary;
            if (!gpu_thr.empty()) throttle_summary += "  gpu-throttle: " + gpu_thr;
            if (!cpu_thr.empty()) throttle_summary += "  cpu-throttle: " + cpu_thr;
            char temp_buf[32] = "";
            if (thermal.temp_c >= 0)
                snprintf(temp_buf, sizeof(temp_buf), "  temp=%.0fC", thermal.temp_c);
            // C0 residency percentage (only when path is available)
            char c0_buf[32] = "";
            if (!s.gpu.c0_residency_path.empty())
                snprintf(c0_buf, sizeof(c0_buf), "  c0=%d%%", (int)(s.gpu.c0_pct * 100.0));
            std::string epp_str = p_val;
            if (e_val != p_val) epp_str += "(" + e_val + ")";
            syslog(LOG_INFO, "[%s] pkg=%.1fW core=%.1fW gpu=%.1fW(gpu_sm=%.1fW) "
                   "pl1=%.1fW core_lmt=%.1fW max_perf=%d%% no_turbo=%d epp=%s%s%s%s",
                   state, pkg_w, core_w, gpu_w, smoothed_gpu_w,
                   effective_pl1_w, core_limit_r, max_perf, no_turbo, epp_str.c_str(),
                   temp_buf, throttle_summary.c_str(), c0_buf);
            last_aggression = aggression;
        }
    }

    // ── Report final throttle statistics ──
    if (have_gpu && gpu_tc.total_events > 0) {
        std::string summary;
        for (int i = 0; THROTTLE_REASONS[i]; ++i) {
            if (gpu_tc.events[i] > 0) {
                if (!summary.empty()) summary += ", ";
                summary += std::string(THROTTLE_REASONS[i]) + "=" + std::to_string(gpu_tc.events[i]);
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
        write_int(s.thermal.powerclamp_dev + "/cur_state", s.thermal.saved_powerclamp_state);
    restore_online_state(s.cpu);
    restore_cpu_state(s);

    syslog(LOG_INFO, "stopped");
    closelog();
    return 0;
}
