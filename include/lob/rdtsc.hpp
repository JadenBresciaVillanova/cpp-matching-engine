#pragma once

// rdtsc.hpp
// -----------------------------------------------------------------------------
// Cycle-accurate timing for the hot path.
//
// WHY NOT std::chrono::high_resolution_clock?
//   - On several standard library implementations it is an alias for
//     system_clock, which is NOT monotonic (it can jump when the wall clock is
//     adjusted) and is too coarse/expensive for measuring sub-microsecond
//     operations.
//   - std::chrono::steady_clock is the correct *portable* choice and we use it
//     for coarse timing. But for measuring an operation that takes tens of
//     nanoseconds, even steady_clock's call overhead is a meaningful fraction
//     of what we are trying to measure.
//
// WHAT WE USE INSTEAD:
//   - rdtscp reads the CPU's Time Stamp Counter (a cycle counter) and, unlike
//     plain rdtsc, it (a) does not execute speculatively before prior
//     instructions retire and (b) returns the core id in IA32_TSC_AUX. The core
//     id lets us DETECT if the thread migrated cores mid-measurement, in which
//     case the sample is invalid and must be discarded. This detection is a
//     detail that signals you understand why naive rdtsc benchmarks lie.
//
// IMPORTANT CAVEATS (state these in interviews):
//   - The TSC is only a reliable wall-clock proxy on CPUs with "invariant TSC"
//     (constant_tsc + nonstop_tsc). On modern x86-64 this is essentially
//     universal, but we verify it rather than assume.
//   - The TSC counts at a fixed reference frequency that is NOT the same as the
//     core's current (turbo/throttled) clock. So cycle counts are stable, but
//     converting cycles -> nanoseconds requires calibrating the TSC frequency
//     against a known clock once at startup.
//
// This header is x86-64 specific. On other architectures we fall back to
// steady_clock (see read_tsc()).
// -----------------------------------------------------------------------------

#include <cstdint>
#include <chrono>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
  #define LOB_HAS_RDTSC 1
  #if defined(_MSC_VER)
    #include <intrin.h>
  #else
    #include <x86intrin.h>
  #endif
#else
  #define LOB_HAS_RDTSC 0
#endif

namespace lob {

// Reads the TSC with a serializing rdtscp, writing the executing core id into
// `cpu`. Caller compares the cpu before/after to detect migration.
inline std::uint64_t read_tsc(unsigned& cpu) noexcept {
#if LOB_HAS_RDTSC
    return __rdtscp(&cpu);
#else
    cpu = 0;
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// One-time calibration: measures how many TSC ticks elapse over a known
// steady_clock interval, yielding ticks-per-nanosecond. Done once at startup;
// the result is used to convert measured cycle deltas to nanoseconds for
// reporting. We sleep rather than busy-wait to keep this cheap; the interval is
// long enough (here ~50ms) that scheduler jitter is negligible relative to it.
struct TscCalibration {
    double ticks_per_ns = 1.0;

    static TscCalibration measure(std::chrono::milliseconds window =
                                      std::chrono::milliseconds(50)) noexcept {
        unsigned c0 = 0, c1 = 0;
        const auto wall_start = std::chrono::steady_clock::now();
        const std::uint64_t tsc_start = read_tsc(c0);

        std::this_thread::sleep_for(window);

        const std::uint64_t tsc_end = read_tsc(c1);
        const auto wall_end = std::chrono::steady_clock::now();

        const auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end -
                                                                 wall_start)
                .count();
        TscCalibration cal;
        if (ns > 0) {
            cal.ticks_per_ns =
                static_cast<double>(tsc_end - tsc_start) / static_cast<double>(ns);
        }
        return cal;
    }

    std::uint64_t cycles_to_ns(std::uint64_t cycles) const noexcept {
        if (ticks_per_ns <= 0.0) return cycles;
        return static_cast<std::uint64_t>(static_cast<double>(cycles) /
                                          ticks_per_ns);
    }
};

// RAII scope timer. Records the cycle delta of a hot-path operation and reports
// it to a sink, but ONLY if the thread did not migrate cores mid-measurement.
// A migration invalidates the TSC comparison (different cores' TSCs can be
// offset), so such samples are dropped to avoid polluting the distribution.
template <class Sink>
class ScopedSample {
public:
    explicit ScopedSample(Sink& sink) noexcept
        : sink_(sink), start_(read_tsc(cpu_start_)) {}

    ~ScopedSample() noexcept {
        unsigned cpu_end = 0;
        const std::uint64_t end = read_tsc(cpu_end);
        if (cpu_end == cpu_start_) {
            sink_.record(end - start_);
        }
        // else: cross-core migration -> discard, do not record.
    }

    ScopedSample(const ScopedSample&) = delete;
    ScopedSample& operator=(const ScopedSample&) = delete;

private:
    Sink& sink_;
    unsigned cpu_start_ = 0;
    std::uint64_t start_ = 0;
};

} // namespace lob
