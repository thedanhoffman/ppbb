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
#include "power-utils.h"

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
    // Try x86_pkg_temp first (most accurate for CPU package).
    // Fallback to SOC DTS or CPU thermal zones if x86_pkg_temp is not available.
    // On Meteor Lake, multiple thermal zones may exist; we take the highest temp.
    DIR* dir = opendir("/sys/class/thermal");
    if (!dir) return -1;
    int max_temp = -1;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name[0] == '.') continue;
        std::string type = sysfs_read_file("/sys/class/thermal/" + name + "/type");
        if (type == "x86_pkg_temp" || type == "SOC DTS" || type == "CPU") {
            int t;
            if (sysfs_read_attr("/sys/class/thermal/" + name, "temp", t)) {
                int celsius = safe_celsius(t / 1000);
                if (celsius > max_temp) max_temp = celsius;
            }
        }
    }
    closedir(dir);
    return max_temp;
}



static int read_gpu_temp() {
    // Try hwmon first — works for i915, amdgpu, and Xe dGPU.
    // Xe dGPU hwmon is named "xe" but is only available for discrete GPUs (IS_DGFX).
    // On integrated GPUs (Meteor Lake iGPU), there is no GPU-specific hwmon.
    {
        DIR* hwmon_dir = opendir("/sys/class/hwmon");
        if (hwmon_dir) {
            struct dirent* entry;
            while ((entry = readdir(hwmon_dir)) != nullptr) {
                std::string name = entry->d_name;
                if (name[0] == '.') continue;
                std::string hname = sysfs_read_file("/sys/class/hwmon/" + name + "/name");
                if (hname.find("i915") != std::string::npos ||
                    hname.find("amdgpu") != std::string::npos ||
                    hname.find("xe") != std::string::npos) {
                    int t;
                    if (sysfs_read_attr("/sys/class/hwmon/" + name, "temp1_input", t)) {
                        closedir(hwmon_dir);
                        return safe_celsius(t / 1000);
                    }
                }
            }
            closedir(hwmon_dir);
        }
    }

    // Fallback: scan thermal zones for GPU-related sensors.
    // On Meteor Lake iGPU, there is no dedicated GPU thermal zone.
    // x86_pkg_temp gives package temp (CPU+GPU), which is the closest thing.
    // SOC DTS / CPU zones may also be relevant.
    DIR* thermal_dir = opendir("/sys/class/thermal");
    if (!thermal_dir) return -1;
    int pkg_temp = -1;
    struct dirent* entry;
    while ((entry = readdir(thermal_dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name[0] == '.') continue;
        std::string type = sysfs_read_file("/sys/class/thermal/" + name + "/type");
        if (type == "x86_pkg_temp" || type == "SOC DTS" || type == "CPU") {
            int t;
            if (sysfs_read_attr("/sys/class/thermal/" + name, "temp", t)) {
                int celsius = safe_celsius(t / 1000);
                if (celsius > pkg_temp) pkg_temp = celsius;
            }
        }
    }
    closedir(thermal_dir);
    return pkg_temp;
}

static int read_nvme_temp() {
    DIR* dir = opendir("/sys/class/hwmon");
    if (!dir) return -1;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string hwmon = entry->d_name;
        if (hwmon[0] == '.') continue;
        std::string name = sysfs_read_file("/sys/class/hwmon/" + hwmon + "/name");
        if (name.find("nvme") != std::string::npos) {
            int t;
            if (sysfs_read_attr("/sys/class/hwmon/" + hwmon, "temp1_input", t)) {
                closedir(dir);
                return safe_celsius(t / 1000);
            }
        }
    }
    closedir(dir);
    return -1;
}



// ── Global for signal handler ──
static volatile sig_atomic_t g_running = 1;

static void handle_signal(int) { g_running = 0; }

// ── GPU throttle event tracking (Xe driver reason_* files) ──
// Uses shared XE_THROTTLE_REASONS/XE_THROTTLE_FILES from power-utils.h

struct GpuThrottleState {
    int events[8] = {0};
    int total_events = 0;
    bool prev_active[8] = {false};
    bool cur_active[8] = {false};   // current read state (0/1 from sysfs)
};

