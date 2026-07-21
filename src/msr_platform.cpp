// msr_platform.cpp — Cross-platform MSR address abstraction implementation
//
// Probes MSR availability at runtime and selects the correct address for
// each conceptual purpose based on the running CPU generation.

#include "msr_platform.h"
#include "power-utils.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <sstream>
#include <string>
#include <syslog.h>

// ═══════════════════════════════════════════════════════════
// CPU model ID definitions (from kernel intel-family.h)
// ═══════════════════════════════════════════════════════════

// Family 6 model IDs for Intel CPU generations we care about.
// These map to the MSR address patterns below.

static constexpr int FAM = 6;

// ── Generations where 0x690 (MSR_CORE_PERF_LIMIT_REASONS) is available ──
// Haswell, Broadwell, Skylake, Kaby Lake, Coffee Lake, Comet Lake, Ice Lake
static constexpr int CPU_HSW      = 0x3C;  // Haswell
static constexpr int CPU_HSW_X    = 0x3F;
static constexpr int CPU_HSW_L    = 0x45;
static constexpr int CPU_HSW_G    = 0x46;
static constexpr int CPU_BDW      = 0x3D;  // Broadwell
static constexpr int CPU_BDW_G    = 0x47;
static constexpr int CPU_BDW_X    = 0x4F;
static constexpr int CPU_BDW_D    = 0x56;
static constexpr int CPU_SKL      = 0x4E;  // Skylake
static constexpr int CPU_SKL_X    = 0x55;
static constexpr int CPU_SKL_U    = 0x5E;
static constexpr int CPU_KBL      = 0x8E;  // Kaby Lake (also 0x9E)
static constexpr int CPU_CFL      = 0x9E;  // Coffee Lake (also 0x8E)
static constexpr int CPU_CFL_U    = 0xA5;  // Coffee Lake U (also Comet Lake 0xA6)
static constexpr int CPU_CML      = 0xA6;
static constexpr int CPU_ICL      = 0x7D;  // Ice Lake (also 0x6A, 0x6C, 0x7E, 0x9D)
static constexpr int CPU_ICL_X    = 0x6A;
static constexpr int CPU_ICL_D    = 0x6C;
static constexpr int CPU_ICL_L    = 0x7E;
static constexpr int CPU_ICL_NNPI = 0x9D;

// ── Generations where 0x690 may NOT be available ──
// Alder Lake: 0x690 uncertain — probe both
static constexpr int CPU_ADL      = 0x97;  // Alder Lake (also 0x9A)
static constexpr int CPU_ADL_L    = 0x9A;

// Raptor Lake: 0x690 uncertain — probe both
static constexpr int CPU_RPL      = 0xB7;
static constexpr int CPU_RPL_P    = 0xBA;
static constexpr int CPU_RPL_S    = 0xBF;

// Meteor Lake: 0x690 NOT available — only 0x64F
static constexpr int CPU_MTL      = 0xAC;  // Meteor Lake (also 0xAA)
static constexpr int CPU_MTL_L    = 0xAA;

// Lunar Lake: 0x690 NOT available — only 0x64F
static constexpr int CPU_LNL_M    = 0xBD;  // Lunar Lake

// ═══════════════════════════════════════════════════════════
// Per-generation MSR tables
// ═══════════════════════════════════════════════════════════

// Haswell – Ice Lake: both 0x64F and 0x690 exist. Use 0x690 (canonical).
static const PlatformMSRs msr_legacy_perf_limit = {
    .cpu_perf_limit     = 0x690,  // MSR_CORE_PERF_LIMIT_REASONS
    .bd_prochot         = 0x1FC,  // MSR_POWER_CTL — stable
    .rapl_pkg_limit     = 0x610,  // MSR_PKG_POWER_LIMIT
    .gpu_perf_limit     = 0x6B0,  // MSR_GFX_PERF_LIMIT_REASONS
    .ring_perf_limit    = 0x6B1,  // MSR_RING_PERF_LIMIT_REASONS
    .pkg_thermal_status = 0x1B1,  // MSR_IA32_PACKAGE_THERM_STATUS
    .therm_control      = 0x19A,  // MSR_IA32_THERM_CONTROL
};

// Alder Lake / Raptor Lake: try 0x690 first, fall back to 0x64F.
// (Table is populated after probing in detect_platform_msrs().)
static const PlatformMSRs msr_adl_rpl = {
    .cpu_perf_limit     = 0x690,  // default — will be overwritten by probe
    .bd_prochot         = 0x1FC,
    .rapl_pkg_limit     = 0x610,
    .gpu_perf_limit     = 0x6B0,
    .ring_perf_limit    = 0x6B1,
    .pkg_thermal_status = 0x1B1,
    .therm_control      = 0x19A,
};

