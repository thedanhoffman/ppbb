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
static constexpr double   GPU_ACTIVE_W         = 3.0;   // above this, GPU considered active
static constexpr double   GPU_HEAVY_W          = 15.0;  // above this, disable CPU turbo
static constexpr double   SMOOTH_ALPHA         = 0.3;
static constexpr double   HEADROOM_MARGIN_W    = 2.0;   // keep this much PL1 headroom always
static constexpr double   CRITICAL_CPU_MAX_W   = 8.0;   // max core budget when GPU is throttling
static constexpr double   CRITICAL_MAX_PERF    = 20.0;  // max_perf_pct when GPU throttling
static constexpr double   ACTIVE_MAX_PERF      = 50.0;  // max_perf_pct when GPU active
static constexpr long     PP0_TIME_WINDOW_US   = 500;

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

// ── RAPL paths (discovered dynamically) ──
static std::string PKG_DIR;       // intel-rapl package-0
static std::string PKG_MMIO_DIR;  // intel-rapl-mmio package-0
static std::string CORE_DIR;
static std::string UNCORE_DIR;

static bool discover_rapl() {
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
                    PKG_MMIO_DIR = path;
                else
                    PKG_DIR = path;
                // Scan subdomains (only for intel-rapl, not mmio)
                DIR* sdir = opendir(path.c_str());
                if (sdir) {
                    struct dirent* se;
                    while ((se = readdir(sdir)) != nullptr) {
                        std::string sn = se->d_name;
                        if (sn[0] == '.') continue;
                        std::string sp = path + "/" + sn;
                        std::string sr = read_file(sp + "/name");
                        if (sr == "core") CORE_DIR = sp;
                        else if (sr == "uncore") UNCORE_DIR = sp;
                    }
                    closedir(sdir);
                }
            }
        }
        closedir(dir);
    }
    return !PKG_DIR.empty();
}

// ── GPU path discovery ──
static std::string GPU_GT0;        // base path for gt0 (render)
static std::string GPU_GT1;        // base path for gt1 (media)
static std::string GPU_THROTTLE;   // throttle/reasons file
static std::string GPU_IDLE;       // gtidle/idle_status file
static std::string g_saved_gt0_profile;
static std::string g_saved_gt1_profile;
static int g_saved_gt0_max_freq = -1;
static int g_saved_gt1_max_freq = -1;

static void discover_gpu() {
    DIR* drm = opendir("/sys/class/drm");
    if (!drm) return;
    struct dirent* de;
    while ((de = readdir(drm)) != nullptr) {
        std::string card = de->d_name;
        if (card.find("card") != 0) continue;
        std::string base0 = "/sys/class/drm/" + card + "/device/tile0/gt0";
        if (read_file(base0 + "/freq0/cur_freq") == "") continue;
        GPU_GT0 = base0;
        GPU_THROTTLE = base0 + "/freq0/throttle/reasons";
        GPU_IDLE = base0 + "/gtidle/idle_status";
        std::string base1 = "/sys/class/drm/" + card + "/device/tile0/gt1";
        if (read_file(base1 + "/freq0/cur_freq") != "") GPU_GT1 = base1;
        break;
    }
    closedir(drm);
}

static void set_gpu_profile(const std::string& profile) {
    for (auto& gpu : {GPU_GT0, GPU_GT1}) {
        if (gpu.empty()) continue;
        std::string path = gpu + "/freq0/power_profile";
        std::string cur = read_file(path);
        if (cur.empty()) continue;
        // The file shows "current    available" — check if not already our target
        if (cur.find(profile) == std::string::npos || cur.find("[" + profile + "]") == std::string::npos)
            write_str(path, profile);
    }
}

// ── CPU control paths ──
static const std::string PSTATE_DIR    = "/sys/devices/system/cpu/intel_pstate";
static const std::string PSTATE_MAX    = PSTATE_DIR + "/max_perf_pct";
static const std::string PSTATE_MIN    = PSTATE_DIR + "/min_perf_pct";
static const std::string PSTATE_NOTURBO = PSTATE_DIR + "/no_turbo";

// ── Per-cluster EPP paths ──
// On hybrid Intel (Meteor Lake+), P-cores and E-cores have different
// max frequencies.  We group them so P-cores get aggressively throttled
// first while E-cores stay more responsive for background tasks.
static std::vector<std::string> g_pcore_epp_paths;
static std::vector<std::string> g_ecore_epp_paths;

