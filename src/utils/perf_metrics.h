#pragma once

// Performance instrumentation scaffolding for the refactor work.
//
// All probes are compiled away completely unless the project is built with
// -DAI_TOOLS_DEBUG=ON (CMake option). Release/test-of-performance builds
// (the default) pay zero cost: the PERF_* macros expand to nothing.
//
// Usage:
//   PERF_SCOPE("name")            RAII timer: counts calls, total/avg/min/max time
//   PERF_COUNTER("name")          counts occurrences of an event
//   PERF_VALUE("name", v)         accumulates a numeric quantity (bytes, rows...)
//   PERF_LOG("fmt %s", x)         one-shot line on stderr
//   PERF_DUMP()                   print the aggregated table now (also printed
//                                 automatically at process exit)
//
// The aggregated table is printed to stderr at exit, sorted by total time,
// so a debug run (e.g. `ai-tools --register .`) doubles as a baseline profile.

#ifdef AI_TOOLS_DEBUG

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace perf_metrics {

struct Stat {
    uint64_t count = 0;
    uint64_t total_us = 0;
    uint64_t min_us = UINT64_MAX;
    uint64_t max_us = 0;
    uint64_t value = 0; // accumulated quantity for PERF_VALUE
};

inline std::unordered_map<std::string, Stat>& stats_map() {
    static std::unordered_map<std::string, Stat> m;
    return m;
}

inline std::mutex& stats_mutex() {
    static std::mutex m;
    return m;
}

inline void record_time(const std::string& name, uint64_t us) {
    std::lock_guard<std::mutex> lock(stats_mutex());
    Stat& s = stats_map()[name];
    s.count++;
    s.total_us += us;
    if (us < s.min_us) s.min_us = us;
    if (us > s.max_us) s.max_us = us;
}

inline void record_count(const std::string& name) {
    std::lock_guard<std::mutex> lock(stats_mutex());
    stats_map()[name].count++;
}

inline void record_value(const std::string& name, uint64_t v) {
    std::lock_guard<std::mutex> lock(stats_mutex());
    Stat& s = stats_map()[name];
    s.count++;
    s.value += v;
}

inline void dump(std::FILE* out = stderr) {
    std::lock_guard<std::mutex> lock(stats_mutex());
    auto& m = stats_map();
    if (m.empty()) return;

    std::vector<std::pair<std::string, Stat>> rows(m.begin(), m.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.second.total_us > b.second.total_us;
    });

    std::fprintf(out, "\n=== perf_metrics (AI_TOOLS_DEBUG) ===\n");
    std::fprintf(out, "%-44s %8s %12s %10s %10s %10s %14s\n",
                 "probe", "calls", "total_ms", "avg_us", "min_us", "max_us", "sum");
    for (const auto& [name, s] : rows) {
        std::fprintf(out, "%-44s %8llu %12.3f %10.1f %10llu %10llu %14llu\n",
                     name.c_str(),
                     static_cast<unsigned long long>(s.count),
                     static_cast<double>(s.total_us) / 1000.0,
                     s.count ? static_cast<double>(s.total_us) / s.count : 0.0,
                     static_cast<unsigned long long>(s.min_us == UINT64_MAX ? 0 : s.min_us),
                     static_cast<unsigned long long>(s.max_us),
                     static_cast<unsigned long long>(s.value));
    }
    std::fprintf(out, "=== end perf_metrics ===\n");
    std::fflush(out);
}

// Print the table once at process exit (registered on first probe use).
inline void ensure_atexit_dump() {
    static bool registered = [] {
        stats_map(); // construct the map before the atexit handler runs
        std::atexit([] { dump(); });
        return true;
    }();
    (void)registered;
}

class Scope {
public:
    explicit Scope(std::string name)
        : name_(std::move(name)),
          start_(std::chrono::steady_clock::now()) {
        ensure_atexit_dump();
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() {
        auto us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start_)
                .count());
        record_time(name_, us);
    }

private:
    std::string name_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace perf_metrics

#define PERF_CONCAT_(a, b) a##b
#define PERF_CONCAT(a, b) PERF_CONCAT_(a, b)

#define PERF_SCOPE(name) ::perf_metrics::Scope PERF_CONCAT(perf_scope_, __LINE__)(name)
#define PERF_COUNTER(name) ::perf_metrics::record_count(name)
#define PERF_VALUE(name, v) ::perf_metrics::record_value(name, static_cast<uint64_t>(v))
#define PERF_LOG(...)                          \
    do {                                       \
        std::fprintf(stderr, "[perf] " __VA_ARGS__); \
        std::fflush(stderr);                   \
    } while (0)
#define PERF_DUMP() ::perf_metrics::dump()

#else // Release build: probes compile away to nothing — zero overhead.

#define PERF_SCOPE(name)
#define PERF_COUNTER(name)
#define PERF_VALUE(name, v)
#define PERF_LOG(...)
#define PERF_DUMP()

#endif