// Meteor Lake / Lunar Lake: only 0x64F exists for CPU perf-limit.
static const PlatformMSRs msr_mtl_lnl = {
    .cpu_perf_limit     = 0x64F,  // MSR_PERF_LIMIT_REASONS (0x690 removed)
    .bd_prochot         = 0x1FC,
    .rapl_pkg_limit     = 0x610,
    .gpu_perf_limit     = 0x6B0,
    .ring_perf_limit    = 0x6B1,
    .pkg_thermal_status = 0x1B1,
    .therm_control      = 0x19A,
};

// Expose externs for tests
const PlatformMSRs msr_haswell   = msr_legacy_perf_limit;
const PlatformMSRs msr_skylake   = msr_legacy_perf_limit;
const PlatformMSRs msr_adl       = msr_adl_rpl;
const PlatformMSRs msr_meteor_lake = msr_mtl_lnl;
const PlatformMSRs msr_lunar_lake = msr_mtl_lnl;

// ═══════════════════════════════════════════════════════════
// CPUID / model detection helpers
// ═══════════════════════════════════════════════════════════

struct CpuIdResult {
    uint32_t eax, ebx, ecx, edx;
};

static CpuIdResult cpuid(uint32_t leaf) {
    CpuIdResult res = {};
    __asm__ __volatile__("cpuid"
        : "=a"(res.eax), "=b"(res.ebx), "=c"(res.ecx), "=d"(res.edx)
        : "a"(leaf));
    return res;
}

static bool is_meteor_lake_family(uint32_t model) {
    return model == CPU_MTL || model == CPU_MTL_L;
}

static bool is_lunar_lake_family(uint32_t model) {
    return model == CPU_LNL_M;
}

static bool is_adl_rpl_family(uint32_t model) {
    return model == CPU_ADL || model == CPU_ADL_L ||
           model == CPU_RPL || model == CPU_RPL_P || model == CPU_RPL_S;
}

static bool is_legacy_perf_limit_family(uint32_t model) {
    // Haswell through Ice Lake — 0x690 is the canonical address
    return model == CPU_HSW || model == CPU_HSW_X || model == CPU_HSW_L ||
           model == CPU_HSW_G || model == CPU_BDW || model == CPU_BDW_G ||
           model == CPU_BDW_X || model == CPU_BDW_D || model == CPU_SKL ||
           model == CPU_SKL_X || model == CPU_SKL_U || model == CPU_KBL ||
           model == CPU_CFL || model == CPU_CFL_U || model == CPU_CML ||
           model == CPU_ICL || model == CPU_ICL_X || model == CPU_ICL_D ||
           model == CPU_ICL_L || model == CPU_ICL_NNPI;
}

// ═══════════════════════════════════════════════════════════
// MSR probing
// ═══════════════════════════════════════════════════════════

unsigned long long probe_msr_read(uint32_t msr_addr) {
    unsigned long long val = 0;
    std::string path = "/dev/cpu/0/msr";
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return (unsigned long long)-1;

    bool ok = false;
    if (lseek(fd, msr_addr, SEEK_SET) == (off_t)msr_addr) {
        if (read(fd, &val, sizeof(val)) == sizeof(val))
            ok = true;
    }
    close(fd);
    return ok ? val : (unsigned long long)-1;
}

bool probe_msr_write(uint32_t msr_addr) {
    unsigned long long val = probe_msr_read(msr_addr);
    if (val == (unsigned long long)-1) return false;

    // Try writing 0 (clear all) and restore
    std::string path = "/dev/cpu/0/msr";
    int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;

    bool ok = false;
    if (lseek(fd, msr_addr, SEEK_SET) == (off_t)msr_addr) {
        // Write 0 to clear sticky/log bits
        if (write(fd, &val, sizeof(val)) == sizeof(val))
            ok = true;
        // Restore original value
        if (ok) {
            unsigned long long restore = val;
            if (lseek(fd, msr_addr, SEEK_SET) == (off_t)msr_addr) {
                if (write(fd, &restore, sizeof(restore)) != sizeof(restore))
                    ok = false;
            }
        }
    }
    close(fd);
    return ok;
}

bool probe_msr_write_with_value(uint32_t msr_addr, unsigned long long val) {
    std::string path = "/dev/cpu/0/msr";
    int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;

    bool ok = false;
    if (lseek(fd, msr_addr, SEEK_SET) == (off_t)msr_addr) {
        ok = (write(fd, &val, sizeof(val)) == sizeof(val));
    }
    close(fd);
    return ok;
}

