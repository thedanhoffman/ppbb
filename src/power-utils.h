// power-utils.h — Shared sysfs and MSR utilities for power-balance and power-status
//
// Extracted from duplicated code in power-balance.cpp and power-status.cpp.
// Both tools read the same sysfs files and MSR registers — this header
// provides a single source of truth for these operations.
//
// Files in power-balance.cpp that used these:
//   read_file(), write_file(), read_attr(), write_str(), write_int()
//   msr_available(), read_msr(), write_msr()
//   THROTTLE_REASONS, THROTTLE_FILES (GPU throttle sysfs)
//   PERF_LIMIT_REASONS (CPU MSR perf limit reasons)
//
// Files in power-status.cpp that used these:
//   Same set of read_file/write_file/etc. utilities
//   msr_available(), read_msr(), write_msr()
//   PERF_LIMIT_REASONS (different struct format but same data)
//   XE_THROTTLE_REASONS, XE_THROTTLE_FILES (GPU throttle sysfs)

#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstdint>

// ── Sysfs file I/O ──

// Read a single line from a sysfs file, trimming whitespace.
// Returns empty string on failure (no error reporting — silent fail by design).
std::string sysfs_read_file(const std::string& path);

// Write content to a sysfs file (trailing newline added if absent).
void sysfs_write_file(const std::string& path, const std::string& content);

// Read a typed attribute from a sysfs directory.
// Returns false if the file is empty or the value doesn't parse.
template<typename T>
bool sysfs_read_attr(const std::string& dir, const std::string& name, T& out) {
    std::string val = sysfs_read_file(dir + "/" + name);
    if (val.empty()) return false;
    std::istringstream iss(val);
    return (bool)(iss >> out);
}

// Write a string value to a sysfs path.
void sysfs_write_str(const std::string& path, const std::string& val);

// Write an integer value to a sysfs path.
void sysfs_write_int(const std::string& path, long long val);

// ── MSR access ──

// Check if /dev/cpu/N/msr is available.
bool msr_available();

// Read a model-specific register from a given CPU.
unsigned long long read_msr(int cpu, unsigned int msr_addr);

// Write a value to a model-specific register on a given CPU.
// Returns false if the write fails (permissions, etc.).
bool write_msr(int cpu, unsigned int msr_addr, unsigned long long val);

// ── Throttle/Perf Limit Tables ──

// GPU throttle reasons (Xe driver sysfs: reason_pl1, reason_pl2, etc.)
extern const char* XE_THROTTLE_REASONS[];
extern const char* XE_THROTTLE_FILES[];

// CPU perf limit reasons (MSR 0x690 — MSR_CORE_PERF_LIMIT_REASONS bit definitions)
struct PerfLimitReason {
    const char* name;
    unsigned int bit;
};
typedef PerfLimitReason PerfLimitBits;

extern const PerfLimitReason PERF_LIMIT_REASONS[];
extern const PerfLimitBits PERF_LIMIT_REASONS_BITS[];

// ── RAPL domain scanning ──

// RAPL power reading (for monitoring/status display).
// Different from power-balance.cpp's RaplDomain which tracks sysfs paths.
struct RaplReading {
    std::string name;          // "package-0", "core", "uncore", etc.
    std::string base_path;     // /sys/class/powercap/... path
    long long power_uw = -1;   // current power limit (uW)
    long long energy_uj = -1;  // energy counter (uJ)
    long long pl1_uw = -1;     // constraint_0_power_limit_uw
    long long pl1_window_us = -1;
    long long pl1_max_uw = -1; // constraint_0_max_power_uw
    long long pl2_uw = -1;     // constraint_1_power_limit_uw
    long long pl2_window_us = -1;
    long long pl4_uw = -1;     // constraint_2_power_limit_uw
    long long pl4_window_us = -1;
};

// Scan RAPL domains from a base powercap path (e.g. intel-rapl).
// Recurses into subdomains. Populates domains vector.
void scan_rapl_domains(const std::string& base_path, std::vector<RaplReading>& domains);

// Convenience: scan both intel-rapl and intel-rapl-mmio.
std::vector<RaplReading> read_all_rapl_domains();
