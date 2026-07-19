// power-status.cpp — simple terminal power monitor
// Only uses 3-bit ANSI foreground colors (30-37) + reset (0) for max compatibility
// Compile: g++ -std=c++17 -O2 power-status.cpp -o power-status

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <functional>

// ── Plain ANSI color codes (3-bit only — universally supported) ──
// Reset: \033[0m   Red: \033[31m   Green: \033[32m
// Yellow: \033[33m   Magenta: \033[35m  Cyan: \033[36m  White: \033[37m

static const char* RST = "\033[0m";
static const char* BLK = "\033[30m";
static const char* RED = "\033[31m";
static const char* GRN = "\033[32m";
static const char* YEL = "\033[33m";

static const char* MAG = "\033[35m";
static const char* CYN = "\033[36m";
static const char* WHT = "\033[37m";

static std::string color(const char* fg, const std::string& text) {
    return std::string(fg) + text + RST;
}

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

template<typename T>
static bool read_attr(const std::string& dir, const std::string& name, T& out) {
    std::string val = read_file(dir + "/" + name);
    if (val.empty()) return false;
    std::istringstream iss(val);
    return (bool)(iss >> out);
}

static int read_uA(const std::string& dir, const std::string& name) {
    long long v;
    if (read_attr(dir, name, v)) return (int)(v / 1000);
    return -1;
}

static int read_uW(const std::string& dir, const std::string& name) {
    long long v;
    if (read_attr(dir, name, v)) return (int)(v / 1000);
    return -1;
}

static int read_uV(const std::string& dir, const std::string& name) {
    long long v;
    if (read_attr(dir, name, v)) return (int)(v / 1000);
    return -1;
}

static int read_temp(const std::string& dir) {
    int t;
    if (read_attr(dir, "temp", t)) return t / 10;
    return -1;
}

// ── Helpers ──

static int safe_celsius(int raw) { return raw >= 0 ? raw : -1; }

static int detect_cpu_count() {
    DIR* dir = opendir("/sys/devices/system/cpu");
    if (!dir) return 8;
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "cpu", 3) == 0) {
            char* end = nullptr;
            long n = strtol(entry->d_name + 3, &end, 10);
            if (*end == '\0' && n >= 0) count++;
        }
    }
    closedir(dir);
    return count > 0 ? count : 8;
}

static const char* temp_color(int celsius) {
    if (celsius < 0)      return BLK;
    if (celsius < 60)     return GRN;
    if (celsius < 80)     return YEL;
    if (celsius < 90)     return MAG;
    return RED;
}

struct Battery {
    std::string name, status, health, model, manufacturer, technology;
    int capacity = 0;
    int cycle_count = -1;
    int charge_now_mA = -1;
    int charge_full_mA = -1;
    int charge_full_design_mA = -1;
    int voltage_now_mV = -1;
    int current_now_mA = -1;
    int temp_C = -1;
};

struct Charger {
    std::string name;
    std::string status, online;
    int voltage_mV = -1;
    int current_mA = -1;
    int power_mW = -1;
};

static std::string current_time_str() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf);
}

// ── Hex formatting ──
static std::string hex(unsigned long long val, int width) {
    std::ostringstream os;
    os << "0x" << std::hex << std::setfill('0') << std::setw(width) << val;
    return os.str();
}

// ── Temperature reading ──

static int read_cpu_pkg_temp() {
    DIR* dir = opendir("/sys/class/thermal");
    if (!dir) return -1;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name[0] == '.') continue;
        std::string type = read_file("/sys/class/thermal/" + name + "/type");
        if (type == "x86_pkg_temp") {
            int t;
            if (read_attr("/sys/class/thermal/" + name, "temp", t)) {
                closedir(dir);
                return safe_celsius(t / 1000);
            }
        }
    }
    closedir(dir);
    return -1;
}

static std::string find_coretemp_hwmon() {
    DIR* dir = opendir("/sys/class/hwmon");
    if (!dir) return "";
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name[0] == '.') continue;
        std::string driver = read_file("/sys/class/hwmon/" + name + "/name");
        if (driver == "coretemp") {
            closedir(dir);
            return "/sys/class/hwmon/" + name;
        }
    }
    closedir(dir);
    return "";
}

static std::map<std::string, int> read_cpu_core_temps() {
    std::map<std::string, int> cores;
    std::string hwmon_dir = find_coretemp_hwmon();
    if (hwmon_dir.empty()) return cores;

    DIR* subdir = opendir(hwmon_dir.c_str());
    if (!subdir) return cores;
    struct dirent* fe;
    while ((fe = readdir(subdir)) != nullptr) {
        std::string fname = fe->d_name;
        if (fname.size() < 11) continue;
        if (fname.substr(0, 4) != "temp") continue;
        size_t us = fname.find("_input", 4);
        if (us == std::string::npos || us < 5) continue;

        std::string full = hwmon_dir + "/" + fname;

        std::string val = read_file(full);
        if (val.empty()) continue;

        int raw = 0;
        if (!(std::istringstream(val) >> raw)) continue;
        int celsius = raw / 1000;

        std::string num_part = fname.substr(4, us - 4);
        std::string label_path = hwmon_dir + "/temp" + num_part + "_label";
            std::string label = read_file(label_path);
            if (label.empty()) continue;
            // Skip the package-level temp entry (e.g. "Package id 0")
            if (label.find("Package") != std::string::npos) continue;
            cores[label] = celsius;
    }
    closedir(subdir);
    return cores;
}

