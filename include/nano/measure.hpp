#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#endif

// Saying what the numbers were measured on, and pinning where the platform
// allows it.
//
// This file exists because of a specific criticism. The engine's latency table
// was measured honestly and reported without saying which core it ran on,
// whether that core was isolated, or what else the machine was doing. A median
// taken that way is still a median, but a p99 taken that way is mostly a
// measurement of the operating system's scheduler, and a reader who works on
// latency for a living reads an unlabelled p99 as a measurement that has not
// been done properly yet.
//
// So every run now prints the machine, whether pinning was actually achieved
// rather than merely requested, and the load average at the time. None of that
// makes a number better. It makes a number checkable, which is the part that
// was missing.

namespace nano {

struct PinningReport {
    bool        requested = false;
    bool        achieved  = false;
    int         core      = -1;
    const char* reason    = "not requested";
};

// Pin the calling thread to one core.
//
// Linux can do this and macOS cannot. Not weakly, not approximately. macOS
// offers THREAD_AFFINITY_POLICY, which is a hint that threads sharing a tag
// would like to share an L2 cache, and on Apple Silicon it returns
// KERN_NOT_SUPPORTED outright. There is no way to bind a thread to a core, and
// there is no way to stop the scheduler moving it between a performance core
// and an efficiency core, which changes the clock rate underneath a
// measurement. So on macOS this returns false and the results say so.
[[nodiscard]] inline PinningReport pin_to_core(int core) noexcept {
    PinningReport r;
    r.requested = true;
    r.core      = core;

#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(core), &set);
    if (sched_setaffinity(0, sizeof(set), &set) == 0) {
        r.achieved = true;
        r.reason   = "sched_setaffinity";
    } else {
        r.reason = "sched_setaffinity failed";
    }
#else
    r.reason = "this platform cannot pin a thread to a core";
#endif
    return r;
}

[[nodiscard]] inline constexpr bool pinning_supported() noexcept {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

[[nodiscard]] inline int current_core() noexcept {
#if defined(__linux__)
    return sched_getcpu();
#else
    // Deliberately not zero, so that unknown can never be mistaken for core
    // zero in a results table.
    return -1;
#endif
}

struct BoxInfo {
    std::string cpu_model = "unknown";
    std::string os        = "unknown";
    std::string compiler  = "unknown";
    std::string build     = "unknown";
    int         cores     = 0;
    bool        pinned    = false;
    bool        isolated  = false;
    double      load_1min = -1.0;

    // Is the machine quiet enough for a timing to mean anything. One runnable
    // thread per two cores is already generous, and the check exists because a
    // benchmark taken at a load average of fifty on twelve threads is not a
    // benchmark. Recording it makes that checkable afterwards rather than
    // remembered.
    [[nodiscard]] bool quiet() const noexcept {
        return load_1min >= 0.0 && cores > 0 &&
               load_1min < static_cast<double>(cores) * 0.5;
    }

    [[nodiscard]] std::string one_line() const {
        std::string s = cpu_model;
        s += ", ";
        s += os;
        s += ", ";
        s += compiler;
        s += ", ";
        s += build;
        s += ", ";
        s += (pinned ? "pinned" : "NOT pinned");
        if (isolated) s += ", isolated core";
        if (load_1min >= 0.0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), ", load %.2f", load_1min);
            s += buf;
            if (!quiet()) s += " (LOADED, the tail here is not trustworthy)";
        }
        return s;
    }
};

namespace measure_detail {

inline std::string compiler_string() {
#if defined(__apple_build_version__)
    return "Apple clang " + std::to_string(__clang_major__) + "." +
           std::to_string(__clang_minor__);
#elif defined(__clang__)
    return "clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(__GNUC__)
    return "gcc " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#else
    return "unknown compiler";
#endif
}

inline std::string build_string() {
#if defined(NDEBUG)
    return "NDEBUG";
#else
    return "assertions on, this is not a release build";
#endif
}

// PRETTY_NAME="Ubuntu 24.04.5 LTS" out of /etc/os-release. Parsed rather than
// sourced, so whatever is in that file is never executed.
inline bool read_os_release_pretty_name(std::string& out) {
    std::FILE* f = std::fopen("/etc/os-release", "r");
    if (f == nullptr) return false;
    char line[512];
    bool found = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, "PRETTY_NAME=", 12) != 0) continue;
        out = line + 12;
        while (!out.empty() && (out.back() == '\n' || out.back() == '"')) out.pop_back();
        if (!out.empty() && out.front() == '"') out.erase(0, 1);
        found = true;
        break;
    }
    std::fclose(f);
    return found;
}

inline bool read_first_line(const char* path, const char* key, std::string& out) {
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) return false;
    char line[512];
    bool found = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (key == nullptr) {
            out   = line;
            found = true;
            break;
        }
        if (std::strncmp(line, key, std::strlen(key)) == 0) {
            const char* colon = std::strchr(line, ':');
            if (colon != nullptr) {
                out = colon + 1;
                while (!out.empty() && (out.front() == ' ' || out.front() == '\t')) out.erase(0, 1);
                while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
                found = true;
            }
            break;
        }
    }
    std::fclose(f);
    return found;
}

} // namespace measure_detail

[[nodiscard]] inline BoxInfo detect_box() {
    BoxInfo b;
    b.compiler = measure_detail::compiler_string();
    b.build    = measure_detail::build_string();
    b.cores    = static_cast<int>(std::thread::hardware_concurrency());

    double la[3] = {-1.0, -1.0, -1.0};
    if (::getloadavg(la, 3) == 3) b.load_1min = la[0];

#if defined(__APPLE__)
    char   buf[256];
    size_t n = sizeof(buf);
    if (sysctlbyname("machdep.cpu.brand_string", buf, &n, nullptr, 0) == 0) b.cpu_model = buf;
    n = sizeof(buf);
    if (sysctlbyname("kern.osproductversion", buf, &n, nullptr, 0) == 0) {
        b.os = std::string("macOS ") + buf;
    }
#elif defined(__linux__)
    std::string s;
    if (measure_detail::read_first_line("/proc/cpuinfo", "model name", s)) {
        b.cpu_model = s;
    } else if (measure_detail::read_first_line("/sys/firmware/devicetree/base/model", nullptr, s)) {
        // arm64 kernels commonly publish no model name in /proc/cpuinfo, only
        // implementer and part numbers. The device tree usually has something
        // readable, and where it does not the field stays unknown rather than
        // being assembled out of hex ids nobody can check.
        b.cpu_model = s;
    }
    // os-release is key=value with the value quoted, which is a different shape
    // from /proc/cpuinfo's key: value, so it gets its own read.
    if (measure_detail::read_os_release_pretty_name(s)) b.os = s;
    // isolcpus on the kernel command line is what makes a pinned core a quiet
    // one. Without it the core is pinned and still shared.
    if (measure_detail::read_first_line("/proc/cmdline", nullptr, s)) {
        b.isolated = s.find("isolcpus=") != std::string::npos;
    }
#endif
    return b;
}

} // namespace nano