PlatformMSRs validate_msrs(const PlatformMSRs& candidate) {
    PlatformMSRs result = candidate;

    // Validate cpu_perf_limit — this is the critical one that varies.
    // If 0x690 fails, try 0x64F.
    if (result.cpu_perf_limit != 0) {
        unsigned long long val = probe_msr_read(result.cpu_perf_limit);
        if (val == (unsigned long long)-1) {
            // Fallback to 0x64F (MSR_PERF_LIMIT_REASONS)
            if (result.cpu_perf_limit != 0x64F) {
                result.cpu_perf_limit = 0x64F;
                val = probe_msr_read(0x64F);
                if (val == (unsigned long long)-1) {
                    result.cpu_perf_limit = 0;
                }
            } else {
                result.cpu_perf_limit = 0;
            }
        }
    }

    // Validate gpu_perf_limit (0x6B0) — may not exist on all platforms
    if (result.gpu_perf_limit != 0) {
        if (probe_msr_read(result.gpu_perf_limit) == (unsigned long long)-1) {
            result.gpu_perf_limit = 0;
        }
    }

    // Validate ring_perf_limit (0x6B1)
    if (result.ring_perf_limit != 0) {
        if (probe_msr_read(result.ring_perf_limit) == (unsigned long long)-1) {
            result.ring_perf_limit = 0;
        }
    }

    // Validate bd_prochot (0x1FC) — should always exist on Intel
    if (result.bd_prochot != 0) {
        if (probe_msr_read(result.bd_prochot) == (unsigned long long)-1) {
            result.bd_prochot = 0;
        }
    }

    // Validate rapl_pkg_limit (0x610)
    if (result.rapl_pkg_limit != 0) {
        if (probe_msr_read(result.rapl_pkg_limit) == (unsigned long long)-1) {
            result.rapl_pkg_limit = 0;
        }
    }

    // Validate pkg_thermal_status (0x1B1) — RO MSR
    if (result.pkg_thermal_status != 0) {
        if (probe_msr_read(result.pkg_thermal_status) == (unsigned long long)-1) {
            result.pkg_thermal_status = 0;
        }
    }

    // Validate therm_control (0x19A)
    if (result.therm_control != 0) {
        if (probe_msr_read(result.therm_control) == (unsigned long long)-1) {
            result.therm_control = 0;
        }
    }

    return result;
}

// ═══════════════════════════════════════════════════════════
// Platform detection
// ═══════════════════════════════════════════════════════════

PlatformMSRs detect_platform_msrs() {
    CpuIdResult cpu = cpuid(1);
    uint32_t eax = cpu.eax;

    // Kernel-compatible family/model extraction (arch/x86/lib/cpu.c)
    uint32_t family = (eax >> 8) & 0xF;
    if (family == 0xF) family += (eax >> 20) & 0xFF;

    uint32_t model = (eax >> 4) & 0xF;
    if (family >= 6) model += ((eax >> 16) & 0xF) << 4;

    const PlatformMSRs* table = nullptr;
    const char* gen_name = "unknown";

    if (family != FAM) {
        // Non-family-6 — use safe defaults
        syslog(LOG_WARNING, "MSR: non-family-6 CPU (family=%u model=%u) — using safe defaults", family, model);
        // Return empty — caller should handle
        return PlatformMSRs{};
    }

    if (is_meteor_lake_family(model) || is_lunar_lake_family(model)) {
        gen_name = is_meteor_lake_family(model) ? "Meteor Lake" : "Lunar Lake";
        table = &msr_mtl_lnl;
    } else if (is_adl_rpl_family(model)) {
        gen_name = "Alder/Raptor Lake";
        table = &msr_adl_rpl;
    } else if (is_legacy_perf_limit_family(model)) {
        gen_name = "Haswell-Skylake era";
        table = &msr_legacy_perf_limit;
    } else {
        // Unknown model — use legacy table as starting point, will probe
        gen_name = "unknown (probing)";
        table = &msr_legacy_perf_limit;
    }

    PlatformMSRs result = validate_msrs(*table);

    syslog(LOG_INFO, "MSR platform: family=%u model=%u (0x%02X) (%s) cpu_plr=0x%03X gpu_plr=0x%03X ring_plr=0x%03X "
           "bd_prochot=0x%03X rapl=0x%03X pkg_therm=0x%03X",
           family, model, model, gen_name,
           result.cpu_perf_limit, result.gpu_perf_limit, result.ring_perf_limit,
           result.bd_prochot, result.rapl_pkg_limit, result.pkg_thermal_status);

    return result;
}

// ═══════════════════════════════════════════════════════════
// Pretty-print
// ═══════════════════════════════════════════════════════════