static int read_gpu_temp() {
    DIR* drm_dir = opendir("/sys/class/drm");
    if (!drm_dir) return -1;
    struct dirent* entry;
    while ((entry = readdir(drm_dir)) != nullptr) {
        std::string card = entry->d_name;
        if (card.find("card") != 0) continue;
        std::string hwmon_base = "/sys/class/drm/" + card + "/device/hwmon";
        DIR* hdir = opendir(hwmon_base.c_str());
        if (!hdir) continue;
        struct dirent* hentry;
        while ((hentry = readdir(hdir)) != nullptr) {
            std::string hwmon = hentry->d_name;
            if (hwmon[0] == '.') continue;
            std::string hname = read_file(hwmon_base + "/" + hwmon + "/name");
            if (hname.find("i915") != std::string::npos ||
                hname.find("amdgpu") != std::string::npos) {
                int t;
                if (read_attr(hwmon_base + "/" + hwmon, "temp1_input", t)) {
                    closedir(drm_dir);
                    closedir(hdir);
                    return safe_celsius(t / 1000);
                }
            }
        }
        closedir(hdir);
    }
    closedir(drm_dir);
    return -1;
}

static int read_nvme_temp() {
    DIR* dir = opendir("/sys/class/hwmon");
    if (!dir) return -1;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string hwmon = entry->d_name;
        if (hwmon[0] == '.') continue;
        std::string name = read_file("/sys/class/hwmon/" + hwmon + "/name");
        if (name.find("nvme") != std::string::npos) {
            int t;
            if (read_attr("/sys/class/hwmon/" + hwmon, "temp1_input", t)) {
                closedir(dir);
                return safe_celsius(t / 1000);
            }
        }
    }
    closedir(dir);
    return -1;
}

// ── Discover power supplies ──

static std::vector<std::pair<std::string, std::string>> list_power_supplies() {
    std::vector<std::pair<std::string, std::string>> supplies;
    DIR* dir = opendir("/sys/class/power_supply");
    if (!dir) return supplies;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name[0] == '.') continue;
        std::string type = read_file("/sys/class/power_supply/" + name + "/type");
        supplies.push_back({name, type});
    }
    closedir(dir);
    std::sort(supplies.begin(), supplies.end());
    return supplies;
}

// ── Format remaining time ──

static std::string format_remaining(int capacity_pct, int charge_now, int current_mA) {
    if (current_mA <= 0 || charge_now <= 0) return "calculating...";
    double hours = (double)capacity_pct * charge_now / (100.0 * current_mA);
    if (hours < 0.05) return "< 3 min";
    if (hours > 99.9) return "> 99h";
    int h = (int)hours;
    int m = (int)((hours - h) * 60 + 0.5);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%dh %02dm", h, m);
    return std::string(buf);
}

// ── Status color mapping ──

static const char* status_color(const std::string& status) {
    if (status == "Charging" || status == "Full") return GRN;
    if (status == "Discharging") return RED;
    if (status.find("Pending") != std::string::npos) return YEL;
    return WHT;
}

// ── Global for signal handler ──
static volatile sig_atomic_t g_running = 1;

static void handle_signal(int) { g_running = 0; }

// ── GPU throttle event tracking (Xe driver reason_* files) ──

static const char* XE_THROTTLE_REASONS[] = {
    "pl1", "pl2", "pl4", "prochot", "thermal", "ratl", "vr_tdc", "vr_thermalert", nullptr
};
static const char* XE_THROTTLE_FILES[] = {
    "reason_pl1", "reason_pl2", "reason_pl4",
    "reason_prochot", "reason_thermal", "reason_ratl",
    "reason_vr_tdc", "reason_vr_thermalert", nullptr
};

struct GpuThrottleState {
    int events[8] = {0};
    int total_events = 0;
    bool prev_active[8] = {false};
};

static void track_gpu_throttle(GpuThrottleState& state, const std::string& throttle_dir) {
    for (int i = 0; XE_THROTTLE_FILES[i]; ++i) {
        int v = 0;
        read_attr(throttle_dir, XE_THROTTLE_FILES[i], v);
        bool active = (v != 0);
        if (active && !state.prev_active[i]) {
            state.events[i]++;
            state.total_events++;
        }
        state.prev_active[i] = active;
    }
}

// ── MSR reading (Perf Limit Reasons) ──

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

// Clear Performance Limit Reasons (MSR 0x6B0) and Package Therm Log (MSR 0x6B1).
// Writing 0 to both clears current reasons + all sticky/history bits.
// Useful for resetting so that new throttling blips can be observed from zero.
static void clear_perf_limit_history() {
    if (!msr_available()) {
        std::cout << color(YEL, "   [PLR clear skipped — MSR unavailable]") << std::endl;
        return;
    }
    bool ok0 = write_msr(0, 0x6B0, 0ULL);   // IA32_PERF_LIMIT_LOG (current + sticky)
    bool ok1 = write_msr(0, 0x6B1, 0ULL);   // IA32_PACKAGE_THERM_LOG (sticky therm events)
    if (ok0 && ok1) {
        std::cout << color(GRN, "   ✓ Perf-limit history cleared (MSR 0x6B0, 0x6B1)") << std::endl;
    } else {
        std::cout << color(RED, "   ✗ Failed to clear perf-limit history (check permissions)") << std::endl;
    }
}

// Intel Perf Limit Reasons bit definitions for MSR 0x690 / 0x6B0
// Lower 16 bits = current status, upper 16 bits = sticky (LOG)
struct PerfLimitBits {
    const char* name;
    unsigned int bit;
};

static const PerfLimitBits PERF_LIMIT_REASONS[] = {
    {"PROCHOT",         0},
    {"Thermal",         1},
    {"Current (EDP)",   2},
    {"Power (PL1)",     3},
    {"Platform",        4},
    {"Autonomous",      5},
    {"VR Thermal",      6},
    {"HTC",             7},
    {"Core/Cache",      8},
    {"Amps",            9},
    {"PROCHOT Deassert",10},
    {"PL4/Peak",       11},
    {"PkgPwr Latch",   12},
    {"Clipping",       13},
    {"", 0}  // sentinel
};

// Perf limit event tracking (accumulated across refresh cycles)
struct PerfEventStats {
    int count = 0;
    int total_ms = 0;
    bool active = false;
};
static PerfEventStats g_pl_stats[16]; // indexed by bit number (0-15)
static unsigned int g_prev_pkg_current = 0;
static auto g_start_time = std::chrono::steady_clock::now();

// ── Thermal throttle sysfs reader ──

