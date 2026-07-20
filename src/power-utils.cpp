// power-utils.cpp — Shared sysfs and MSR utilities implementation
//
// See power-utils.h for API documentation.

#include "power-utils.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// ── Sysfs file I/O ──

std::string sysfs_read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string content;
    std::getline(f, content);
    // Trim whitespace
    content.erase(0, content.find_first_not_of(" \t\n\r"));
    if (!content.empty())
        content.erase(content.find_last_not_of(" \t\n\r") + 1);
    return content;
}

void sysfs_write_file(const std::string& path, const std::string& content) {
    sysfs_write_str(path, content + "\n");
}

void sysfs_write_str(const std::string& path, const std::string& val) {
    std::ofstream f(path);
    if (f.is_open()) f << val;
}

void sysfs_write_int(const std::string& path, long long val) {
    sysfs_write_str(path, std::to_string(val));
}

// ── MSR access ──

bool msr_available() {
    int fd = open("/dev/cpu/0/msr", O_RDONLY);
    if (fd < 0) return false;
    close(fd);
    return true;
}

unsigned long long read_msr(int cpu, unsigned int msr_addr) {
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

bool write_msr(int cpu, unsigned int msr_addr, unsigned long long val) {
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

// ── Throttle/Perf Limit Tables ──

const char* XE_THROTTLE_REASONS[] = {
    "pl1", "pl2", "pl4", "prochot", "thermal", "ratl",
    "vr_tdc", "vr_thermalert", nullptr
};

const char* XE_THROTTLE_FILES[] = {
    "reason_pl1", "reason_pl2", "reason_pl4",
    "reason_prochot", "reason_thermal", "reason_ratl",
    "reason_vr_tdc", "reason_vr_thermalert", nullptr
};

const PerfLimitReason PERF_LIMIT_REASONS[] = {
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
    {"PROCHOT_Deassert", 10},
    {"PL4/Peak",        11},
    {"PkgPwrLatch",     12},
    {"Clipping",        13},
    {nullptr, 0}
};

// Alternate formatting for power-status.cpp display
const PerfLimitBits PERF_LIMIT_REASONS_BITS[] = {
    {"PROCHOT",          0},
    {"Thermal",          1},
    {"Current (EDP)",    2},
    {"Power (PL1)",      3},
    {"Platform",         4},
    {"Autonomous",       5},
    {"VR Thermal",       6},
    {"HTC",              7},
    {"Core/Cache",       8},
    {"Amps",             9},
    {"PROCHOT Deassert", 10},
    {"PL4/Peak",        11},
    {"PkgPwr Latch",    12},
    {"Clipping",        13},
    {"", 0}
};

// ── RAPL domain scanning ──

void scan_rapl_domains(const std::string& base_path, std::vector<RaplReading>& domains) {
    DIR* dir = opendir(base_path.c_str());
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name[0] == '.') continue;
        // Only recurse into actual RAPL domain directories (have ":" in name)
        if (name.find(':') == std::string::npos) continue;

        std::string full = base_path + "/" + name;
        std::string rname = sysfs_read_file(full + "/name");
        if (rname.empty()) continue;

        RaplReading d;
        d.name = rname;
        d.base_path = full;
        sysfs_read_attr(full, "power_uw", d.power_uw);
        sysfs_read_attr(full, "energy_uj", d.energy_uj);
        sysfs_read_attr(full, "constraint_0_power_limit_uw", d.pl1_uw);
        sysfs_read_attr(full, "constraint_0_time_window_us", d.pl1_window_us);
        sysfs_read_attr(full, "constraint_0_max_power_uw", d.pl1_max_uw);
        sysfs_read_attr(full, "constraint_1_power_limit_uw", d.pl2_uw);
        sysfs_read_attr(full, "constraint_1_time_window_us", d.pl2_window_us);
        sysfs_read_attr(full, "constraint_2_power_limit_uw", d.pl4_uw);
        sysfs_read_attr(full, "constraint_2_time_window_us", d.pl4_window_us);

        domains.push_back(d);

        // Recurse into subdomains
        scan_rapl_domains(full, domains);
    }
    closedir(dir);
}

std::vector<RaplReading> read_all_rapl_domains() {
    std::vector<RaplReading> domains;
    scan_rapl_domains("/sys/class/powercap/intel-rapl", domains);
    scan_rapl_domains("/sys/class/powercap/intel-rapl-mmio", domains);
    return domains;
}