static void discover_clusters() {
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
        for (auto& c : cpus) g_pcore_epp_paths.push_back(c.first);
        return;
    }

    int threshold = (int)(global_max_freq * 0.9);
    for (auto& c : cpus) {
        if (c.second >= threshold)
            g_pcore_epp_paths.push_back(c.first);
        else
            g_ecore_epp_paths.push_back(c.first);
    }
    syslog(LOG_INFO, "clusters: %zu P-cores, %zu E-cores",
           g_pcore_epp_paths.size(), g_ecore_epp_paths.size());
}

// ── CPU hotplug (core offlining) ──
// Groups CPUs by physical core (HT siblings share a group).  We offline
// entire groups to eliminate leakage and switching power from unused cores.
// E-cores get offlined first, then P-core HT pairs.

struct CoreGroup {
    int         id;              // group index
    bool        is_pcore;
    bool        has_ht;          // true if this group has HT siblings
    std::vector<int> cpus;       // logical CPU numbers in this group
    int         priority;        // higher = offline first
    bool        saved_online;    // initial state (for restore)
};

static std::vector<CoreGroup> g_core_groups;

static void discover_topology() {
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
        g.id = (int)g_core_groups.size();
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
        g_core_groups.push_back(g);
    }

    // Sort by priority descending (highest = offline first)
    std::sort(g_core_groups.begin(), g_core_groups.end(),
        [](const CoreGroup& a, const CoreGroup& b) { return a.priority > b.priority; });

    // Save initial online state
    for (auto& g : g_core_groups) {
        std::string online_str = read_file("/sys/devices/system/cpu/cpu" + std::to_string(g.cpus[0]) + "/online");
        g.saved_online = (online_str != "0");
    }

    int p = 0, e = 0;
    for (auto& g : g_core_groups) { if (g.is_pcore) p++; else e++; }
    syslog(LOG_INFO, "topology: %d P-core groups, %d E-core groups (%zu logical CPUs)",
           p, e, g_core_groups.size());
}

static int count_online_groups() {
    int n = 0;
    for (auto& g : g_core_groups) {
        std::string s = read_file("/sys/devices/system/cpu/cpu" + std::to_string(g.cpus[0]) + "/online");
        if (s != "0") n++;
    }
    return n;
}

// Target logical CPUs to keep online based on aggression + temperature.
// Returns the number of CoreGroups to keep online.
static int compute_keep_groups(int aggression, double temp_c, double gpu_w, bool weak_charger = false) {
    // If charger is weak, cap aggressively
    if (weak_charger) return 2;
    // idle/cool:  all groups online
    if (aggression <= 0 && temp_c < 70.0) return (int)g_core_groups.size();

    // critical:   temp >= 90°C or GPU throttling hard → 1 P-core group (2 threads)
    if (aggression >= 2 || temp_c >= 90.0) return 1;

    // throttle:   temp 85-90°C or throttling → 3 P + 1 E = 4 groups (7 threads)
    if (temp_c >= 85.0) return 4;

    // heavy:      temp 80-85°C or GPU > 15W → 6 P + 2 E = 8 groups (14 threads)
    if (aggression >= 1 || temp_c >= 80.0 || gpu_w > GPU_HEAVY_W) return 8;

    // moderate:   temp 70-80°C or GPU active → 6 P + 6 E = 12 groups (18 threads)
    return 12;
}

static int g_hotplug_settle = 0;
static int g_keep_groups_target = -1;