struct ThrottleStats {
    int core_count = -1;
    int core_max_ms = -1;
    int core_total_ms = -1;
    int pkg_count = -1;
    int pkg_max_ms = -1;
    int pkg_total_ms = -1;
};

static ThrottleStats read_throttle_stats(int cpu) {
    ThrottleStats s;
    std::string dir = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/thermal_throttle";
    read_attr(dir, "core_throttle_count", s.core_count);
    read_attr(dir, "core_throttle_max_time_ms", s.core_max_ms);
    read_attr(dir, "core_throttle_total_time_ms", s.core_total_ms);
    read_attr(dir, "package_throttle_count", s.pkg_count);
    read_attr(dir, "package_throttle_max_time_ms", s.pkg_max_ms);
    read_attr(dir, "package_throttle_total_time_ms", s.pkg_total_ms);
    return s;
}

// ── RAPL powercap reader ──

struct RAPLDomain {
    std::string name;
    std::string base_path;
    long long power_uw = -1;
    long long energy_uj = -1;
    long long pl1_uw = -1;
    long long pl1_window_us = -1;
    long long pl1_max_uw = -1;
    long long pl2_uw = -1;
    long long pl2_window_us = -1;
    long long pl4_uw = -1;
};

static void scan_rapl_domains_in(const std::string& dir_path, std::vector<RAPLDomain>& domains) {
    DIR* dir = opendir(dir_path.c_str());
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name[0] == '.') continue;

        // Only recurse into actual RAPL domain directories (have ":" in name)
        // This avoids following symlinks like "device" or "subsystem"
        if (name.find(':') == std::string::npos) continue;

        std::string full = dir_path + "/" + name;

        std::string rname = read_file(full + "/name");
        if (rname.empty()) continue;

        RAPLDomain d;
        d.name = rname;
        d.base_path = full;
        long long tmp;
        if (read_attr(full, "power_uw", tmp)) d.power_uw = tmp;
        if (read_attr(full, "energy_uj", tmp)) d.energy_uj = tmp;

        if (read_attr(full, "constraint_0_power_limit_uw", tmp)) d.pl1_uw = tmp;
        if (read_attr(full, "constraint_0_time_window_us", tmp)) d.pl1_window_us = tmp;
        if (read_attr(full, "constraint_0_max_power_uw", tmp)) d.pl1_max_uw = tmp;
        if (read_attr(full, "constraint_1_power_limit_uw", tmp)) d.pl2_uw = tmp;
        if (read_attr(full, "constraint_1_time_window_us", tmp)) d.pl2_window_us = tmp;
        if (read_attr(full, "constraint_2_power_limit_uw", tmp)) d.pl4_uw = tmp;

        domains.push_back(d);

        // Recurse into subdomains (entries with two ":" separators, e.g., intel-rapl:0:0)
        scan_rapl_domains_in(full, domains);
    }
    closedir(dir);
}

static std::vector<RAPLDomain> read_rapl_domains() {
    std::vector<RAPLDomain> domains;
    scan_rapl_domains_in("/sys/class/powercap/intel-rapl", domains);
    scan_rapl_domains_in("/sys/class/powercap/intel-rapl-mmio", domains);
    return domains;
}

// ── Format power values ──

static std::string fmt_power(long long uw) {
    if (uw < 0) return "?";
    if (uw >= 1000000) return std::to_string(uw / 1000000) + "." + std::to_string((uw / 100000) % 10) + " W";
    if (uw >= 1000) return std::to_string(uw / 1000) + " mW";
    return std::to_string(uw) + " uW";
}

static std::string fmt_power_double(double w) {
    if (w < 0) return "?";
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << w << " W";
    return os.str();
}

static std::string fmt_time_us(long long us) {
    if (us < 0) return "?";
    if (us >= 1000000) return std::to_string(us / 1000000) + " s";
    if (us >= 1000) return std::to_string(us / 1000) + " ms";
    return std::to_string(us) + " us";
}

// ── Live power from RAPL energy counter deltas ──

struct RAPLState {
    long long prev_energy = -1;
    std::chrono::steady_clock::time_point prev_time;
    bool first = true;
    double live_power_w = -1;
};

static RAPLState g_pkg_rapl;  // tracks package-0 energy

// ── Main ──

