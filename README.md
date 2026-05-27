# lob — a cache-optimized limit order book & matching engine

A single-threaded, allocation-free limit order book and matching engine in
C++20, built to demonstrate low-latency systems engineering: price-time
priority matching, an intrusive-list / direct-indexed-array data structure, a
pool allocator that keeps the hot path free of `malloc`, and **cycle-accurate
latency measurement reported as full percentile distributions (p50/p99/p99.9)**.

> **Status:** stage 2 in progress. Core data structures, timing, build, resting
> orders, and the **matching sweep** (price-time priority, partial fills, price
> improvement, multi-level sweeps) are implemented and covered by tests, all
> compiling clean under `-Wall -Wextra`. Market/IOC/FOK, modify, the reference-
> model cross-check, and the benchmark/ablation harness are next.

---

## The headline (for the impatient)

*(to be filled with real numbers once benchmarked on isolated-core Linux)*

| Operation            | p50      | p99      | p99.9    |
| -------------------- | -------- | -------- | -------- |
| add (rest) order     | _tbd_    | _tbd_    | _tbd_    |
| cancel order         | _tbd_    | _tbd_    | _tbd_    |
| match (per fill)     | _tbd_    | _tbd_    | _tbd_    |

Measured with `rdtscp`, TSC-frequency-calibrated, warmup discarded, samples
dropped on detected core migration. Numbers are reported as distributions
because **in low-latency work the tail is the product** — a good mean hides the
jitter that lock-free, allocation-free designs exist to remove.

---

## Why these design choices (the part engineers read)

**Direct-indexed array of price levels, not a tree or hash map.** `levels_[tick]`
is O(1) and lays price levels out contiguously, so the inward walk that matching
performs is a cache-friendly sequential scan rather than a pointer chase across
a red-black tree. Cost is memory proportional to the tick *range*, not live
order count — a small, fixed, cache-resident array for a single instrument.

**Intrusive doubly-linked FIFO per price level.** Links live inside the order
record in the pool, so there is no per-order list-node allocation. Append on
arrival, pop on fill, and cancel-by-handle are all O(1), and popping from the
head preserves time priority.

**Pool allocator; zero `malloc` on the hot path.** All order slots are
pre-allocated. A general allocator can lock, fault, or fragment — any of which
is a tail spike. The pool hands out slots by index from an intrusive free list
in O(1).

**Single-threaded matching core, on purpose.** The fastest matching engines run
the core on one pinned thread with no locks (cf. LMAX Disruptor); cross-core
coordination *is* the latency. Concurrency belongs at the edges (ingest /
publish) via SPSC ring buffers, never inside matching.

**Integer ticks, never floating point** for price/quantity — floats can't
exactly represent decimal money and break equality comparisons that matching
depends on.

## What a production system adds (honest scope)

This is a faithful demonstration of the *principles*, not a drop-in venue. A
production low-latency stack additionally needs: Linux with boot-time core
isolation (`isolcpus` / `nohz_full`) and NUMA pinning; kernel-bypass networking
(Solarflare/onload, DPDK) — Windows is not a low-latency OS and is deliberately
not targeted here; NIC hardware timestamping for true tick-to-trade; a hybrid
price structure for instruments with very large tick ranges; persistence /
sequencing / gap recovery; and for the most extreme latencies, FPGA on the hot
path. Naming what's *not* here is part of the point.

## Measurement methodology

`steady_clock` for coarse timing (never `high_resolution_clock`, which can be
non-monotonic). For per-operation latency, `rdtscp` cycle counts converted to
nanoseconds via a once-at-startup TSC calibration; `rdtscp` returns the core id,
so samples spanning a core migration are discarded. A warmup phase is run and
discarded so first-touch page faults and cold caches don't pollute the
distribution. Percentiles via HdrHistogram (added stage 2).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build      # tests
./build/lob_bench           # benchmark/replay driver
```

Release is the default build type because benchmarks in Debug are meaningless.
`-march=native` is used in Release for local benchmarking (not a portable
binary).

## Layout

```
include/lob/   public headers (types, pool, rdtsc, order_book)
src/core/      order book implementation
tests/         GoogleTest suite (fetched automatically)
benchmarks/    benchmark / replay driver
docs/          architecture notes & diagrams
```

## Roadmap

- **Stage 1 (done):** scaffold, core types, pool allocator, TSC timing, build.
- **Stage 2 (in progress):** matching logic.
  - done: resting limit orders, cancel, the matching sweep (price-time
    priority, partial fills, multi-level/multi-order sweeps, price improvement,
    limit-price cap).
  - next: Market / IOC / FOK, modify (in-place size decrease keeps priority;
    price change or size increase = cancel + resubmit), self-trade prevention,
    and a slow reference model cross-checked against the fast book on seeded
    random order flow.
- **Stage 3:** benchmark harness with HdrHistogram percentiles, deterministic
  replay of synthetic *and* recorded market data (e.g. a free LOBSTER/ITCH
  sample), and the **ablation study** isolating each optimization's latency
  contribution. Run on isolated-core bare metal — see
  [`docs/benchmarking.md`](docs/benchmarking.md) for the runbook and cost
  (~$0.10–$0.20 per session on hourly bare metal, or $0 on a spare box).
- **Stage 4:** SPSC ring buffer edges, optional live dashboard skin.