static void apply_hotplug(int aggression, double temp_c, double gpu_w, bool weak_charger) {
    if (g_core_groups.empty()) return;

    if (g_hotplug_settle > 0) { g_hotplug_settle--; return; }

    int target = compute_keep_groups(aggression, temp_c, gpu_w, weak_charger);
    target = std::max(target, 1);
    if (target == g_keep_groups_target) return;

    int total = (int)g_core_groups.size();
    int current = count_online_groups();

    // Build a "should_online" mask for each group:
    // 1. Start by marking the last `target` groups (lowest priority / most worth keeping)
    // 2. CPU0 group always online — mark it before any P-core minimum check
    // 3. Ensure at least 2 P-core groups remain (CPU0 counts toward this)
    std::vector<bool> should_online(total, false);
    for (int i = total - target; i < total; ++i) should_online[i] = true;

    for (int i = 0; i < total; ++i)
        for (int c : g_core_groups[i].cpus)
            if (c == 0) { should_online[i] = true; break; }

    // Ensure at least 2 P-core groups including CPU0
    for (int i = 0; i < total; ++i) {
        if (!should_online[i] && g_core_groups[i].is_pcore) {
            int p_would_stay = 0;
            for (int j = 0; j < total; ++j)
                if (should_online[j] && g_core_groups[j].is_pcore) p_would_stay++;
            if (p_would_stay < 2) should_online[i] = true;
        }
    }

    // Apply: offline groups marked offline, online groups marked online
    bool changed = false;
    for (int i = 0; i < total; ++i) {
        auto& g = g_core_groups[i];
        std::string s = read_file("/sys/devices/system/cpu/cpu" + std::to_string(g.cpus[0]) + "/online");
        bool is_online = (s != "0");
        if (is_online && !should_online[i]) {
            for (int c : g.cpus)
                write_int("/sys/devices/system/cpu/cpu" + std::to_string(c) + "/online", 0);
            changed = true;
        } else if (!is_online && should_online[i]) {
            for (int c : g.cpus)
                write_int("/sys/devices/system/cpu/cpu" + std::to_string(c) + "/online", 1);
            changed = true;
        }
    }

    if (changed) {
        int new_count = count_online_groups();
        syslog(LOG_INFO, "hotplug: %d groups online (target=%d, keep=%d)",
               new_count, target, current);
        g_keep_groups_target = target;
        g_hotplug_settle = 20;
    }
}

static void restore_online_state() {
    for (auto& g : g_core_groups) {
        for (int c : g.cpus) {
            if (g.saved_online)
                write_int("/sys/devices/system/cpu/cpu" + std::to_string(c) + "/online", 1);
        }
    }
    syslog(LOG_INFO, "restored CPU online state");
}

// ── EPP write (per cluster, only when changed) ──
static std::string g_current_epp_p;
static std::string g_current_epp_e;
static void set_epp(const std::string& p_val, const std::string& e_val) {
    if (p_val == g_current_epp_p && e_val == g_current_epp_e) return;
    bool any = false;
    for (auto& p : g_pcore_epp_paths) { write_str(p, p_val); any = true; }
    for (auto& p : g_ecore_epp_paths) { write_str(p, e_val); any = true; }
    g_current_epp_p = p_val;
    g_current_epp_e = e_val;
    if (any) {
        std::string msg = "EPP -> P:" + p_val;
        if (e_val != p_val) msg += "  E:" + e_val;
        syslog(LOG_INFO, "%s", msg.c_str());
    }
}

// Legacy single-value EPP for save/restore (writes same value to all CPUs)
static void set_epp_legacy_all(const std::string& val) {
    for (auto& p : g_pcore_epp_paths) write_str(p, val);
    for (auto& p : g_ecore_epp_paths) write_str(p, val);
}

// ── Saved state (restored on exit) ──
static bool msr_available();
static unsigned long long read_msr(int cpu, unsigned int msr_addr);
static bool write_msr(int cpu, unsigned int msr_addr, unsigned long long val);
static int    g_saved_max_perf   = 100;
static int    g_saved_min_perf   = 8;
static int    g_saved_no_turbo   = 0;
static std::string g_saved_epp;
static unsigned long long g_saved_msr_1fc = 0;

static void save_cpu_state() {
    read_attr(PSTATE_DIR, "max_perf_pct", g_saved_max_perf);
    read_attr(PSTATE_DIR, "min_perf_pct", g_saved_min_perf);
    read_attr(PSTATE_DIR, "no_turbo", g_saved_no_turbo);
    if (!g_pcore_epp_paths.empty())
        g_saved_epp = read_file(g_pcore_epp_paths[0]);
    g_saved_gt0_profile = GPU_GT0.empty() ? "" : read_file(GPU_GT0 + "/freq0/power_profile");
    g_saved_gt1_profile = GPU_GT1.empty() ? "" : read_file(GPU_GT1 + "/freq0/power_profile");
    if (!GPU_GT0.empty()) read_attr(GPU_GT0 + "/freq0", "max_freq", g_saved_gt0_max_freq);
    if (!GPU_GT1.empty()) read_attr(GPU_GT1 + "/freq0", "max_freq", g_saved_gt1_max_freq);
    g_saved_msr_1fc = msr_available() ? read_msr(0, 0x1FC) : 0;
    syslog(LOG_INFO, "saved state: max_perf=%d  min_perf=%d  no_turbo=%d  epp=%s",
           g_saved_max_perf, g_saved_min_perf, g_saved_no_turbo, g_saved_epp.c_str());
}