static void refresh() {
    int cpu_count = detect_cpu_count();
    std::cout << "\033[H\033[2J";
    std::cout << "\033[?25l";
    std::cout << color(CYN, "══════════════════════════════════════════════════════════") << std::endl;
    std::cout << color(CYN, "  ") + color(WHT, "⚡  POWER STATUS") + color(CYN, "  " + current_time_str()) << std::endl;
    std::cout << color(CYN, "══════════════════════════════════════════════════════════") << std::endl;
    std::cout << std::endl;

    auto supplies = list_power_supplies();
    Battery battery;
    Charger charger;
    bool has_battery = false;
    bool has_charger = false;

    for (auto& [name, type] : supplies) {
        std::string path = "/sys/class/power_supply/" + name;

        if (type == "Battery") {
            if (!has_battery) {
                has_battery = true;
                battery.name = name;
                read_attr(path, "status", battery.status);
                read_attr(path, "health", battery.health);
                read_attr(path, "model_name", battery.model);
                read_attr(path, "manufacturer", battery.manufacturer);
                read_attr(path, "technology", battery.technology);
                read_attr(path, "capacity", battery.capacity);
                read_attr(path, "cycle_count", battery.cycle_count);
                battery.charge_now_mA = read_uA(path, "charge_now");
                battery.charge_full_mA = read_uA(path, "charge_full");
                battery.charge_full_design_mA = read_uA(path, "charge_full_design");
                battery.voltage_now_mV = read_uV(path, "voltage_now");
                battery.current_now_mA = read_uA(path, "current_now");
                battery.temp_C = read_temp(path);
            }
        } else {
            if (!has_charger) {
                has_charger = true;
                charger.name = name;
                read_attr(path, "status", charger.status);
                read_attr(path, "online", charger.online);
                charger.voltage_mV = read_uV(path, "voltage_now");
                charger.current_mA = read_uA(path, "current_now");
                charger.power_mW = read_uW(path, "power_now");
            }
        }
    }

    // ── Battery display ──
    if (has_battery) {
        std::cout << color(YEL, "🔋 Battery") << std::endl;

        std::string status_warn;
        std::string time_remaining;
        bool warning = (battery.status == "Discharging");

        if (warning) {
            time_remaining = format_remaining(
                battery.capacity,
                battery.charge_now_mA,
                battery.current_now_mA
            );
            status_warn = " ⚠ DISCHARGING — " + color(RED, "~" + time_remaining + " remaining");
        }

        std::cout << "   Status:  " << color(status_color(battery.status), battery.status)
                  << status_warn << std::endl;
        std::cout << "   Level:   " << color(WHT, std::to_string(battery.capacity) + "%") << std::endl;
        if (!battery.health.empty())
            std::cout << "   Health:  " << color(WHT, battery.health) << std::endl;
        if (battery.cycle_count >= 0)
            std::cout << "   Cycle:   " << color(WHT, std::to_string(battery.cycle_count)) << std::endl;
        if (battery.temp_C >= 0)
            std::cout << "   Temp:    " << color(WHT, std::to_string(battery.temp_C) + "°C") << std::endl;

        std::string charge_str;
        if (battery.charge_now_mA >= 0 && battery.charge_full_mA >= 0) {
            charge_str = std::to_string(battery.charge_now_mA) + " mAh / "
                       + std::to_string(battery.charge_full_mA) + " mAh";
            if (battery.charge_full_design_mA > 0) {
                charge_str += " (" + std::to_string(battery.charge_full_design_mA) + " mAh design)";
            }
        } else {
            charge_str = "?";
        }
        std::cout << "   Charge:  " << color(WHT, charge_str) << std::endl;

        std::string volt_str = battery.voltage_now_mV >= 0
            ? std::to_string(battery.voltage_now_mV) + " mV" : "?";
        std::cout << "   Voltage: " << color(WHT, volt_str) << std::endl;

        if (battery.current_now_mA >= 0) {
            std::cout << "   Current: " << color(WHT, std::to_string(battery.current_now_mA) + " mA") << std::endl;
        }

        if (!battery.manufacturer.empty() || !battery.model.empty()) {
            std::string model_str = battery.manufacturer;
            if (!battery.model.empty()) model_str += " " + battery.model;
            std::cout << "   Model:   " << color(MAG, model_str) << std::endl;
        }

        if (!battery.technology.empty()) {
            std::cout << "   Tech:    " << color(MAG, battery.technology) << std::endl;
        }
    } else {
        std::cout << color(WHT, "No battery detected") << std::endl;
    }

    std::cout << std::endl;

    // ── Charger display ──
    if (has_charger) {
        std::cout << color(YEL, "🔌 Charger") << " (" + charger.name + ")" << std::endl;

        std::string charger_status_str;
        if (!charger.status.empty()) charger_status_str = charger.status;
        else if (!charger.online.empty()) charger_status_str = (charger.online == "1") ? "Online" : "Offline";
        else charger_status_str = "?";

        std::cout << "   Status: " << color(status_color(charger_status_str), charger_status_str) << std::endl;

        if (charger.voltage_mV >= 0)
            std::cout << "   Voltage: " << color(WHT, std::to_string(charger.voltage_mV) + " mV") << std::endl;
        if (charger.current_mA >= 0)
            std::cout << "   Current: " << color(WHT, std::to_string(charger.current_mA) + " mA") << std::endl;
        if (charger.power_mW >= 0)
            std::cout << "   Power:   " << color(WHT, std::to_string(charger.power_mW) + " mW") << std::endl;
    } else {
        std::cout << color(WHT, "No external charger detected") << std::endl;
    }

    std::cout << std::endl;

    // ── Temperature display ──
    std::cout << color(YEL, "🌡️ Temperatures") << std::endl;

    int cpu_pkg = read_cpu_pkg_temp();
    if (cpu_pkg >= 0) {
        std::cout << "   CPU pkg: " << color(temp_color(cpu_pkg), std::to_string(cpu_pkg) + "°C") << std::endl;
    } else {
        std::cout << "   CPU pkg: " << color(BLK, "N/A") << std::endl;
    }

    auto cores = read_cpu_core_temps();
    if (!cores.empty()) {
        std::cout << "   Cores:";
        for (auto& [label, t] : cores) {
            std::cout << " " << color(temp_color(t), label + ": " + std::to_string(t) + "°C");
        }
        std::cout << std::endl;
    }

    int gpu = read_gpu_temp();
    if (gpu >= 0) {
        std::cout << "   GPU:     " << color(temp_color(gpu), std::to_string(gpu) + "°C") << std::endl;
    } else {
        std::cout << "   GPU:     " << color(BLK, "N/A (no sensor exposed)") << std::endl;
    }

    int nvme = read_nvme_temp();
    if (nvme >= 0) {
        std::cout << "   NVMe:    " << color(temp_color(nvme), std::to_string(nvme) + "°C") << std::endl;
    } else {
        std::cout << "   NVMe:    " << color(BLK, "N/A") << std::endl;
    }

    // Thermal warnings
    if (cpu_pkg >= 90 || gpu >= 90) {
        std::cout << std::endl << color(RED, "⚠ TEMPERATURE WARNING — CPU/GPU approaching critical levels!") << std::endl;
    } else if (cpu_pkg >= 80 || gpu >= 80) {
        std::cout << std::endl << color(MAG, "⚠ HIGH TEMPERATURE — CPU/GPU running hot") << std::endl;
    }

    std::cout << std::endl;

    // ── Performance Limits & Throttling Report ──
    std::cout << color(YEL, "⚙️  Performance Limits & Throttling") << std::endl;

    // 1. MSR-based Perf Limit Reasons (needs root for /dev/cpu/N/msr)
    bool msr_ok = msr_available();

    auto fmt_bits = [](unsigned int bits) -> std::string {
        std::string r;
        unsigned int remaining = bits;
        for (int i = 0; PERF_LIMIT_REASONS[i].name[0]; ++i) {
            unsigned int m = 1u << PERF_LIMIT_REASONS[i].bit;
            if (bits & m) {
                if (!r.empty()) r += ", ";
                r += PERF_LIMIT_REASONS[i].name;
                remaining &= ~m;
            }
        }
        if (remaining) {
            for (int b = 0; b < 16; ++b) {
                if (remaining & (1u << b)) {
                    if (!r.empty()) r += ", ";
                    r += "bit" + std::to_string(b);
                }
            }
        }
        return r;
    };

    if (msr_ok) {
        // Package perf limit reasons (MSR 0x6B0) - current + logged in one MSR
        unsigned long long pkg_reasons = read_msr(0, 0x6B0);
        unsigned int pkg_current = pkg_reasons & 0xFFFF;
        unsigned int pkg_logged  = (pkg_reasons >> 16) & 0xFFFF;

        // Also read LOG-only sticky MSR (0x6B1) - shows different history
        unsigned long long pkg_sticky = read_msr(0, 0x6B1);

        // Update accumulated perf limit event stats
        unsigned int interval_ms = 5000;
        unsigned int newly_active = pkg_current & ~g_prev_pkg_current;
        for (int b = 0; b < 16; ++b) {
            unsigned int m = 1u << b;
            if (newly_active & m) g_pl_stats[b].count++;
            g_pl_stats[b].active = (pkg_current & m) != 0;
            if (pkg_current & m) g_pl_stats[b].total_ms += interval_ms;
        }
        g_prev_pkg_current = pkg_current;

        std::cout << "   Package:" << std::endl;
        std::cout << "     raw 0x6B0: " << color(WHT, hex(pkg_reasons, 16)) << std::endl;
        std::cout << "     raw 0x6B1: " << color(WHT, hex(pkg_sticky, 16)) << std::endl;
        {
            bool active_shown = false;
            if (pkg_current) {
                std::string a = fmt_bits(pkg_current);
                if (!a.empty()) { std::cout << "     active:    " << color(RED, a) << std::endl; active_shown = true; }
            }
            if (pkg_logged) {
                std::string h = fmt_bits(pkg_logged);
                if (!h.empty()) { std::cout << "     history:   " << color(YEL, h) << std::endl; active_shown = true; }
            }
            if (pkg_sticky) {
                unsigned int sl = (pkg_sticky >> 16) & 0xFFFF;
                if (sl) {
                    std::string s = fmt_bits(sl);
                    if (!s.empty()) { std::cout << "     sticky:    " << color(YEL, s) << std::endl; active_shown = true; }
                }
            }
            if (!active_shown) std::cout << "     " << color(GRN, "no throttling") << std::endl;
        }

        // Check each core for active throttling
        bool any_core = false;
        for (int c = 0; c < cpu_count; ++c) {
            unsigned long long cv = read_msr(c, 0x690);
            if (cv) {
                if (!any_core) {
                    std::cout << "   Cores:" << std::endl;
                    any_core = true;
                }
                unsigned int cur = cv & 0xFFFF;
                unsigned int log = (cv >> 16) & 0xFFFF;
                std::string a = fmt_bits(cur);
                std::string h = fmt_bits(log);
                std::cout << "     CPU" << c << ": " << color(WHT, hex(cv, 16));
                if (!a.empty()) std::cout << " active=" << color(RED, a);
                if (!h.empty()) std::cout << " hist=" << color(YEL, h);
                std::cout << std::endl;
            }
        }
        if (!any_core) {
            std::cout << "   Cores:      " << color(GRN, "no active throttling") << std::endl;
        }
    } else {
        std::cout << "   MSR:        " << color(YEL, "unavailable") << " (run with sudo for Perf Limit Reasons)" << std::endl;
    }

    // 2. Thermal throttle stats from sysfs
    {
        // Aggregate across cores for a summary
        int total_core_count = 0;
        int max_core_max = 0;
        int total_core_time = 0;
        int max_pkg_max = 0;
        int max_pkg_total = 0;
        bool any_data = false;

        for (int c = 0; c < cpu_count; ++c) {
            auto t = read_throttle_stats(c);
            if (t.core_count >= 0) any_data = true;
            total_core_count += t.core_count;
            if (t.core_max_ms > max_core_max) max_core_max = t.core_max_ms;
            total_core_time += t.core_total_ms;
            if (t.pkg_max_ms > max_pkg_max) max_pkg_max = t.pkg_max_ms;
            if (t.pkg_total_ms > max_pkg_total) max_pkg_total = t.pkg_total_ms;
        }

        if (any_data) {
            std::cout << "   Throttle events:" << std::endl;
            for (int c = 0; c < cpu_count; ++c) {
                auto t = read_throttle_stats(c);
                if (t.core_count > 0 || t.pkg_count > 0) {
                    std::cout << "     CPU" << c << ": core=" << t.core_count
                              << " (" << t.core_total_ms << "ms, max " << t.core_max_ms << "ms)"
                              << "  pkg=" << t.pkg_count
                              << " (" << t.pkg_total_ms << "ms, max " << t.pkg_max_ms << "ms)" << std::endl;
                }
            }
        } else {
            std::cout << "   Throttle:   " << color(BLK, "N/A") << std::endl;
        }
    }

    // 2b. Accumulated perf limit event stats (since tool start)
    if (msr_ok) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - g_start_time).count();
        bool any_events = false;
        for (int b = 0; b < 16; ++b) {
            if (g_pl_stats[b].count > 0 || g_pl_stats[b].total_ms > 0) {
                any_events = true;
                break;
            }
        }
        if (any_events) {
            std::cout << "   Events (" << elapsed << "s uptime):" << std::endl;
            for (int b = 0; b < 16; ++b) {
                auto& s = g_pl_stats[b];
                if (s.count == 0 && s.total_ms == 0) continue;
                // Find name for this bit
                std::string name = "bit" + std::to_string(b);
                for (int i = 0; PERF_LIMIT_REASONS[i].name[0]; ++i) {
                    if (PERF_LIMIT_REASONS[i].bit == (unsigned)b) {
                        name = PERF_LIMIT_REASONS[i].name;
                        break;
                    }
                }
                std::string active_flag = s.active ? color(RED, " 🔴") : "";
                std::cout << "     " << name << ":" << active_flag
                          << "  " << s.count << " event" << (s.count == 1 ? "" : "s")
                          << "  (" << s.total_ms << "ms)"
                          << std::endl;
            }
        }
    }

    // 3. RAPL power caps + live draw
    {
        auto domains = read_rapl_domains();

        // Compute live package power from energy_uj delta
        if (!domains.empty()) {
            for (auto& d : domains) {
                if (d.name == "package-0" && d.energy_uj >= 0) {
                    auto now = std::chrono::steady_clock::now();
                    if (!g_pkg_rapl.first && g_pkg_rapl.prev_energy >= 0) {
                        long long delta_uj = d.energy_uj - g_pkg_rapl.prev_energy;
                        auto delta_us = std::chrono::duration_cast<std::chrono::microseconds>(now - g_pkg_rapl.prev_time).count();
                        if (delta_us > 0) {
                            g_pkg_rapl.live_power_w = (double)delta_uj / delta_us;
                        }
                    }
                    g_pkg_rapl.prev_energy = d.energy_uj;
                    g_pkg_rapl.prev_time = now;
                    g_pkg_rapl.first = false;
                    break;
                }
            }
        }

        if (!domains.empty()) {
            std::cout << "   Power:" << std::endl;
            bool pkg0_shown = false;
            for (auto& d : domains) {
                // Deduplicate package-0 (may appear from both intel-rapl and intel-rapl-mmio)
                if (d.name == "package-0") {
                    if (pkg0_shown) continue;
                    pkg0_shown = true;
                }
                std::string line = "     " + d.name;
                // Determine effective PL1 — max_constraint is the real hw floor
                long long pl1_effective = d.pl1_uw;
                bool pl1_clamped = false;
                if (d.pl1_max_uw > 0 && d.pl1_max_uw < d.pl1_uw) {
                    pl1_effective = d.pl1_max_uw;
                    pl1_clamped = true;
                }
                // Show live power draw for package-0
                if (d.name == "package-0" && g_pkg_rapl.live_power_w >= 0) {
                    const char* pwr_col = GRN;
                    if (g_pkg_rapl.live_power_w > pl1_effective / 1e6) pwr_col = YEL;
                    if (g_pkg_rapl.live_power_w > (d.pl4_uw > 0 ? d.pl4_uw / 1e6 : 999)) pwr_col = RED;
                    line += " draw: " + color(pwr_col, fmt_power_double(g_pkg_rapl.live_power_w));
                }
                if (d.pl1_uw > 0) {
                    line += "  PL1: " + fmt_power(pl1_effective);
                    if (pl1_clamped) line += color(YEL, " (platform capped)") + " [raw: " + fmt_power(d.pl1_uw) + "]";
                }
                if (d.pl1_window_us > 0) line += " window: " + fmt_time_us(d.pl1_window_us);
                if (d.pl2_uw > 0)   line += "  PL2: " + fmt_power(d.pl2_uw);
                if (d.pl2_window_us > 0) line += " window: " + fmt_time_us(d.pl2_window_us);
                if (d.pl4_uw > 0)   line += "  PL4: " + fmt_power(d.pl4_uw);
                // Check MSR lock status
                if (msr_ok && d.name == "package-0") {
                    unsigned long long msr = read_msr(0, 0x610);
                    if ((msr >> 63) & 1) line += color(RED, " MSR-LOCKED");
                    if ((msr >> 16) & 1) line += color(YEL, " clamp-on");  // PL1 clamp = throttle below min if limit hit
                }
                std::cout << line << std::endl;
            }
        }
    }

    // ── GPU metrics (Xe driver) ──
    {
        // Find any card with a tile0/gt0/freq0 directory (Xe driver)
        std::string xe_freq_base;
        DIR* drm_xe = opendir("/sys/class/drm");
        if (drm_xe) {
            struct dirent* de;
            while ((de = readdir(drm_xe)) != nullptr) {
                std::string card = de->d_name;
                if (card.find("card") != 0) continue;
                std::string candidate = "/sys/class/drm/" + card + "/device/tile0/gt0/freq0";
                if (read_file(candidate + "/cur_freq") != "") {
                    xe_freq_base = candidate;
                    break;
                }
            }
            closedir(drm_xe);
        }

        bool has_gpu = !xe_freq_base.empty();
        if (has_gpu) {
            std::string gt_idle = xe_freq_base.substr(0, xe_freq_base.rfind('/')) + "/gtidle";
            std::cout << "   GPU:" << std::endl;

            // Frequencies
            int cur, actual, max_f, rp0, rpe, rpn;
            std::string line = "     freq:";
            if (read_attr(xe_freq_base, "cur_freq", cur))
                line += " cur " + std::to_string(cur) + " MHz";
            if (read_attr(xe_freq_base, "act_freq", actual))
                line += " act " + std::to_string(actual) + " MHz";
            if (read_attr(xe_freq_base, "max_freq", max_f))
                line += " max " + std::to_string(max_f) + " MHz";
            if (read_attr(xe_freq_base, "rp0_freq", rp0))
                line += " turbo " + std::to_string(rp0) + " MHz";
            if (read_attr(xe_freq_base, "rpe_freq", rpe))
                line += " eff " + std::to_string(rpe) + " MHz";
            if (read_attr(xe_freq_base, "rpn_freq", rpn))
                line += " min " + std::to_string(rpn) + " MHz";
            std::cout << line << std::endl;

            // Throttle status (combined flag + individual reason tracking)
            {
                static GpuThrottleState throttle_state;
                static bool first_throttle = true;

                std::string throttle_dir = xe_freq_base + "/throttle";
                std::string status = read_file(throttle_dir + "/status");
                bool throttling = (status == "1");

                if (!first_throttle)
                    track_gpu_throttle(throttle_state, throttle_dir);
                first_throttle = false;

                std::string throttle_line = "     hw-throttle: ";
                if (throttling) {
                    throttle_line += color(RED, "active");
                } else if (throttle_state.total_events > 0) {
                    throttle_line += color(YEL, "history (" + std::to_string(throttle_state.total_events) + " events)");
                } else {
                    throttle_line += color(GRN, "none");
                }
                if (throttle_state.total_events > 0) {
                    throttle_line += "  [";
                    bool first_reason = true;
                    for (int i = 0; XE_THROTTLE_REASONS[i]; ++i) {
                        if (throttle_state.events[i] > 0) {
                            if (!first_reason) throttle_line += " ";
                            throttle_line += std::string(XE_THROTTLE_REASONS[i]) + ":" + std::to_string(throttle_state.events[i]);
                            first_reason = false;
                        }
                    }
                    throttle_line += "]";
                }
                std::cout << throttle_line << std::endl;
            }

            // Power profile
            std::string profile = read_file(xe_freq_base + "/power_profile");
            if (!profile.empty()) {
                std::string active;
                size_t bs = profile.find('[');
                size_t be = profile.find(']');
                if (bs != std::string::npos && be != std::string::npos && be > bs) {
                    active = profile.substr(bs + 1, be - bs - 1);
                }
                if (!active.empty()) {
                    std::cout << "     profile:  " << color(WHT, active) << std::endl;
                }
            }

            // GT idle / RC6
            std::string idle_status = read_file(gt_idle + "/idle_status");
            long long idle_ms = -1;
            read_attr(gt_idle, "idle_residency_ms", idle_ms);
            if (!idle_status.empty()) {
                std::string idle_line = "     idle:     " + color(idle_status.find("c6") != std::string::npos ? GRN : YEL, idle_status);
                if (idle_ms > 0) {
                    int ih = (int)(idle_ms / 3600000);
                    int im = (int)((idle_ms % 3600000) / 60000);
                    idle_line += " (" + std::to_string(ih) + "h " + std::to_string(im) + "m residency)";
                }
                std::cout << idle_line << std::endl;
            }

            // GPU power from uncore RAPL energy delta
            {
                static long long prev_uncore_energy = -1;
                static std::chrono::steady_clock::time_point prev_uncore_time;
                static bool first_uncore = true;

                // Find uncore RAPL subdomain dynamically
                auto uncore_domains = read_rapl_domains();
                std::string uncore_path;
                for (auto& r : uncore_domains) {
                    if (r.name == "uncore") { uncore_path = r.base_path; break; }
                }

                long long ue = -1;
                if (!uncore_path.empty())
                    read_attr(uncore_path, "energy_uj", ue);
                if (ue < 0) {
                    // Fallback: direct path for intel-rapl:0:1
                    read_attr("/sys/class/powercap/intel-rapl/intel-rapl:0/intel-rapl:0:1", "energy_uj", ue);
                }
                if (ue >= 0) {
                    auto now = std::chrono::steady_clock::now();
                    if (!first_uncore && prev_uncore_energy >= 0) {
                        long long delta_uj = ue - prev_uncore_energy;
                        auto delta_us = std::chrono::duration_cast<std::chrono::microseconds>(now - prev_uncore_time).count();
                        if (delta_us > 0 && delta_uj >= 0) {
                            double gpu_w = (double)delta_uj / delta_us;
                            std::cout << "     power:    " << color(gpu_w > 10 ? YEL : GRN, fmt_power_double(gpu_w)) << std::endl;
                        }
                    }
                    prev_uncore_energy = ue;
                    prev_uncore_time = now;
                    first_uncore = false;
                }
            }
        }
    }

    // 5. intel_pstate & frequency info
    {
        std::string pstate_dir = "/sys/devices/system/cpu/intel_pstate";
        std::string turbo_pct = read_file(pstate_dir + "/turbo_pct");
        std::string max_perf = read_file(pstate_dir + "/max_perf_pct");
        std::string min_perf = read_file(pstate_dir + "/min_perf_pct");
        std::string no_turbo = read_file(pstate_dir + "/no_turbo");
        std::string hwp_status = read_file(pstate_dir + "/status");

        if (!max_perf.empty()) {
            std::cout << "   P-State:    ";
            std::cout << "perf " + max_perf + "/" + min_perf + "%";
            if (!turbo_pct.empty()) std::cout << "  turbo avail: " + turbo_pct + "%";
            if (no_turbo == "1") std::cout << "  " + color(RED, "no_turbo=1");
            if (!hwp_status.empty()) std::cout << "  HWP: " + hwp_status;
            std::cout << std::endl;
        }

        // EPB (energy_perf_bias)
        int epb;
        if (read_attr("/sys/devices/system/cpu/cpu0/power", "energy_perf_bias", epb)) {
            const char* epb_label;
            if (epb <= 0)       epb_label = "performance";
            else if (epb <= 4)  epb_label = "balance_performance";
            else if (epb <= 8)  epb_label = "normal";
            else if (epb <= 12) epb_label = "balance_power";
            else                epb_label = "power";
            std::string epb_color_str = (epb <= 4) ? GRN : (epb <= 8) ? WHT : YEL;
            std::cout << "   EPB:        " << color(epb_color_str.c_str(), std::to_string(epb) + " (" + epb_label + ")") << std::endl;
        }

        // Current freq on cpu0 as a quick view
        int cur_freq_khz;
        if (read_attr("/sys/devices/system/cpu/cpu0/cpufreq", "scaling_cur_freq", cur_freq_khz)) {
            int base_freq_khz;
            std::string freq_str;
            if (read_attr("/sys/devices/system/cpu/cpu0/cpufreq", "base_frequency", base_freq_khz)) {
                std::ostringstream os;
                os << std::fixed << std::setprecision(1) << (cur_freq_khz / 1000000.0) << "/" << (base_freq_khz / 1000000.0) << " GHz";
                freq_str = os.str();
            } else {
                std::ostringstream os;
                os << std::fixed << std::setprecision(1) << (cur_freq_khz / 1000000.0) << " GHz";
                freq_str = os.str();
            }
            // Compare against max
            int max_freq_khz;
            if (read_attr("/sys/devices/system/cpu/cpu0/cpufreq", "cpuinfo_max_freq", max_freq_khz) && max_freq_khz > 0) {
                double ratio = (double)cur_freq_khz / max_freq_khz;
                std::ostringstream os;
                os << std::fixed << std::setprecision(1) << (max_freq_khz / 1000000.0) << " GHz";
                std::cout << "   Freq:       " << color(ratio > 0.9 ? GRN : ratio > 0.5 ? WHT : YEL, freq_str);
                std::cout << " of " << color(WHT, os.str());
                if (ratio < 0.5 && msr_ok) {
                    std::cout << " " + color(RED, "(throttled)");
                }
                std::cout << std::endl;
            } else {
                std::cout << "   Freq:       " << color(WHT, freq_str) << std::endl;
            }
        }
    }

    // ── Throttling summary (only known active reasons) ──
    if (msr_ok) {
        unsigned long long pkg_now = read_msr(0, 0x6B0);
        unsigned int cur = pkg_now & 0xFFFF;
        bool known_active = false;
        for (int i = 0; PERF_LIMIT_REASONS[i].name[0]; ++i)
            if (cur & (1u << PERF_LIMIT_REASONS[i].bit)) { known_active = true; break; }
        if (known_active) {
            std::cout << color(RED, "⚠ PACKAGE THROTTLING ACTIVE — performance is being limited") << std::endl;
        }
    }
    // Check sysfs throttle counts accumulated since last boot
    {
        int worst_core = 0, worst_pkg = 0;
        for (int c = 0; c < cpu_count; ++c) {
            auto t = read_throttle_stats(c);
            if (t.core_count > worst_core) worst_core = t.core_count;
            if (t.pkg_count > worst_pkg) worst_pkg = t.pkg_count;
        }
        if (worst_pkg > 1000 || worst_core > 1000) {
            std::cout << color(YEL, "⚠ High throttle event counts — " +
                std::to_string(worst_core) + " core / " +
                std::to_string(worst_pkg) + " package events since boot") << std::endl;
        }
    }

    // ── Tuning capability summary ──
    if (msr_ok) {
        unsigned long long msr610 = read_msr(0, 0x610);
        bool msr_locked = (msr610 >> 63) & 1;
        bool pl1_clamp = (msr610 >> 16) & 1;
        long long pl1_max = 0;
        // Find first package-0 domain for PL1 max cap info
        for (auto& rd : read_rapl_domains()) {
            if (rd.name == "package-0" && rd.pl1_max_uw > 0) {
                pl1_max = rd.pl1_max_uw;
                break;
            }
        }

        std::cout << std::endl;
        std::cout << color(YEL, "🔧  Tuning") << std::endl;

        if (msr_locked) {
            std::cout << "   RAPL MSR:  " << color(RED, "LOCKED by BIOS") << " — power limits cannot be changed" << std::endl;
        } else {
            std::cout << "   RAPL MSR:  " << color(GRN, "unlocked") << " — PL1/PL2 MSR writable";
            if (pl1_max > 0) std::cout << " (sysfs caps PL1 at " + fmt_power(pl1_max) + ")";
            std::cout << std::endl;
            if (pl1_clamp) {
                std::cout << "   PL1 clamp: " << color(YEL, "enabled") << " — CPU will throttle below min freq if limit exceeded" << std::endl;
            }
            // Check GPU max_freq writability — find Xe GT0 freq0 path dynamically
            int gpu_max = 0, gpu_rp0 = 0;
            std::string gpu_freq_path;
            DIR* drm_tune = opendir("/sys/class/drm");
            if (drm_tune) {
                struct dirent* de;
                while ((de = readdir(drm_tune)) != nullptr) {
                    std::string card = de->d_name;
                    if (card.find("card") != 0) continue;
                    std::string candidate = "/sys/class/drm/" + card + "/device/tile0/gt0/freq0";
                    if (read_file(candidate + "/cur_freq") != "") {
                        gpu_freq_path = candidate;
                        break;
                    }
                }
                closedir(drm_tune);
            }
            if (!gpu_freq_path.empty() &&
                read_attr(gpu_freq_path, "max_freq", gpu_max) &&
                read_attr(gpu_freq_path, "rp0_freq", gpu_rp0)) {
                std::string gpu_tune = "   GPU freq:  max=" + std::to_string(gpu_max) + " MHz  turbo cap=" + std::to_string(gpu_rp0) + " MHz";
                if (gpu_max < gpu_rp0) gpu_tune += color(YEL, " (apply_max_performance() via forcewake can unlock)");
                std::cout << gpu_tune << std::endl;
            }
        }
    }

    std::cout << std::endl;

    // ── Warning banner if discharging ──
    if (has_battery && battery.status == "Discharging") {
        std::cout << color(RED, "╔══════════════════════════════════════════════════════════╗") << std::endl;
        std::cout << color(RED, "║  ⚠ WARNING — Battery is DISCHARGING                    ║") << std::endl;
        std::cout << color(RED, "║  Plug in your charger to avoid unexpected shutdown.     ║") << std::endl;
        std::cout << color(RED, "╚══════════════════════════════════════════════════════════╝") << std::endl;
    }

    std::cout << "\033[?25h";
}

int main(int argc, char** argv) {
    bool clear_plr = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--clear-plr") == 0) {
            clear_plr = true;
        }
    }

    // Clear PLR history before entering the monitoring loop.
    // This resets all sticky/perf-limit bits so new throttling events
    // (including sub-second blips) are observed from a clean slate.
    if (clear_plr) {
        std::cout << "\033[H\033[2J";  // clear screen
        clear_perf_limit_history();
    }

    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    const int interval_sec = 5;
    while (g_running) {
        refresh();
        if (!g_running) break;
        std::this_thread::sleep_for(std::chrono::seconds(interval_sec));
    }
    std::cout << "\033[?25h";
    return 0;
}
