#pragma once

#include "nano/types.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace nano {

// A simple latency sample collector. It stores every sample so it can report
// exact percentiles and export the raw distribution for histogram plotting.
// Storing all samples costs memory but keeps the percentiles honest. For a
// bounded footprint a production system would use an HdrHistogram instead, which
// we note in the design writeup.
class LatencyStats {
public:
    void reserve(std::size_t n) { samples_.reserve(n); }

    NANO_ALWAYS_INLINE void record(uint64_t latency_ns) { samples_.push_back(latency_ns); }

    [[nodiscard]] uint64_t count() const noexcept {
        return static_cast<uint64_t>(samples_.size());
    }

    [[nodiscard]] uint64_t mean() const {
        if (samples_.empty()) return 0;
        long double sum = 0;
        for (uint64_t s : samples_) sum += static_cast<long double>(s);
        return static_cast<uint64_t>(sum / static_cast<long double>(samples_.size()));
    }

    [[nodiscard]] uint64_t percentile(double p) const {
        if (samples_.empty()) return 0;
        std::vector<uint64_t> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());
        double rank = p * static_cast<double>(sorted.size() - 1);
        std::size_t idx = static_cast<std::size_t>(rank + 0.5);
        if (idx >= sorted.size()) idx = sorted.size() - 1;
        return sorted[idx];
    }

    [[nodiscard]] uint64_t p50()  const { return percentile(0.50); }
    [[nodiscard]] uint64_t p99()  const { return percentile(0.99); }
    [[nodiscard]] uint64_t p999() const { return percentile(0.999); }
    [[nodiscard]] uint64_t median() const { return p50(); }

    [[nodiscard]] uint64_t min() const {
        if (samples_.empty()) return 0;
        return *std::min_element(samples_.begin(), samples_.end());
    }

    [[nodiscard]] uint64_t max() const {
        if (samples_.empty()) return 0;
        return *std::max_element(samples_.begin(), samples_.end());
    }

    [[nodiscard]] uint64_t stddev() const {
        if (samples_.size() < 2) return 0;
        long double m = static_cast<long double>(mean());
        long double acc = 0;
        for (uint64_t s : samples_) {
            long double d = static_cast<long double>(s) - m;
            acc += d * d;
        }
        return static_cast<uint64_t>(std::sqrt(acc / static_cast<long double>(samples_.size())));
    }

    [[nodiscard]] const std::vector<uint64_t>& samples() const noexcept { return samples_; }

    void clear() { samples_.clear(); }

    // Print one formatted row. The header is printed once by print_header.
    void print(const std::string& label) const {
        std::printf("%-18s %10llu %10llu %10llu %10llu %10llu %10llu\n",
                    label.c_str(),
                    static_cast<unsigned long long>(count()),
                    static_cast<unsigned long long>(mean()),
                    static_cast<unsigned long long>(p50()),
                    static_cast<unsigned long long>(p99()),
                    static_cast<unsigned long long>(p999()),
                    static_cast<unsigned long long>(stddev()));
    }

    static void print_header() {
        std::printf("%-18s %10s %10s %10s %10s %10s %10s\n",
                    "Operation", "Count", "Mean(ns)", "p50(ns)", "p99(ns)",
                    "p999(ns)", "Stddev");
        std::printf("--------------------------------------------------------"
                    "-------------------------------------\n");
    }

private:
    std::vector<uint64_t> samples_;
};

} // namespace nano
