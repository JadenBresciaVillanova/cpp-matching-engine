// bench_main.cpp
// Stage-1 driver: proves TSC calibration works and the binary runs in Release.
// Stage 2 replaces this with the real harness: warmup, randomized order flow,
// per-operation cycle sampling into an HdrHistogram, and the ablation runner.

#include "lob/rdtsc.hpp"
#include <cstdio>

int main() {
    const auto cal = lob::TscCalibration::measure();
    std::printf("TSC calibration: %.4f ticks/ns\n", cal.ticks_per_ns);

    // Demonstrate a sampled region.
    struct CountSink {
        std::uint64_t last = 0;
        void record(std::uint64_t cycles) { last = cycles; }
    } sink;
    {
        lob::ScopedSample<CountSink> s(sink);
        volatile std::uint64_t x = 0;
        for (int i = 0; i < 1000; ++i) x += i;
    }
    std::printf("sampled region: %llu cycles (~%llu ns)\n",
                (unsigned long long)sink.last,
                (unsigned long long)cal.cycles_to_ns(sink.last));
    return 0;
}