static void restore_cpu_state() {
    write_int(PSTATE_MAX, g_saved_max_perf);
    write_int(PSTATE_MIN, g_saved_min_perf);
    write_int(PSTATE_NOTURBO, g_saved_no_turbo);
    if (!g_saved_epp.empty())
        set_epp_legacy_all(g_saved_epp);
    if (!g_saved_gt0_profile.empty()) write_str(GPU_GT0 + "/freq0/power_profile", g_saved_gt0_profile);
    if (!g_saved_gt1_profile.empty()) write_str(GPU_GT1 + "/freq0/power_profile", g_saved_gt1_profile);
    if (g_saved_gt0_max_freq > 0) write_int(GPU_GT0 + "/freq0/max_freq", g_saved_gt0_max_freq);
    if (g_saved_gt1_max_freq > 0) write_int(GPU_GT1 + "/freq0/max_freq", g_saved_gt1_max_freq);
    if (g_saved_msr_1fc) write_msr(0, 0x1FC, g_saved_msr_1fc);
    syslog(LOG_INFO, "restored CPU and GPU state");
}

// ── GPU throttle tracking ──
static const char* THROTTLE_REASONS[] = {
    "pl1", "pl2", "pl4", "prochot", "thermal", "ratl", "vr_tdc", "vr_thermalert", nullptr
};
static const char* THROTTLE_FILES[] = {
    "reason_pl1", "reason_pl2", "reason_pl4",
    "reason_prochot", "reason_thermal", "reason_ratl",
    "reason_vr_tdc", "reason_vr_thermalert", nullptr
};

struct GpuThrottleCounters {
    int events[8] = {0};
    int total_events = 0;
    int cycles_throttled = 0;
    bool prev_state[8] = {false};
};

