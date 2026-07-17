// power-balance.cpp — CPU/GPU power balancer daemon
// Dynamically limits CPU (PP0) power to stay within package TDP
// when GPU (uncore) power draw is high.
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
static constexpr double PACKAGE_TDP_W = 28.0;
static constexpr int    INTERVAL_MS  = 500;
static constexpr double MARGIN_W     = 2.0;
static constexpr double MIN_CPU_W    = 5.0;
static constexpr double SMOOTH_ALPHA = 0.3;

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
static const std::string PKG_DIR  = "/sys/class/powercap/intel-rapl/intel-rapl:0";
static const std::string CORE_DIR = PKG_DIR + "/intel-rapl:0:0";
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
        // energy counter wrapped — skip this sample
        return -1;
    }
    auto delta_us = std::chrono::duration_cast<std::chrono::microseconds>(cur.time - prev.time).count();
    if (delta_us <= 0) return -1;
    return (double)delta_uj / delta_us;
}

// ── State ──
static volatile sig_atomic_t g_running = 1;
static void handle_signal(int) { g_running = 0; }

int main(int argc, char** argv) {
    // Parse optional --tdp argument
    double tdp = PACKAGE_TDP_W;
    if (argc >= 3 && strcmp(argv[1], "--tdp") == 0) {
        tdp = std::stod(argv[2]);
        if (tdp < 1) tdp = PACKAGE_TDP_W;
    }

    openlog("power-balance", LOG_PID | LOG_CONS, LOG_DAEMON);
    syslog(LOG_INFO, "starting — package TDP: %.1fW, margin: %.1fW, interval: %dms",
           tdp, MARGIN_W, INTERVAL_MS);

    // Check RAPL paths exist
    if (read_file(PKG_DIR + "/name").empty() ||
        read_file(CORE_DIR + "/name").empty() ||
        read_file(UNCORE_DIR + "/name").empty()) {
        syslog(LOG_ERR, "RAPL domains not found — check /sys/class/powercap/intel-rapl/");
        return 1;
    }

    // Enable RAPL domains
    write_file(PKG_DIR + "/enabled", "1");
    write_file(CORE_DIR + "/enabled", "1");

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

        // Budget for CPU = TDP - GPU - margin
        double cpu_budget = tdp - smoothed_gpu_w - MARGIN_W;
        cpu_budget = std::max(cpu_budget, MIN_CPU_W);
        cpu_budget = std::min(cpu_budget, tdp);

        // Round to nearest 0.125W (RAPL register granularity)
        long long pp0_uw = (long long)std::round(cpu_budget * 1e6 / 125000.0) * 125000;
        if (pp0_uw < 0) pp0_uw = 0;

        // Write PP0 limit via sysfs
        write_file(CORE_DIR + "/constraint_0_power_limit_uw", std::to_string(pp0_uw));
        write_file(CORE_DIR + "/constraint_0_time_window_us", "976");

        if (iterations % 20 == 0) {
            syslog(LOG_INFO, "pkg=%.1fW  core=%.1fW  gpu=%.1fW  gpu_sm=%.1fW  cpu_budget=%.1fW  pp0=%llduW",
                   pkg_w, core_w, gpu_w, smoothed_gpu_w, cpu_budget, pp0_uw);
        }
    }

    // Reset PP0 limit on exit
    write_file(CORE_DIR + "/constraint_0_power_limit_uw", "0");
    syslog(LOG_INFO, "stopped — PP0 limit reset to 0 (disabled)");
    closelog();
    return 0;
}