std::string msrs_to_string(const PlatformMSRs& msrs) {
    std::ostringstream ss;
    ss << "cpu_plr=0x" << std::hex << (msrs.cpu_perf_limit ? msrs.cpu_perf_limit : 0)
       << " gpu_plr=0x" << (msrs.gpu_perf_limit ? msrs.gpu_perf_limit : 0)
       << " ring_plr=0x" << (msrs.ring_perf_limit ? msrs.ring_perf_limit : 0)
       << " bd_prochot=0x" << (msrs.bd_prochot ? msrs.bd_prochot : 0)
       << " rapl=0x" << (msrs.rapl_pkg_limit ? msrs.rapl_pkg_limit : 0)
       << " pkg_therm=0x" << (msrs.pkg_thermal_status ? msrs.pkg_thermal_status : 0);
    return ss.str();
}

// ═══════════════════════════════════════════════════════════
// Convenience wrappers
// ═══════════════════════════════════════════════════════════

// Cached platform MSRs (set once at startup)
static PlatformMSRs g_platform_msrs;
static bool g_msrs_initialized = false;

void init_platform_msrs() {
    if (!g_msrs_initialized) {
        g_platform_msrs = detect_platform_msrs();
        g_msrs_initialized = true;
    }
}

PlatformMSRs get_platform_msrs() {
    if (!g_msrs_initialized) init_platform_msrs();
    return g_platform_msrs;
}

unsigned long long platform_read_cpu_perf_limit(int cpu) {
    if (!g_msrs_initialized) init_platform_msrs();
    if (g_platform_msrs.cpu_perf_limit == 0) return 0;

    // Read per-CPU MSR
    char path[64];
    std::snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    unsigned long long val = 0;
    if (lseek(fd, g_platform_msrs.cpu_perf_limit, SEEK_SET) == (off_t)g_platform_msrs.cpu_perf_limit) {
        if (read(fd, &val, sizeof(val)) != sizeof(val)) val = 0;
    }
    close(fd);
    return val;
}

bool platform_clear_cpu_perf_limit(int cpu) {
    if (!g_msrs_initialized) init_platform_msrs();
    if (g_platform_msrs.cpu_perf_limit == 0) return false;

    char path[64];
    std::snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return false;

    bool ok = false;
    unsigned long long zero = 0;
    if (lseek(fd, g_platform_msrs.cpu_perf_limit, SEEK_SET) == (off_t)g_platform_msrs.cpu_perf_limit) {
        ok = (write(fd, &zero, sizeof(zero)) == sizeof(zero));
    }
    close(fd);
    return ok;
}

unsigned long long platform_read_gpu_perf_limit(int cpu) {
    if (!g_msrs_initialized) init_platform_msrs();
    if (g_platform_msrs.gpu_perf_limit == 0) return 0;

    char path[64];
    std::snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    unsigned long long val = 0;
    if (lseek(fd, g_platform_msrs.gpu_perf_limit, SEEK_SET) == (off_t)g_platform_msrs.gpu_perf_limit) {
        if (read(fd, &val, sizeof(val)) != sizeof(val)) val = 0;
    }
    close(fd);
    return val;
}

unsigned long long platform_read_ring_perf_limit(int cpu) {
    if (!g_msrs_initialized) init_platform_msrs();
    if (g_platform_msrs.ring_perf_limit == 0) return 0;

    char path[64];
    std::snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    unsigned long long val = 0;
    if (lseek(fd, g_platform_msrs.ring_perf_limit, SEEK_SET) == (off_t)g_platform_msrs.ring_perf_limit) {
        if (read(fd, &val, sizeof(val)) != sizeof(val)) val = 0;
    }
    close(fd);
    return val;
}

bool platform_clear_bd_prochot() {
    if (!g_msrs_initialized) init_platform_msrs();
    if (g_platform_msrs.bd_prochot == 0) return false;

    unsigned long long val = probe_msr_read(g_platform_msrs.bd_prochot);
    if (val == (unsigned long long)-1) return false;

    // Clear bit 0 (BD_PROCHOT)
    unsigned long long clear = val & ~1ULL;
    if (clear == val) return true;  // already cleared

    char path[] = "/dev/cpu/0/msr";
    int fd = open(path, O_WRONLY);
    if (fd < 0) return false;

    bool ok = false;
    if (lseek(fd, g_platform_msrs.bd_prochot, SEEK_SET) == (off_t)g_platform_msrs.bd_prochot) {
        ok = (write(fd, &clear, sizeof(clear)) == sizeof(clear));
    }
    close(fd);
    return ok;
}

unsigned long long platform_read_rapl_pkg_limit() {
    if (!g_msrs_initialized) init_platform_msrs();
    if (g_platform_msrs.rapl_pkg_limit == 0) return 0;
    return probe_msr_read(g_platform_msrs.rapl_pkg_limit);
}