// Returns 1 if any serious throttle reason active.  GPU-internal power limit
// events (PL1, PL2, PL4) are excluded — they are normal GuC-managed peak
// current management at the frequency ceiling.  PROCHOT is also excluded since
// we disable the external PROCHOT# response on the CPU (MSR 0x1FC bit 0).
// Only genuine thermal/current alerts (thermal, vr_tdc, vr_thermalert, ratl)
// trigger aggression escalation.
static int gpu_throttling(GpuThrottleCounters* counters) {
    std::string throttle_dir = GPU_GT0 + "/freq0/throttle";
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

static bool gpu_active() {
    std::string s = read_file(GPU_IDLE);
    return s.find("c6") == std::string::npos;
}

// ── CPU Perf Limit Reasons (MSR 0x6B0, 0x690) ──
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

struct PerfLimitCounters {
    int events[16] = {0};
    unsigned int prev_current = 0;
};

static void track_perf_limits(PerfLimitCounters* pl, int cpu) {
    unsigned long long msr = read_msr(cpu, 0x6B0);
    unsigned int current = msr & 0xFFFF;
    if (!pl) return;
    unsigned int newly = current & ~pl->prev_current;
    for (int i = 0; PERF_LIMIT_REASONS[i].name; ++i) {
        unsigned int m = 1u << PERF_LIMIT_REASONS[i].bit;
        if (newly & m) pl->events[i]++;
    }
    pl->prev_current = current;
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

// ── Temperature ──
static std::string CORETEMP_DIR;  // coretemp hwmon directory
static std::string POWERCLAMP_DEV;  // intel_powerclamp cooling device path
static int g_saved_powerclamp_state = 0;

static void discover_coretemp() {
    DIR* hwmon = opendir("/sys/class/hwmon");
    if (!hwmon) return;
    struct dirent* entry;
    while ((entry = readdir(hwmon)) != nullptr) {
        std::string name = entry->d_name;
        if (name[0] == '.') continue;
        std::string path = std::string("/sys/class/hwmon/") + name;
        if (read_file(path + "/name") == "coretemp") {
            CORETEMP_DIR = path;
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
            POWERCLAMP_DEV = path;
            read_attr(POWERCLAMP_DEV, "cur_state", g_saved_powerclamp_state);
            syslog(LOG_INFO, "intel_powerclamp=%s (saved state=%d)", path.c_str(), g_saved_powerclamp_state);
            break;
        }
    }
    closedir(therm);
}

static double read_max_core_temp() {
    // Returns max temp across all cores in Celsius, or -1 on failure
    DIR* dir = CORETEMP_DIR.empty() ? nullptr : opendir(CORETEMP_DIR.c_str());
    if (!dir) return -1;
    double max_temp = -1;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string fn = entry->d_name;
        if (fn.find("temp") == 0 && fn.size() > 5 && fn.find("_input") != std::string::npos) {
            std::string val = read_file(CORETEMP_DIR + "/" + fn);
            if (!val.empty()) {
                double t = std::stod(val) / 1000.0;
                if (t > max_temp) max_temp = t;
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

static double read_pl1_w(const std::string& dir) {
    long long uw = 0;
    read_attr(dir, "constraint_0_power_limit_uw", uw);
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

    // Discover all hardware paths
    if (!discover_rapl() || PKG_DIR.empty()) {
        syslog(LOG_ERR, "RAPL package domain not found");
        return 1;
    }
    discover_gpu();

    syslog(LOG_INFO, "RAPL pkg=%s core=%s uncore=%s mmio=%s",
           PKG_DIR.c_str(),
           CORE_DIR.empty() ? "?" : CORE_DIR.c_str(),
           UNCORE_DIR.empty() ? "?" : UNCORE_DIR.c_str(),
           PKG_MMIO_DIR.empty() ? "?" : PKG_MMIO_DIR.c_str());
    discover_coretemp();

    if (!GPU_GT0.empty()) {
        syslog(LOG_INFO, "GPU gt0=%s", GPU_GT0.c_str());
        if (!GPU_GT1.empty())
            syslog(LOG_INFO, "GPU gt1=%s", GPU_GT1.c_str());
        set_gpu_profile("base");
    } else
        syslog(LOG_WARNING, "GPU not found — running in CPU-only mode");
    if (!CORETEMP_DIR.empty())
        syslog(LOG_INFO, "coretemp=%s", CORETEMP_DIR.c_str());
    else
        syslog(LOG_WARNING, "coretemp not found — no temperature-aware capping");

    discover_clusters();
    discover_topology();

    // Read system PL1 for logging reference only (we raise it below)
    double sysfs_pl1 = read_pl1_w(PKG_DIR);
    (void)sysfs_pl1;
    double mmio_pl1 = PKG_MMIO_DIR.empty() ? pl1_w : read_pl1_w(PKG_MMIO_DIR);

    // Raise hardware PL1 to desired value on both MSR and MMIO domains.
    // The hardware max_power_uw is conservative — actual VR can handle more.
    // Higher PL1 gives the GPU VR more peak current headroom, avoiding PL4 events.
    write_int(PKG_DIR + "/constraint_0_power_limit_uw", (long long)(pl1_w * 1e6));
    if (!PKG_MMIO_DIR.empty())
        write_int(PKG_MMIO_DIR + "/constraint_0_power_limit_uw", (long long)(pl1_w * 1e6));

    // Enable RAPL domains
    write_file(PKG_DIR + "/enabled", "1");
    for (auto& d : {CORE_DIR, UNCORE_DIR})
        if (!d.empty()) write_file(d + "/enabled", "1");

    // Save initial CPU state
    save_cpu_state();

    // The EC on this laptop asserts PROCHOT# even at low temperatures (44°C / 4W),
    // which conflicts with the daemon's own thermal management.  Disable the
    // external PROCHOT# response (MSR 0x1FC bit 0), since we handle throttling
    // ourselves via aggression levels, temperature overlay, and core offlining.
    // This is a standard Intel MSR — restored on exit.
    if (msr_available()) {
        unsigned long long msr_1fc = read_msr(0, 0x1FC);
        if (msr_1fc & 1) {
            write_msr(0, 0x1FC, msr_1fc & ~1ULL);
            syslog(LOG_INFO, "external PROCHOT# response disabled (MSR 0x1FC bit 0)");
        }
    }

    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGHUP,  handle_signal);

    bool have_core   = !CORE_DIR.empty();
    bool have_uncore = !UNCORE_DIR.empty();
    bool have_gpu    = !GPU_GT0.empty();
    int settle_cycles = 3;  // skip throttle detection until RAPL counters settle

    syslog(LOG_INFO, "starting — PL1: %.1fW (mmio: %.1fW)  interval: %dms  GPU: %s",
           pl1_w, mmio_pl1, INTERVAL_MS, have_gpu ? "yes" : "no");

    Sample prev_pkg  = read_energy(PKG_DIR);
    Sample prev_core, prev_unc;
    if (have_core)   prev_core = read_energy(CORE_DIR);
    if (have_uncore) prev_unc  = read_energy(UNCORE_DIR);

    double smoothed_gpu_w = 0;
    bool first = true;
    int iterations = 0;

    // Track throttle events (GPU and CPU MSR)
    GpuThrottleCounters gpu_tc;
    bool msr_ok = msr_available();
    PerfLimitCounters pl_tc;
    int last_aggression = -1; // 0=idle, 1=active, 2=throttling

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(INTERVAL_MS));
        if (!g_running) break;
        iterations++;

        Sample cur_pkg  = read_energy(PKG_DIR);
        Sample cur_core, cur_unc;
        if (have_core)   cur_core = read_energy(CORE_DIR);
        if (have_uncore) cur_unc  = read_energy(UNCORE_DIR);

        double pkg_w  = compute_power_w(prev_pkg, cur_pkg);
        double core_w = have_core ? compute_power_w(prev_core, cur_core) : 0;
        double gpu_w  = have_uncore ? compute_power_w(prev_unc, cur_unc) : 0;

        prev_pkg  = cur_pkg;
        if (have_core)   prev_core = cur_core;
        if (have_uncore) prev_unc  = cur_unc;

        if (pkg_w < 0) continue;
        if (first) { smoothed_gpu_w = gpu_w; first = false; }

        smoothed_gpu_w = SMOOTH_ALPHA * gpu_w + (1.0 - SMOOTH_ALPHA) * smoothed_gpu_w;

        // ── Assess GPU state (skip throttle check during settle) ──
        bool throttling = false;
        if (settle_cycles <= 0) throttling = have_gpu ? (gpu_throttling(&gpu_tc) != 0) : false;
        bool active = have_gpu ? gpu_active() : false;
        if (settle_cycles > 0) settle_cycles--;

        // ── Track CPU MSR perf limit reasons ──
        if (msr_ok) track_perf_limits(&pl_tc, 0);

        // ── Determine aggression level ──
        // 0 = GPU idle: relax CPU controls
        // 1 = GPU active: moderate CPU back-off
        // 2 = GPU throttling: CRITICAL — hammer the CPU
        int aggression = 0;
        if (throttling || (active && smoothed_gpu_w > GPU_ACTIVE_W)) aggression = 1;
        if (throttling) aggression = 2;

        // ── Temperature reading ──
        double max_temp = read_max_core_temp();
        ThermalConstraints thermal = compute_thermal(max_temp);

        // ── Weak charger detection ──
        static bool g_was_weak_charger = false;
        static double g_charger_max_watts = 0;
        static std::string g_charger_note;
        bool weak_charger = g_was_weak_charger;
        if (iterations % 20 == 10) {  // offset from logging cycle
            ChargerInfo chg = check_charger();
            weak_charger = chg.weak;
            if (chg.max_watts > 0) g_charger_max_watts = chg.max_watts;
            if (chg.weak) g_charger_note = chg.note;
            if (weak_charger != g_was_weak_charger) {
                if (weak_charger)
                    syslog(LOG_WARNING, "weak charger: %s — enabling power saving", g_charger_note.c_str());
                else
                    syslog(LOG_INFO, "adequate charger detected — disabling power saving");
                g_was_weak_charger = weak_charger;
            }
        }

        // ── Calculate CPU core budget ──
        // Principle: GPU gets unlimited budget (PP1 = 0 = disabled).
        // CPU (PP0) gets: PL1 - gpu_current_draw - headroom_margin
        // If GPU is throttling, cap core budget very low.
        // Temperature adds extra headroom to let hot cores cool down.
        double headroom = HEADROOM_MARGIN_W + thermal.headroom_extra;

        double core_budget_w;
        if (aggression >= 2) {
            core_budget_w = std::min(CRITICAL_CPU_MAX_W, pl1_w - smoothed_gpu_w - headroom);
        } else {
            core_budget_w = pl1_w - smoothed_gpu_w - headroom;
        }
        core_budget_w = std::max(core_budget_w, 0.0);

        double core_limit_r = round_to_125mw(core_budget_w);

        // ── Apply RAPL limits ──
        if (have_core) {
            write_int(CORE_DIR + "/constraint_0_power_limit_uw", (long long)(core_limit_r * 1e6));
            write_int(CORE_DIR + "/constraint_0_time_window_us", PP0_TIME_WINDOW_US);
        }
        if (have_uncore) {
            // Uncore = unlimited (0 = disabled)
            write_int(UNCORE_DIR + "/constraint_0_power_limit_uw", 0);
            write_int(UNCORE_DIR + "/constraint_0_time_window_us", PP0_TIME_WINDOW_US);
        }

        // ── MMIO package PL1 ──
        // Raise MMIO PL1 to match MSR PL1 so it doesn't become a bottleneck
        if (!PKG_MMIO_DIR.empty()) {
            long long mmio_now = 0;
            read_attr(PKG_MMIO_DIR, "constraint_0_power_limit_uw", mmio_now);
            long long target = (long long)(pl1_w * 1e6);
            if (mmio_now < target)
                write_int(PKG_MMIO_DIR + "/constraint_0_power_limit_uw", target);
        }

        // ── PROCHOT# response ──
        // The EC/firmware keeps re-enabling PROCHOT# response (MSR 0x1FC bit 0).
        // Clear it every cycle since we manage throttling ourselves.
        if (msr_ok) {
            unsigned long long msr_1fc = read_msr(0, 0x1FC);
            if (msr_1fc & 1)
                write_msr(0, 0x1FC, msr_1fc & ~1ULL);
        }

        // ── CPU frequency capping ──
        // Combine aggression-based cap with thermal cap (most conservative wins)
        int max_perf = 100;
        if (aggression >= 2) {
            max_perf = (int)CRITICAL_MAX_PERF;
        } else if (aggression >= 1) {
            double ratio = std::min(smoothed_gpu_w / GPU_HEAVY_W, 1.0);
            max_perf = 100 - (int)(ratio * (100.0 - ACTIVE_MAX_PERF));
            max_perf = std::max(max_perf, (int)ACTIVE_MAX_PERF);
        }
        max_perf = std::min(max_perf, thermal.max_perf);
        write_int(PSTATE_MAX, max_perf);

        // ── CPU turbo control ──
        int no_turbo = 0;
        if (aggression >= 2 || smoothed_gpu_w > GPU_HEAVY_W) no_turbo = 1;
        no_turbo = std::max(no_turbo, thermal.no_turbo);
        write_int(PSTATE_NOTURBO, no_turbo);

        // ── EPP — per-cluster: P-cores get throttled first, E-cores stay responsive
        // Temperature overrides aggression when hotter
        // On hybrid: P-cores take the aggressive value, E-cores are 1 tier less aggressive
        // (except at critical temp >= 90°C where both get power)
        std::string p_val = "balance_performance";
        std::string e_val = "balance_performance";
        if (weak_charger) {
            p_val = "power"; e_val = "power";
        } else if (aggression >= 2)       { p_val = "power";             e_val = "balance_power"; }
        else if (aggression >= 1)  { p_val = "balance_power";     e_val = "balance_performance"; }
        if (thermal.epp_tier >= 2) { p_val = "power";             if (thermal.temp_c < 90.0) e_val = "balance_power"; else e_val = "power"; }
        else if (thermal.epp_tier >= 1) { p_val = "balance_power"; e_val = "balance_performance"; }
        set_epp(p_val, e_val);

        // ── CPU hotplug (core offlining) ──
        // Drop min_perf to 0 when cores are offlined so remaining idle cores
        // can reach minimum frequency and deepest C-states.
        apply_hotplug(aggression, thermal.temp_c, smoothed_gpu_w, weak_charger);
        int min_perf = (g_keep_groups_target > 0 && g_keep_groups_target < (int)g_core_groups.size()) ? 0 : g_saved_min_perf;
        write_int(PSTATE_MIN, min_perf);

        // ── Weak charger constraints ──
        // Apply aggressive power saving when the charger can't keep up.
        // These are applied AFTER all normal controls so they always win.
        // On transition out of weak charger mode, restore GPU freq, profile, uncore, powerclamp.
        static bool g_gpu_freq_capped = false;
        double effective_pl1_w = pl1_w;
        if (weak_charger && g_charger_max_watts > 0) {
            if (!g_gpu_freq_capped) {
                g_gpu_freq_capped = true;
                // Switch GPU profile to power_saving on entry
                set_gpu_profile("power_saving");
                // Set intel_powerclamp to 20% idle injection
                if (!POWERCLAMP_DEV.empty())
                    write_int(POWERCLAMP_DEV + "/cur_state", 20);
            }

            // Lower package PL1 to ~50% of charger capacity
            double weak_pl1 = g_charger_max_watts * 0.5;
            if (weak_pl1 < pl1_w) {
                effective_pl1_w = weak_pl1;
                write_int(PKG_DIR + "/constraint_0_power_limit_uw", (long long)(weak_pl1 * 1e6));
                if (!PKG_MMIO_DIR.empty())
                    write_int(PKG_MMIO_DIR + "/constraint_0_power_limit_uw", (long long)(weak_pl1 * 1e6));
                double weak_budget = weak_pl1 - smoothed_gpu_w - headroom;
                weak_budget = std::max(weak_budget, 0.0);
                weak_budget = std::min(weak_budget, core_budget_w);
                core_budget_w = weak_budget;
                core_limit_r = round_to_125mw(core_budget_w);
                if (have_core)
                    write_int(CORE_DIR + "/constraint_0_power_limit_uw", (long long)(core_limit_r * 1e6));
            }

            no_turbo = 1;
            write_int(PSTATE_NOTURBO, no_turbo);

            max_perf = std::min(max_perf, 20);
            write_int(PSTATE_MAX, max_perf);

            write_int(PSTATE_MIN, 0);

            if (!GPU_GT0.empty()) write_int(GPU_GT0 + "/freq0/max_freq", 800);
            if (!GPU_GT1.empty()) write_int(GPU_GT1 + "/freq0/max_freq", 100);

            if (have_uncore)
                write_int(UNCORE_DIR + "/constraint_0_power_limit_uw", (long long)(3.0 * 1e6));
        } else if (g_gpu_freq_capped) {
            g_gpu_freq_capped = false;
            // Restore GPU to base profile, freq, uncore, and powerclamp
            set_gpu_profile("base");
            if (g_saved_gt0_max_freq > 0) write_int(GPU_GT0 + "/freq0/max_freq", g_saved_gt0_max_freq);
            if (g_saved_gt1_max_freq > 0) write_int(GPU_GT1 + "/freq0/max_freq", g_saved_gt1_max_freq);
            if (have_uncore)
                write_int(UNCORE_DIR + "/constraint_0_power_limit_uw", 0);
            if (!POWERCLAMP_DEV.empty())
                write_int(POWERCLAMP_DEV + "/cur_state", g_saved_powerclamp_state);
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
            std::string epp_str = p_val;
            if (e_val != p_val) epp_str += "(" + e_val + ")";
            syslog(LOG_INFO, "[%s] pkg=%.1fW core=%.1fW gpu=%.1fW(gpu_sm=%.1fW) "
                   "pl1=%.1fW core_lmt=%.1fW max_perf=%d%% no_turbo=%d epp=%s%s%s",
                   state, pkg_w, core_w, gpu_w, smoothed_gpu_w,
                   effective_pl1_w, core_limit_r, max_perf, no_turbo, epp_str.c_str(),
                   temp_buf, throttle_summary.c_str());
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
    if (have_core)   write_int(CORE_DIR + "/constraint_0_power_limit_uw", 0);
    if (have_uncore) write_int(UNCORE_DIR + "/constraint_0_power_limit_uw", 0);
    if (!POWERCLAMP_DEV.empty()) write_int(POWERCLAMP_DEV + "/cur_state", g_saved_powerclamp_state);
    restore_online_state();
    restore_cpu_state();

    syslog(LOG_INFO, "stopped");
    closelog();
    return 0;
}
