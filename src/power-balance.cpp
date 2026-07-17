// power-balance.cpp — CPU/GPU power balancer daemon
// Dynamically limits CPU (PP0) and GPU (uncore) power to stay within
// a fraction of the configured package PL1.
// Compile: g++ -std=c++17 -O2 power-balance.cpp -o power-balance

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <syslog.h>

// ── Configuration ──
static constexpr double DEFAULT_PACKAGE_TDP_W = 28.0;
static constexpr double DEFAULT_RATIO         = 0.80;
static constexpr int    INTERVAL_MS           = 500;
static constexpr double MARGIN_W              = 2.0;
static constexpr double MIN_CORE_W            = 5.0;
static constexpr double MIN_UNCORE_W          = 0.0;
static constexpr double SMOOTH_ALPHA          = 0.3;

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
    if (f.is_open()) {
        f << content;
    }
}

template<typename T>
static bool read_attr(const std::string& dir, const std::string& name, T& out) {
    std::string val = read_file(dir + "/" + name);
    if (val.empty()) return false;
    std::istringstream iss(val);
    return (bool)(iss >> out);
}

// ── RAPL paths ──
static const std::string PKG_DIR    = "/sys/class/powercap/intel-rapl/intel-rapl:0";
static const std::string CORE_DIR   = PKG_DIR + "/intel-rapl:0:0";
static const std::string UNCORE_DIR = PKG_DIR + "/intel-rapl:0:1";

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
    if (delta_uj < 0) {
        return -1;
    }
    auto delta_us = std::chrono::duration_cast<std::chrono::microseconds>(cur.time - prev.time).count();
    if (delta_us <= 0) return -1;
    return (double)delta_uj / delta_us;
}

static double read_package_pl1_w() {
    long long uw = 0;
    if (read_attr(PKG_DIR, "constraint_0_power_limit_uw", uw) && uw > 0)
        return uw / 1e6;
    return DEFAULT_PACKAGE_TDP_W;
}

static double round_to_125mw(double watts) {
    if (watts < 0) return 0;
    return std::round(watts / 0.125) * 0.125;
}

// ── State ──
static volatile sig_atomic_t g_running = 1;
static void handle_signal(int) { g_running = 0; }

int main(int argc, char** argv) {
    double tdp   = DEFAULT_PACKAGE_TDP_W;
    double ratio = DEFAULT_RATIO;
    bool tdp_explicit = false;

    for (int i = 1; i < argc; ++i) {
        if (i + 1 < argc && strcmp(argv[i], "--tdp") == 0) {
            tdp = std::stod(argv[++i]);
            if (tdp < 1) tdp = DEFAULT_PACKAGE_TDP_W;
            tdp_explicit = true;
        } else if (i + 1 < argc && strcmp(argv[i], "--ratio") == 0) {
            ratio = std::stod(argv[++i]);
            if (ratio <= 0.0 || ratio > 1.0) ratio = DEFAULT_RATIO;
        }
    }

    if (!tdp_explicit) {
        double sysfs_tdp = read_package_pl1_w();
        if (sysfs_tdp > 0) tdp = sysfs_tdp;
    }

    openlog("power-balance", LOG_PID | LOG_CONS, LOG_DAEMON);
    syslog(LOG_INFO, "starting — package PL1: %.1fW  ratio: %.2f  margin: %.1fW  interval: %dms",
           tdp, ratio, MARGIN_W, INTERVAL_MS);

    // Check RAPL paths exist
    if (read_file(PKG_DIR + "/name").empty() ||
        read_file(CORE_DIR + "/name").empty() ||
        read_file(UNCORE_DIR + "/name").empty()) {
        syslog(LOG_ERR, "RAPL domains not found — check /sys/class/powercap/intel-rapl/");
        return 1;
    }

    // Enable RAPL domains
    write_file(PKG_DIR + "/enabled",      "1");
    write_file(CORE_DIR + "/enabled",     "1");
    write_file(UNCORE_DIR + "/enabled",   "1");

    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGHUP,  handle_signal);

    Sample prev_pkg  = read_energy(PKG_DIR);
    Sample prev_core = read_energy(CORE_DIR);
    Sample prev_unc  = read_energy(UNCORE_DIR);

    double smoothed_gpu_w = 0;
    bool first = true;
    int iterations = 0;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(INTERVAL_MS));
        if (!g_running) break;
        iterations++;

        Sample cur_pkg  = read_energy(PKG_DIR);
        Sample cur_core = read_energy(CORE_DIR);
        Sample cur_unc  = read_energy(UNCORE_DIR);

        double pkg_w  = compute_power_w(prev_pkg, cur_pkg);
        double core_w = compute_power_w(prev_core, cur_core);
        double gpu_w  = compute_power_w(prev_unc, cur_unc);

        prev_pkg  = cur_pkg;
        prev_core = cur_core;
        prev_unc  = cur_unc;

        if (gpu_w < 0 || core_w < 0) continue;
        if (first) {
            smoothed_gpu_w = gpu_w;
            first = false;
        }

        // Exponential smoothing on GPU power
        smoothed_gpu_w = SMOOTH_ALPHA * gpu_w + (1.0 - SMOOTH_ALPHA) * smoothed_gpu_w;

        // Total budget for core + uncore = ratio × package PL1
        double total_budget = tdp * ratio;

        // Uncore gets its current draw plus margin, but not so much
        // that core would fall below its minimum.
        double uncore_limit = smoothed_gpu_w + MARGIN_W;
        uncore_limit = std::clamp(uncore_limit, MIN_UNCORE_W, total_budget - MIN_CORE_W);

        // Core gets the remainder
        double core_limit = total_budget - uncore_limit;
        core_limit = std::max(core_limit, MIN_CORE_W);

        // Round to nearest 0.125W (RAPL register granularity)
        double core_limit_r   = round_to_125mw(core_limit);
        double uncore_limit_r = round_to_125mw(uncore_limit);

        // Write both limits via sysfs (kernel expects microwatts)
        write_file(CORE_DIR + "/constraint_0_power_limit_uw",
                   std::to_string((long long)(core_limit_r * 1e6)));
        write_file(CORE_DIR + "/constraint_0_time_window_us", "976");
        write_file(UNCORE_DIR + "/constraint_0_power_limit_uw",
                   std::to_string((long long)(uncore_limit_r * 1e6)));
        write_file(UNCORE_DIR + "/constraint_0_time_window_us", "976");

        if (iterations % 20 == 0) {
            syslog(LOG_INFO, "pkg=%.1fW  core=%.1fW  gpu=%.1fW  gpu_sm=%.1fW  budget=%.1fW  core_lmt=%.3fW  uncore_lmt=%.3fW",
                   pkg_w, core_w, gpu_w, smoothed_gpu_w, total_budget, core_limit_r, uncore_limit_r);
        }
    }

    // Reset limits on exit
    write_file(CORE_DIR + "/constraint_0_power_limit_uw",   "0");
    write_file(UNCORE_DIR + "/constraint_0_power_limit_uw", "0");
    syslog(LOG_INFO, "stopped — PP0 and uncore limits reset to 0 (disabled)");
    closelog();
    return 0;
}
