// power-utils.h — Shared sysfs utilities, RAPL types, and sysfs paths
//
// Provides: sysfs I/O with error reporting, shared sysfs path constants,
//           unified RaplDomain type, RAPL domain scanning, temperature discovery.
//
// MSR access is in msr_platform.h — it provides a cross-platform abstraction
// that selects the correct MSR addresses per CPU generation.

#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstdint>

// ═══════════════════════════════════════════════════════════
// Shared sysfs path constants (const char* for opendir() compatibility)
// ═══════════════════════════════════════════════════════════

// Power management
constexpr const char* SYSFS_PSTATE_DIR    = "/sys/devices/system/cpu/intel_pstate";
constexpr const char* SYSFS_PSTATE_MAX    = "/sys/devices/system/cpu/intel_pstate/max_perf_pct";
constexpr const char* SYSFS_PSTATE_MIN    = "/sys/devices/system/cpu/intel_pstate/min_perf_pct";
constexpr const char* SYSFS_PSTATE_NOTURBO = "/sys/devices/system/cpu/intel_pstate/no_turbo";

// RAPL domains
constexpr const char* SYSFS_POWERCAP_BASE     = "/sys/class/powercap";
constexpr const char* SYSFS_RAPL              = "/sys/class/powercap/intel-rapl";
constexpr const char* SYSFS_RAPL_MMIO         = "/sys/class/powercap/intel-rapl-mmio";

// CPU topology / hotplug
constexpr const char* SYSFS_CPU_BASE          = "/sys/devices/system/cpu";

// Thermal
constexpr const char* SYSFS_THERMAL           = "/sys/class/thermal";
constexpr const char* SYSFS_HWMON             = "/sys/class/hwmon";

// DRM / GPU
constexpr const char* SYSFS_DRM               = "/sys/class/drm";

// Power supply
constexpr const char* SYSFS_POWER_SUPPLY      = "/sys/class/power_supply";

// ═══════════════════════════════════════════════════════════
// Sysfs file I/O
// ═══════════════════════════════════════════════════════════

// Read a single line from a sysfs file, trimming whitespace.
// Returns empty string on failure.
std::string sysfs_read_file(const std::string& path);

// Write content to a sysfs file (trailing newline added if absent).
// Returns false if the write fails.
bool sysfs_write_file(const std::string& path, const std::string& content);

// Read a typed attribute from a sysfs directory.
// Returns false if the file is empty or the value doesn't parse.
template<typename T>
bool sysfs_read_attr(const std::string& dir, const std::string& name, T& out) {
    std::string val = sysfs_read_file(dir + "/" + name);
    if (val.empty()) return false;
    std::istringstream iss(val);
    return (bool)(iss >> out);
}

// Write a string value to a sysfs path. Returns false on failure.
bool sysfs_write_str(const std::string& path, const std::string& val);

// Write an integer value to a sysfs path. Returns false on failure.
bool sysfs_write_int(const std::string& path, long long val);

// Write an integer only if the current value differs.
// Avoids unnecessary sysfs churn and kernel context switches.
// Returns true if the value was written (changed) or was already correct.
bool sysfs_write_int_if_changed(const std::string& path, long long val);

// ═══════════════════════════════════════════════════════════
// MSR access
// ═══════════════════════════════════════════════════════════

// Check if /dev/cpu/N/msr is available (used for gating MSR operations).
// All MSR read/write is via msr_platform.h for cross-platform correctness.
bool msr_available();

// ═══════════════════════════════════════════════════════════
// Throttle/Perf Limit Tables
// ═══════════════════════════════════════════════════════════

// GPU throttle reasons (Xe driver sysfs: reason_pl1, reason_pl2, etc.)
extern const char* XE_THROTTLE_REASONS[];
extern const char* XE_THROTTLE_FILES[];

// CPU perf limit reasons (MSR bit definitions — platform address via msr_platform.h)
struct PerfLimitReason {
    const char* name;
    unsigned int bit;
};
typedef PerfLimitReason PerfLimitBits;

extern const PerfLimitReason PERF_LIMIT_REASONS[];
extern const PerfLimitBits PERF_LIMIT_REASONS_BITS[];

// ═══════════════════════════════════════════════════════════
// RAPL domain (unified type used by both binaries)
// ═══════════════════════════════════════════════════════════

// Unified RAPL domain — used by power-balance (control) and power-status (display).
// All raw values in microunits (uW, uJ, us) as returned by sysfs.
// Convenience accessors for watts are provided inline.
struct RaplDomain {
    std::string path;              // /sys/class/powercap/intel-rapl/domain-N
    bool        valid = false;
    std::string name;              // "package-0", "core", "uncore", "dram", "pp0", etc.
    long long   energy_uj = 0;     // energy_uj (cumulative counter)
    long long   power_uw = 0;      // instantaneous power (uW, if available)
    long long   pl1_uw = 0;        // constraint_0_power_limit_uw
    long long   pl1_window_us = 0; // constraint_0_time_window_us
    long long   pl1_max_uw = 0;    // constraint_0_max_power_uw
    long long   pl2_uw = 0;        // constraint_1_power_limit_uw
    long long   pl2_window_us = 0; // constraint_1_time_window_us
    long long   pl4_uw = 0;        // constraint_2_power_limit_uw
    long long   pl4_window_us = 0; // constraint_2_time_window_us

    // Convenience: limits in watts (double)
    double pl4_w() const { return pl4_uw > 0 ? pl4_uw / 1e6 : 0.0; }
    double max_w() const { return pl1_max_uw > 0 ? pl1_max_uw / 1e6 : 0.0; }
    double pl1_w() const { return pl1_uw > 0 ? pl1_uw / 1e6 : 0.0; }
    double pl2_w() const { return pl2_uw > 0 ? pl2_uw / 1e6 : 0.0; }
};

// Scan RAPL domains from a base powercap path (e.g. intel-rapl).
// Recurses into subdomains. Populates domains vector.
void scan_rapl_domains(const std::string& base_path, std::vector<RaplDomain>& domains);

// Convenience: scan both intel-rapl and intel-rapl-mmio.
std::vector<RaplDomain> read_all_rapl_domains();

// ═══════════════════════════════════════════════════════════
// Temperature discovery (shared between binaries)
// ═══════════════════════════════════════════════════════════

// Scan thermal zones and hwmon for a temperature sensor matching the given
// type name (e.g. "x86_pkg_temp", "coretemp", "i915", "xe").
// Returns temperature in Celsius, or -1 if not found.
int find_temperature(const std::string& sensor_type);

// Read the highest temperature from all matching thermal zone types.
// Types checked: "x86_pkg_temp", "SOC DTS", "CPU".
int read_cpu_pkg_temp();

// Read GPU temperature from hwmon (i915/amdgpu/xe) or thermal fallback.
int read_gpu_temp();

// Read NVMe temperature from hwmon.
int read_nvme_temp();