static void track_gpu_throttle(GpuThrottleState& state, const std::string& throttle_dir) {
    for (int i = 0; XE_THROTTLE_FILES[i]; ++i) {
        int v = 0;
        sysfs_read_attr(throttle_dir, XE_THROTTLE_FILES[i], v);
        bool active = (v != 0);
        if (active && !state.prev_active[i]) {
            state.events[i]++;
            state.total_events++;
        }
        state.prev_active[i] = active;
        state.cur_active[i] = active;
    }
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
// Using shared PERF_LIMIT_REASONS_BITS from power-utils.h
// Perf limit event tracking (accumulated across refresh cycles)
struct PerfEventStats {
    int count = 0;
    int total_ms = 0;
    bool active = false;
};
static PerfEventStats g_pl_stats[16]; // indexed by bit number (0-15)
static unsigned int g_prev_pkg_current = 0;
static auto g_start_time = std::chrono::steady_clock::now();

// GPU throttle state (shared between perf-limit table and GPU display)
static GpuThrottleState g_gpu_throttle;
static bool g_gpu_init = false;
static std::string g_xe_freq_base;
static std::string g_gpu_throttle_dir;

// Discover Xe GPU throttle path and read current state (called once on first refresh).
// Populates g_gpu_throttle, g_xe_freq_base, g_gpu_throttle_dir.
static void init_gpu_throttle() {
    if (g_gpu_init) {
        if (!g_gpu_throttle_dir.empty())
            track_gpu_throttle(g_gpu_throttle, g_gpu_throttle_dir);
        return;
    }
    DIR* drm_dir = opendir("/sys/class/drm");
    if (!drm_dir) return;
    struct dirent* de;
    while ((de = readdir(drm_dir)) != nullptr) {
        std::string card = de->d_name;
        if (card.find("card") != 0) continue;
        std::string candidate = "/sys/class/drm/" + card + "/device/tile0/gt0/freq0";
        if (sysfs_read_file(candidate + "/cur_freq") != "") {
            g_xe_freq_base = candidate;
            g_gpu_throttle_dir = candidate + "/throttle";
            // First read: snapshot baseline state only (no event counting).
            // GPU throttle bits can be sticky — a stale bit from a past event
            // must not be counted as a new event.
            for (int i = 0; XE_THROTTLE_FILES[i]; ++i) {
                int v = 0;
                sysfs_read_attr(g_gpu_throttle_dir, XE_THROTTLE_FILES[i], v);
                g_gpu_throttle.cur_active[i] = (v != 0);
                g_gpu_throttle.prev_active[i] = g_gpu_throttle.cur_active[i];
            }
            g_gpu_init = true;
            break;
        }
    }
    closedir(drm_dir);
}

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
    sysfs_read_attr(dir, "core_throttle_count", s.core_count);
    sysfs_read_attr(dir, "core_throttle_max_time_ms", s.core_max_ms);
    sysfs_read_attr(dir, "core_throttle_total_time_ms", s.core_total_ms);
    sysfs_read_attr(dir, "package_throttle_count", s.pkg_count);
    sysfs_read_attr(dir, "package_throttle_max_time_ms", s.pkg_max_ms);
    sysfs_read_attr(dir, "package_throttle_total_time_ms", s.pkg_total_ms);
    return s;
}

// ── RAPL powercap reader ──

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

// ── ANSI visual-width helper ──
// Counts visible characters, stripping \033[...m escape sequences.
static size_t ansi_visual_len(const std::string& s) {
    size_t v = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < s.size() && s[j] != 'm') ++j;
            i = j; // advance past the closing 'm'
        } else {
            ++v;
        }
    }
    return v;
}

// ── Box-drawn table helper ──
// Usage:
//   Table t({"header1", "h2"});
//   t.row({"cell1", "val"});
//   t.print();
struct Table {
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows;
    std::vector<size_t> widths;
    std::string indent;

