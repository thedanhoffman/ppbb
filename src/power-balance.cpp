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
static constexpr double   DEFAULT_PL1_W        = 28.0;
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
static std::string GPU_GT0;        // base path for gt0
static std::string GPU_THROTTLE;   // throttle/reasons file
static std::string GPU_IDLE;       // gtidle/idle_status file

static void discover_gpu() {
    DIR* drm = opendir("/sys/class/drm");
    if (!drm) return;
    struct dirent* de;
    while ((de = readdir(drm)) != nullptr) {
        std::string card = de->d_name;
        if (card.find("card") != 0) continue;
        std::string base = "/sys/class/drm/" + card + "/device/tile0/gt0";
        if (read_file(base + "/freq0/cur_freq") == "") continue;
        GPU_GT0 = base;
        GPU_THROTTLE = base + "/freq0/throttle/reasons";
        GPU_IDLE = base + "/gtidle/idle_status";
        break;
    }
    closedir(drm);
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
static int    g_saved_max_perf   = 100;
static int    g_saved_min_perf   = 8;
static int    g_saved_no_turbo   = 0;
static std::string g_saved_epp;

static void save_cpu_state() {
    read_attr(PSTATE_DIR, "max_perf_pct", g_saved_max_perf);
    read_attr(PSTATE_DIR, "min_perf_pct", g_saved_min_perf);
    read_attr(PSTATE_DIR, "no_turbo", g_saved_no_turbo);
    if (!g_pcore_epp_paths.empty())
        g_saved_epp = read_file(g_pcore_epp_paths[0]);
    syslog(LOG_INFO, "saved state: max_perf=%d  min_perf=%d  no_turbo=%d  epp=%s",
           g_saved_max_perf, g_saved_min_perf, g_saved_no_turbo, g_saved_epp.c_str());
}

static void restore_cpu_state() {
    write_int(PSTATE_MAX, g_saved_max_perf);
    write_int(PSTATE_MIN, g_saved_min_perf);
    write_int(PSTATE_NOTURBO, g_saved_no_turbo);
    if (!g_saved_epp.empty())
        set_epp_legacy_all(g_saved_epp);
    syslog(LOG_INFO, "restored CPU state");
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

static int gpu_throttling(GpuThrottleCounters* counters) {
    std::string throttle_dir = GPU_GT0 + "/freq0/throttle";
    int any = 0;
    for (int i = 0; THROTTLE_FILES[i]; ++i) {
        int v = 0;
        read_attr(throttle_dir, THROTTLE_FILES[i], v);
        bool active = (v != 0);
        if (active) any = 1;
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

// ── Temperature ──
static std::string CORETEMP_DIR;  // coretemp hwmon directory

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
    long long uw = 0, max_uw = 0;
    // The effective limit is the min of the set value and the hardware max cap
    read_attr(dir, "constraint_0_power_limit_uw", uw);
    read_attr(dir, "constraint_0_max_power_uw", max_uw);
    if (max_uw > 0) uw = std::min(uw, max_uw);
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

    if (!GPU_GT0.empty())
        syslog(LOG_INFO, "GPU gt0=%s", GPU_GT0.c_str());
    else
        syslog(LOG_WARNING, "GPU not found — running in CPU-only mode");
    if (!CORETEMP_DIR.empty())
        syslog(LOG_INFO, "coretemp=%s", CORETEMP_DIR.c_str());
    else
        syslog(LOG_WARNING, "coretemp not found — no temperature-aware capping");

    discover_clusters();

    // Override PL1 from sysfs if not explicitly set
    double sysfs_pl1 = read_pl1_w(PKG_DIR);
    if (sysfs_pl1 > 0) {
        if (pl1_w == DEFAULT_PL1_W || !std::isnan(pl1_w))
            pl1_w = sysfs_pl1;
    }
    // Also read MMIO PL1 for reference
    double mmio_pl1 = PKG_MMIO_DIR.empty() ? pl1_w : read_pl1_w(PKG_MMIO_DIR);

    // Enable RAPL domains
    write_file(PKG_DIR + "/enabled", "1");
    for (auto& d : {CORE_DIR, UNCORE_DIR})
        if (!d.empty()) write_file(d + "/enabled", "1");

    // Save initial CPU state
    save_cpu_state();

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

    // Track GPU throttle events
    GpuThrottleCounters gpu_tc;
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
        if (aggression >= 2)       { p_val = "power";             e_val = "balance_power"; }
        else if (aggression >= 1)  { p_val = "balance_power";     e_val = "balance_performance"; }
        if (thermal.epp_tier >= 2) { p_val = "power";             if (thermal.temp_c < 90.0) e_val = "balance_power"; else e_val = "power"; }
        else if (thermal.epp_tier >= 1) { p_val = "balance_power"; e_val = "balance_performance"; }
        set_epp(p_val, e_val);

        // ── Log state changes ──
        if (aggression != last_aggression || iterations % 20 == 0) {
            const char* state = "idle";
            if (aggression == 1) state = "active";
            if (aggression == 2) state = "THROTTLE";
            // Build throttle event summary
            std::string throttle_summary;
            if (have_gpu && gpu_tc.total_events > 0) {
                for (int i = 0; THROTTLE_REASONS[i]; ++i) {
                    if (gpu_tc.events[i] > 0) {
                        if (!throttle_summary.empty()) throttle_summary += " ";
                        throttle_summary += std::string(THROTTLE_REASONS[i]) + ":" + std::to_string(gpu_tc.events[i]);
                    }
                }
            }
            char temp_buf[32] = "";
            if (thermal.temp_c >= 0)
                snprintf(temp_buf, sizeof(temp_buf), "  temp=%.0fC", thermal.temp_c);
            std::string epp_str = p_val;
            if (e_val != p_val) epp_str += "(" + e_val + ")";
            syslog(LOG_INFO, "[%s] pkg=%.1fW core=%.1fW gpu=%.1fW(gpu_sm=%.1fW) "
                   "pl1=%.1fW core_lmt=%.1fW max_perf=%d%% no_turbo=%d epp=%s%s%s%s",
                   state, pkg_w, core_w, gpu_w, smoothed_gpu_w,
                   pl1_w, core_limit_r, max_perf, no_turbo, epp_str.c_str(),
                   temp_buf,
                   throttle_summary.empty() ? "" : "  throttle: ",
                   throttle_summary.c_str());
            last_aggression = aggression;
        }
    }

    // ── Report final GPU throttle statistics ──
    if (have_gpu && gpu_tc.total_events > 0) {
        std::string summary;
        for (int i = 0; THROTTLE_REASONS[i]; ++i) {
            if (gpu_tc.events[i] > 0) {
                if (!summary.empty()) summary += ", ";
                summary += std::string(THROTTLE_REASONS[i]) + "=" + std::to_string(gpu_tc.events[i]);
            }
        }
        syslog(LOG_INFO, "GPU throttle events: %d total (%s)  cycles_throttled=%d",
               gpu_tc.total_events, summary.c_str(), gpu_tc.cycles_throttled);
    } else if (have_gpu) {
        syslog(LOG_INFO, "GPU throttle events: none — no throttling occurred during this session");
    }

    // ── Restore everything on exit ──
    if (have_core)   write_int(CORE_DIR + "/constraint_0_power_limit_uw", 0);
    if (have_uncore) write_int(UNCORE_DIR + "/constraint_0_power_limit_uw", 0);
    restore_cpu_state();

    syslog(LOG_INFO, "stopped");
    closelog();
    return 0;
}