    Table() = default;
    Table(const std::vector<std::string>& hdr, const std::string& ind = "   ")
        : headers(hdr), indent(ind) {
        for (auto& h : headers) widths.push_back(h.size());
    }
    void row(const std::vector<std::string>& cells) {
        rows.push_back(cells);
        for (size_t i = 0; i < (std::min)(cells.size(), headers.size()); ++i) {
            size_t vw = ansi_visual_len(cells[i]);
            if (vw > widths[i]) widths[i] = vw;
        }
    }
    void print() const {
        auto pad = [](const std::string& s, size_t w) {
            size_t vw = ansi_visual_len(s);
            return s + std::string(std::max(w, vw) - vw, ' ');
        };

        // but we need different corner chars for top/mid/bottom
        // Box-drawing chars are multi-byte UTF-8 — use string repeat helper.
        auto repeat = [](const std::string& s, size_t n) {
            std::string r;
            r.reserve(s.size() * n);
            for (size_t i = 0; i < n; ++i) r += s;
            return r;
        };
        auto top_sep = [this,&repeat]() -> std::string {
            std::string line = indent + "┌";
            for (size_t i = 0; i < headers.size(); ++i) {
                if (i < headers.size() - 1)
                    line += repeat("─", widths[i] + 1) + "┬";
                else
                    line += repeat("─", widths[i] + 1) + "┐";
            }
            return line;
        };
        auto mid_sep = [this,&repeat]() -> std::string {
            std::string line = indent + "├";
            for (size_t i = 0; i < headers.size(); ++i) {
                if (i < headers.size() - 1)
                    line += repeat("─", widths[i] + 1) + "┼";
                else
                    line += repeat("─", widths[i] + 1) + "┤";
            }
            return line;
        };
        auto bot_sep = [this,&repeat]() -> std::string {
            std::string line = indent + "└";
            for (size_t i = 0; i < headers.size(); ++i) {
                if (i < headers.size() - 1)
                    line += repeat("─", widths[i] + 1) + "┴";
                else
                    line += repeat("─", widths[i] + 1) + "┘";
            }
            return line;
        };
        auto print_row = [this,&pad](const std::vector<std::string>& cells) {
            std::string line = indent + "│";
            for (size_t i = 0; i < headers.size(); ++i) {
                line += " " + pad(i < cells.size() ? cells[i] : "", widths[i]) + "│";
            }
            std::cout << line << std::endl;
        };

        std::cout << top_sep() << std::endl;
        print_row(headers);
        if (!rows.empty()) {
            std::cout << mid_sep() << std::endl;
            for (auto& r : rows) print_row(r);
        }
        std::cout << bot_sep() << std::endl;
    }
};

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

    // ── Power supply (charger only — battery info removed) ──
    {
        DIR* ps = opendir("/sys/class/power_supply");
        std::string charger_name, charger_power_mw;
        bool on_ac = false;
        if (ps) {
            struct dirent* entry;
            while ((entry = readdir(ps)) != nullptr) {
                std::string name = entry->d_name;
                if (name[0] == '.') continue;
                std::string type = sysfs_read_file("/sys/class/power_supply/" + name + "/type");
                if (type == "Mains") {
                    std::string online = sysfs_read_file("/sys/class/power_supply/" + name + "/online");
                    if (online == "1") {
                        on_ac = true;
                        charger_name = name;
                        long long pwr = 0;
                        sysfs_read_attr("/sys/class/power_supply/" + name, "power_now", pwr);
                        if (pwr > 0) charger_power_mw = std::to_string(pwr / 1000) + " W";
                    }
                } else if (type == "Battery") {
                    std::string status = sysfs_read_file("/sys/class/power_supply/" + name + "/status");
                    if (status == "Discharging") on_ac = false;
                }
            }
            closedir(ps);
        }
        std::cout << "   AC:       " << color(on_ac ? GRN : RED, on_ac ? "connected" : "disconnected");
        if (!charger_power_mw.empty())
            std::cout << "  " << color(WHT, charger_power_mw);
        std::cout << std::endl;
        std::cout << std::endl;
    }

    // ── Temperature display ──
    std::cout << color(YEL, "🌡️ Temperatures") << std::endl;

    int cpu_pkg = read_cpu_pkg_temp();
    if (cpu_pkg >= 0) {
        std::cout << "   CPU pkg: " << color(temp_color(cpu_pkg), std::to_string(cpu_pkg) + "°C") << std::endl;
    } else {
        std::cout << "   CPU pkg: " << color(BLK, "N/A") << std::endl;
    }

    int gpu = read_gpu_temp();
    if (gpu >= 0) {
        std::cout << "   GPU:     " << color(temp_color(gpu), std::to_string(gpu) + "°C") << std::endl;
    } else {
        std::cout << "   GPU:     " << color(BLK, "N/A (iGPU: no dedicated temp sensor; see CPU pkg)") << std::endl;
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

    // ── Helper: format a bitmask as comma-separated reason names ──
    auto fmt_bits = [](unsigned int bits) -> std::string {
        std::string r;
        unsigned int remaining = bits;
        for (int i = 0; PERF_LIMIT_REASONS[i].name && PERF_LIMIT_REASONS[i].name[0]; ++i) {
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

    // ── Perf Limit Reasons table ──
    bool msr_ok = msr_available();
    unsigned long long pkg_reasons = 0, pkg_sticky = 0;
    unsigned int pkg_current = 0, pkg_logged = 0;
    if (msr_ok) {
        pkg_reasons = read_msr(0, 0x6B0);
        pkg_current = pkg_reasons & 0xFFFF;
        pkg_logged  = (pkg_reasons >> 16) & 0xFFFF;
        pkg_sticky  = read_msr(0, 0x6B1);

        // Update accumulated stats
        unsigned int newly_active = pkg_current & ~g_prev_pkg_current;
        for (int b = 0; b < 16; ++b) {
            unsigned int m = 1u << b;
            if (newly_active & m) g_pl_stats[b].count++;
            g_pl_stats[b].active = (pkg_current & m) != 0;
            if (pkg_current & m) g_pl_stats[b].total_ms += 5000;
        }
        g_prev_pkg_current = pkg_current;
    }

    // Read per-core MSR 0x690
    struct CorePlr {
        unsigned int cur = 0, log = 0;
    };
    std::vector<CorePlr> core_plrs(cpu_count);
    if (msr_ok) {
        for (int c = 0; c < cpu_count; ++c) {
            unsigned long long cv = read_msr(c, 0x690);
            core_plrs[c].cur = cv & 0xFFFF;
            core_plrs[c].log = (cv >> 16) & 0xFFFF;
        }
    }

    // Read GPU throttle state early so it's available for the perf-limit table
    init_gpu_throttle();

    {
        auto dot = [](bool on) { return on ? color(RED, "x") : " "; };
        Table t({"bit", "reason", "pkg cur", "pkg log", "pkg stk", "cores"});
        for (int i = 0; PERF_LIMIT_REASONS[i].name && PERF_LIMIT_REASONS[i].name[0]; ++i) {
            unsigned int bit = PERF_LIMIT_REASONS[i].bit;
            unsigned int m = 1u << bit;
            bool in_pkg_cur = (pkg_current & m) != 0;
            bool in_pkg_log = (pkg_logged & m) != 0;
            bool in_pkg_stk = (msr_ok && ((pkg_sticky >> 16) & 0xFFFF) & m) != 0;

            std::string core_str;
            for (int c = 0; c < cpu_count; ++c) {
                if (core_plrs[c].cur & m) {
                    if (!core_str.empty()) core_str += ",";
                    core_str += std::to_string(c);
                }
            }
            if (core_str.empty()) {
                for (int c = 0; c < cpu_count; ++c) {
                    if (core_plrs[c].log & m) {
                        if (!core_str.empty()) core_str += ",";
                        core_str += std::to_string(c);
                    }
                }
            }
            t.row({std::to_string(bit), PERF_LIMIT_REASONS[i].name,
                   dot(in_pkg_cur), dot(in_pkg_log), dot(in_pkg_stk), core_str});
        }
        t.print();
    }
    if (!msr_ok) {
        std::cout << color(YEL, "   ⚠ MSR unavailable — run with sudo for Perf Limit Reasons") << std::endl;
    }

    // ── GPU Perf Limit Reasons ──
    {
        auto dot = [](bool on) { return on ? color(RED, "x") : " "; };
        Table t({"reason", "active", "events"});
        for (int i = 0; XE_THROTTLE_REASONS[i]; ++i) {
            t.row({XE_THROTTLE_REASONS[i],
                   dot(g_gpu_throttle.cur_active[i]),
                   std::to_string(g_gpu_throttle.events[i])});
        }
        if (g_gpu_init) {
            std::cout << "   GPU Perf Limits (Xe):" << std::endl;
            t.print();
        }
    }

    std::cout << std::endl;

    // ── Thermal throttle stats ──
    {
        bool any_data = false;
        Table t({"cpu", "core_cnt", "core_ms", "pkg_cnt"});
        for (int c = 0; c < cpu_count; ++c) {
            auto s = read_throttle_stats(c);
            if (s.core_count >= 0) any_data = true;
            if (s.core_count > 0 || s.pkg_count > 0) {
                t.row({"cpu" + std::to_string(c),
                       std::to_string(s.core_count),
                       std::to_string(s.core_total_ms),
                       std::to_string(s.pkg_count)});
            }
        }
        if (any_data && !t.rows.empty()) {
            std::cout << "   Throttle (since boot):" << std::endl;
            t.print();
        }
    }

    // ── Accumulated events since tool start ──
    if (msr_ok) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - g_start_time).count();
        Table t({"reason", "cnt", "total_ms"});
        for (int b = 0; b < 16; ++b) {
            auto& s = g_pl_stats[b];
            if (s.count == 0 && s.total_ms == 0) continue;
            std::string name = "bit" + std::to_string(b);
            for (int i = 0; PERF_LIMIT_REASONS[i].name && PERF_LIMIT_REASONS[i].name[0]; ++i) {
                if (PERF_LIMIT_REASONS[i].bit == (unsigned)b) { name = PERF_LIMIT_REASONS[i].name; break; }
            }
            if (s.active) name += color(RED, " x");
            t.row({name, std::to_string(s.count), std::to_string(s.total_ms)});
        }
        if (!t.rows.empty()) {
            std::cout << "   Events (" << elapsed << "s uptime):" << std::endl;
            t.print();
        }
    }

    // ── RAPL domains table ──
    {
        auto domains = read_all_rapl_domains();

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

        // Table: domain | draw | PL1 | PL2 | PL4 | lock
        std::cout << std::endl;
        Table t({"domain", "draw", "PL1", "PL2", "PL4", "lock"});

        auto val_cell = [](long long uw, long long window_us) -> std::string {
            if (uw == -1) return std::string(BLK) + "-" + RST;
            if (uw == 0)  return std::string(YEL) + "inf" + RST;
            std::string s = fmt_power(uw);
            if (window_us > 0) s += " (" + fmt_time_us(window_us) + ")";
            return s;
        };

        bool pkg0_shown = false;
        for (auto& d : domains) {
            if (d.name == "package-0" && pkg0_shown) continue;
            if (d.name == "package-0") pkg0_shown = true;

            std::string label = d.name;
            if (d.name == "uncore") label = "uncore (GPU)";
            else if (d.name == "pp1") label = "pp1 (GPU)";

            long long pl1_eff = d.pl1_uw;
            bool clamped = false;
            if (d.pl1_max_uw > 0 && d.pl1_max_uw < d.pl1_uw) {
                pl1_eff = d.pl1_max_uw;
                clamped = true;
            }

            std::string draw;
            if (d.name == "package-0" && g_pkg_rapl.live_power_w >= 0) {
                const char* col = GRN;
                if (g_pkg_rapl.live_power_w > pl1_eff / 1e6) col = YEL;
                if (g_pkg_rapl.live_power_w > (d.pl4_uw > 0 ? d.pl4_uw / 1e6 : 999)) col = RED;
                draw = color(col, fmt_power_double(g_pkg_rapl.live_power_w));
            }

            std::string pl1_cell = val_cell(pl1_eff, d.pl1_window_us);
            if (clamped) pl1_cell += color(YEL, "!");

            std::string lock;
            if (msr_ok && d.name == "package-0") {
                unsigned long long msr = read_msr(0, 0x610);
                if ((msr >> 63) & 1) lock = color(RED, "LOCKED");
                else if ((msr >> 16) & 1) lock = color(YEL, "clamp-on");
                else lock = color(GRN, "unlocked");
            }

            t.row({label, draw, pl1_cell, val_cell(d.pl2_uw, d.pl2_window_us),
                   val_cell(d.pl4_uw, d.pl4_window_us), lock});
        }
        t.print();
    }

    // ── GPU metrics (Xe driver) ──
    // g_xe_freq_base / g_gpu_throttle already populated by init_gpu_throttle() above.
    if (!g_xe_freq_base.empty()) {
        std::string gt_idle = g_xe_freq_base.substr(0, g_xe_freq_base.rfind('/')) + "/gtidle";
        std::cout << "   GPU:" << std::endl;

        // Throttle status — use 'reasons' file, not 'status'.
        // The 'status' file reads the raw MSR with mask U32_MAX, so any
        // garbage/stale bit in the register returns 1, even when the GPU
        // is idle at 0W. The 'reasons' file masks with
        // GT0_PERF_LIMIT_REASONS_MASK (0xde3) and returns "none" when clean.
        // See xe_gt_throttle.c:
        //   "It's preferred over monitoring status and then reading the
        //    reason from individual attributes since that is racy."
        {
            std::string reasons = sysfs_read_file(g_gpu_throttle_dir + "/reasons");
            reasons.erase(reasons.find_last_not_of(" \n\r\t") + 1); // trim
            bool throttling = (!reasons.empty() && reasons != "none");

            std::string throttle_line = "     hw-throttle: ";
            if (throttling) {
                throttle_line += color(RED, "active") + " [" + reasons + "]";
            } else if (g_gpu_throttle.total_events > 0) {
                throttle_line += color(YEL, "history (" + std::to_string(g_gpu_throttle.total_events) + " events)");
            } else {
                throttle_line += color(GRN, "none");
            }
            if (g_gpu_throttle.total_events > 0) {
                throttle_line += "  [";
                bool first_reason = true;
                for (int i = 0; XE_THROTTLE_REASONS[i]; ++i) {
                    if (g_gpu_throttle.events[i] > 0) {
                        if (!first_reason) throttle_line += " ";
                        throttle_line += std::string(XE_THROTTLE_REASONS[i]) + ":" + std::to_string(g_gpu_throttle.events[i]);
                        first_reason = false;
                    }
                }
                throttle_line += "]";
            }
            std::cout << throttle_line << std::endl;
        }

        // GPU idle state and C0 residency
        {
                std::string idle_status = sysfs_read_file(gt_idle + "/idle_status");
                std::string idle_line = "     idle: ";
                if (idle_status.find("c6") != std::string::npos)
                    idle_line += color(GRN, idle_status);
                else if (idle_status.find("c0") != std::string::npos)
                    idle_line += color(RED, idle_status);
                else
                    idle_line += color(YEL, idle_status);

                // C0 residency (activity percentage over last measurement window)
                // Path: gt0/activity/c0_residency_ms — returns microseconds despite name.
                {
                    static long long prev_c0_us = 0;
                    static std::chrono::steady_clock::time_point prev_c0_time;
                    std::string c0_path = gt_idle + "/../activity/c0_residency_ms";
                    std::string c0_str = sysfs_read_file(c0_path);
                    long long c0_us = 0;
                    if (!c0_str.empty()) {
                        try { c0_us = std::stoll(c0_str); } catch (...) { c0_us = 0; }
                    }
                    if (c0_us > 0 && prev_c0_us > 0) {
                        auto now = std::chrono::steady_clock::now();
                        long long delta_c0 = c0_us - prev_c0_us;
                        long long delta_time = std::chrono::duration_cast<std::chrono::microseconds>(now - prev_c0_time).count();
                        if (delta_c0 > 0 && delta_time > 0) {
                            double c0_pct = std::min(1.0, (double)delta_c0 / (double)delta_time);
                            idle_line += "  c0=" + color(c0_pct > 0.7 ? RED : c0_pct > 0.3 ? YEL : GRN,
                                                         std::to_string((int)(c0_pct * 100)) + "%");
                        }
                    }
                    prev_c0_us = c0_us;
                    prev_c0_time = std::chrono::steady_clock::now();
                }
                std::cout << idle_line << std::endl;
            }

            // Power profile
            std::string profile = sysfs_read_file(g_xe_freq_base + "/power_profile");
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

            // GT1 (media engine) — min/max freq, profile
            {
                std::string gt1_freq_base = g_xe_freq_base.substr(0, g_xe_freq_base.rfind('/'))
                    + "/gt1/freq0";
                std::string gt1_cur = sysfs_read_file(gt1_freq_base + "/cur_freq");
                if (!gt1_cur.empty()) {
                    std::string gt1_line = "     GT1:    ";
                    int gt1_min = 0, gt1_max = 0;
                    sysfs_read_attr(gt1_freq_base, "min_freq", gt1_min);
                    sysfs_read_attr(gt1_freq_base, "max_freq", gt1_max);
                    gt1_line += "cur " + gt1_cur + " MHz";
                    if (gt1_min > 0) gt1_line += " min " + std::to_string(gt1_min) + " MHz";
                    if (gt1_max > 0) gt1_line += " max " + std::to_string(gt1_max) + " MHz";
                    if (gt1_max > 0 && gt1_min > 0 && gt1_max <= gt1_min + 10)
                        gt1_line += color(YEL, " (capped to min_freq)");
                    std::string gt1_prof = sysfs_read_file(gt1_freq_base + "/power_profile");
                    if (!gt1_prof.empty()) {
                        size_t bs = gt1_prof.find('[');
                        size_t be = gt1_prof.find(']');
                        if (bs != std::string::npos && be != std::string::npos && be > bs)
                            gt1_line += " profile: " + color(WHT, gt1_prof.substr(bs + 1, be - bs - 1));
                    }
                    std::cout << gt1_line << std::endl;
                }
            }

            // GPU power from uncore RAPL energy delta
            {
                static long long prev_uncore_energy = -1;
                static std::chrono::steady_clock::time_point prev_uncore_time;
                static bool first_uncore = true;

                // Find uncore RAPL subdomain dynamically
                auto uncore_domains = read_all_rapl_domains();
                std::string uncore_path;
                for (auto& r : uncore_domains) {
                    if (r.name == "uncore") { uncore_path = r.base_path; break; }
                }

                long long ue = -1;
                if (!uncore_path.empty())
                    sysfs_read_attr(uncore_path, "energy_uj", ue);
                if (ue < 0) {
                    // Fallback: direct path for intel-rapl:0:1
                    sysfs_read_attr("/sys/class/powercap/intel-rapl/intel-rapl:0/intel-rapl:0:1", "energy_uj", ue);
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

    // 5. intel_pstate & frequency info
    {
        std::string pstate_dir = "/sys/devices/system/cpu/intel_pstate";
        std::string turbo_pct = sysfs_read_file(pstate_dir + "/turbo_pct");
        std::string max_perf = sysfs_read_file(pstate_dir + "/max_perf_pct");
        std::string min_perf = sysfs_read_file(pstate_dir + "/min_perf_pct");
        std::string no_turbo = sysfs_read_file(pstate_dir + "/no_turbo");
        std::string hwp_status = sysfs_read_file(pstate_dir + "/status");

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
        if (sysfs_read_attr("/sys/devices/system/cpu/cpu0/power", "energy_perf_bias", epb)) {
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
        if (sysfs_read_attr("/sys/devices/system/cpu/cpu0/cpufreq", "scaling_cur_freq", cur_freq_khz)) {
            int base_freq_khz;
            std::string freq_str;
            if (sysfs_read_attr("/sys/devices/system/cpu/cpu0/cpufreq", "base_frequency", base_freq_khz)) {
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
            if (sysfs_read_attr("/sys/devices/system/cpu/cpu0/cpufreq", "cpuinfo_max_freq", max_freq_khz) && max_freq_khz > 0) {
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
        for (int i = 0; PERF_LIMIT_REASONS[i].name && PERF_LIMIT_REASONS[i].name[0]; ++i)
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
        for (auto& rd : read_all_rapl_domains()) {
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
                    if (sysfs_read_file(candidate + "/cur_freq") != "") {
                        gpu_freq_path = candidate;
                        break;
                    }
                }
                closedir(drm_tune);
            }
            if (!gpu_freq_path.empty() &&
                sysfs_read_attr(gpu_freq_path, "max_freq", gpu_max) &&
                sysfs_read_attr(gpu_freq_path, "rp0_freq", gpu_rp0)) {
                std::string gpu_tune = "   GPU freq:  max=" + std::to_string(gpu_max) + " MHz  turbo cap=" + std::to_string(gpu_rp0) + " MHz";
                if (gpu_max < gpu_rp0) gpu_tune += color(YEL, " (apply_max_performance() via forcewake can unlock)");
                std::cout << gpu_tune << std::endl;
            }
        }
    }

    std::cout << std::endl;

    // ── Warning banner if discharging ──
    // (removed — battery display removed per user request)

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
