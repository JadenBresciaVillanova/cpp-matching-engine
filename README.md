# lob — Cache-Optimized Limit Order Book & Matching Engine

A single-threaded, allocation-free limit order book and matching engine in
C++20. Built to demonstrate the data-structure and systems-engineering
techniques that real HFT matching engines use: direct-indexed O(1) price
lookup, intrusive FIFO queues, a pool allocator that keeps `malloc` off the hot
path, and cycle-accurate `rdtscp` latency measurement with core-migration
detection.

```
                        ┌──────────────────────────────────────────────────┐
 ┌──────────┐   push    │              Matching Core                      │   push    ┌───────────┐
 │  Ingest  │──────────▶│                                                 │──────────▶│ Publisher  │
 │  Thread  │  SPSC     │  ┌────────────────────────────────────────────┐  │  SPSC     │  Thread   │
 │          │  Queue    │  │  Price Array  [0] [1] [2] ... [N]         │  │  Queue    │           │
 └──────────┘  (lock-   │  │  O(1) index   ▼   ▼   ▼       ▼         │  │  (lock-   └───────────┘
   LOBSTER     free)    │  │             ┌───┐ nil ┌───┐  ┌───┐       │  │   free)     Logging
   ITCH 5.0             │  │             │ O │     │ O │  │ O │       │  │             Broadcast
   WebSocket            │  │             │ ↕ │     │ ↕ │  │ ↕ │       │  │             Dashboard
                        │  │             │ O │     │ O │  │ O │       │  │
                        │  │             └───┘     └───┘  └───┘       │  │
                        │  │           Intrusive FIFO per level       │  │
                        │  └────────────────────────────────────────────┘  │
                        │                                                 │
                        │  Pool Allocator ─ zero malloc on hot path       │
                        │  Cached BBO ─ O(1) best bid/ask                 │
                        │  Single pinned core ─ no locks, no contention   │
                        └──────────────────────────────────────────────────┘
```

---

## Latency

| Operation        | p50    | p99    | p99.9  |
| ---------------- | ------ | ------ | ------ |
| Submit (resting) | 48 ns  | 60 ns  | 68 ns  |
| Submit (match)   | 78 ns  | 144 ns | 164 ns |
| Cancel           | 100 ns | 140 ns | 157 ns |
| Modify           | 136 ns | 184 ns | 200 ns |

Measured with `rdtscp` (cycle-accurate, ~0.3ns resolution), TSC calibrated at
startup (3.00 GHz), 50K warmup ops discarded, 500K ops measured per category at
10K orders/side steady-state depth. Samples dropped on core migration. Hardware:
AWS `c5.metal` (96 vCPU bare metal), Intel Xeon Platinum, Ubuntu 24.04,
`-O3 -march=native`, `cpupower performance` governor.

The tail is the product — p50 to p99.9 stays under 2× for every operation.
That flat tail is what the lock-free, allocation-free design exists to deliver.

### Ablation Study

Each variant changes **one variable** from the baseline. Same workload, same
seed, same measurement — isolates each optimization's individual contribution.

| Variant                  | p50    | vs Baseline |
| ------------------------ | ------ | ----------- |
| Baseline (all opts)      | 54 ns  | —           |
| std::map price levels    | 94 ns  | +74%        |
| new/delete (no pool)     | 72 ns  | +33%        |
| Linear BBO scan          | 536 ns | +893%       |

```
Impact waterfall (p50 overhead vs baseline):

  BBO caching              +482 ns  ##############################
  Direct-indexed array      +40 ns  ##
  Pool allocator            +18 ns  #
```

**BBO caching dominates.** Without it, every submit and cancel triggers a
linear scan for the new best price — the 10× penalty dwarfs the data-structure
and allocation choices combined. This is the ablation an interviewer looks for:
changing one variable at a time, honest measurement, and the answer isn't what
you'd naively expect.

---

## Design Decisions

**Direct-indexed array of price levels, not a tree or hash map.**
`levels_[tick]` is O(1) and lays price levels out contiguously, so the inward
matching walk is a cache-friendly sequential scan instead of a pointer chase
through a red-black tree. Memory cost is proportional to tick *range*, not live
order count — a fixed, cache-resident array for a single instrument.

**Intrusive doubly-linked FIFO per price level.** `prev`/`next` pointers live
inside the `Order` struct in the pool — no per-order list-node allocation.
Append on arrival, pop on fill, cancel-by-handle are all O(1), and popping from
the head preserves time priority.

**Pool allocator; zero `malloc` on the hot path.** All order slots are
pre-allocated at startup. A general allocator can lock, fault, or fragment —
any of which is a tail spike. The pool hands out slots from an intrusive free
list in O(1). Supports both LIFO (cache-hot reuse) and FIFO (predictable aging)
free-list policies.

**Single-threaded matching core, on purpose.** The fastest matching engines run
the core on one pinned thread with no locks (cf. LMAX Disruptor). Cross-core
coordination *is* the latency. Concurrency belongs at the edges via SPSC ring
buffers, never inside matching.

**Integer ticks, never floating point.** Floats can't exactly represent decimal
money and break the equality comparisons matching depends on. `Price` is a
signed tick index; `Quantity` is unsigned.

**`rdtscp` timing, not `std::chrono`.** Reads the CPU timestamp counter
directly (~0.3ns resolution) instead of `high_resolution_clock`, which can
alias to `system_clock` or call into the kernel. `rdtscp` also returns the core
ID, so samples spanning a core migration are detected and discarded.

---

## Features

### Core Engine (`OrderBook`)
- Price-time priority matching with partial fills, multi-level sweeps, and
  limit-price cap (trades execute at the resting price)
- All order types: **Limit**, **Market**, **IOC**, **FOK** — each reusing the
  same matching sweep
- **Modify**: in-place size decrease keeps time priority; price change or size
  increase = cancel + resubmit (loses priority). Modify that crosses the book
  routes through matching automatically
- **Self-trade prevention** (cancel-incoming policy) with per-order `OwnerId`
- Cached best bid/ask with inward reprice walk on empty levels

### PhotonBook — Production-Hardened Variant
- **GTD (Good-Til-Date) expiry**: orders auto-cancel at a specified nanosecond
  timestamp via `check_expiry(now_ns)`
- **FIX-style lifecycle events**: Ack, Fill, Canceled, Replaced, Expired —
  published through a compile-time-polymorphic event sink (zero-overhead
  `NullSink` option)
- **Fibonacci hash map** (`FlatMap`) for order ID lookup — open-addressed,
  golden-ratio hash, linear probing, pre-sized
- **FIFO pool policy** for predictable memory aging
- Cache-line-aligned order struct

### Infrastructure
- **SPSC lock-free ring buffer**: acquire/release atomics, power-of-2 capacity,
  cache-line-padded head/tail. Connects ingest → core → publisher
- **Publisher thread**: drains outbound queue in batches, handles logging and
  broadcast without stalling the core
- **LOBSTER parser**: reads academic L3 market data (CSV) with configurable
  tick divisor
- **ITCH 5.0 parser**: real NASDAQ binary protocol parsing — big-endian
  readers, fixed-size message types (A/F/E/C/X/D/U), symbol filtering, 2-byte
  length prefix support (372 lines of real protocol code, not stubs)
- **Format auto-detection**: file extension + content sniffing

### Reference Model & Testing
- **`RefOrderBook`**: a deliberately slow, obviously-correct implementation
  using `std::map` + `std::deque` for cross-checking
- **Fuzz cross-check**: 1M+ seeded random operations comparing every observable
  (ExecResult, trades, BBO, open orders, full depth per tick) between fast and
  reference books, with binary-search shrinking on divergence
- **173 tests** across 4 binaries: 47 typed GoogleTest cases × 3 book types +
  2 standalone tests + 19 PhotonBook tests + 10 SPSC queue tests + 1 fuzz suite

### Interactive Dashboard
- **13-tab HTML dashboard** (`report.html`) with live order book visualization
- Solo Simulator, Latency Race, P&L Race with 4 engine model variants
- Live Binance WebSocket integration (BTC, ETH, SOL, DOGE, XRP)
- Data Explorer loading real LOBSTER-format sample data
- C++ source reference, production gap analysis, architecture education
- Landing page (`index.html`) with glassmorphism design and concept explainers

### Terminal Tools
- `lob_bench` — per-operation latency with rdtscp, warmup, percentiles
- `lob_ablation` — isolate each optimization's latency contribution
- `lob_replay` — replay LOBSTER/ITCH data through the engine
- `lob_demo` — quick terminal demonstration
- `lob_dashboard` — 3-thread SPSC terminal UI with live queue depths

---

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build          # run all tests
./build/lob_bench               # benchmark driver
./build/lob_dashboard           # 3-thread SPSC terminal UI
./build/lob_replay file <path>  # replay LOBSTER/ITCH data
```

Release is the default build type — benchmarks in Debug are meaningless.
`-O3 -march=native` in Release for full ISA utilization (non-portable binary;
fine for local benchmarking).

---

## Project Structure

```
include/lob/
  types.hpp           Core types: Price, Quantity, Side, OrderType, ExecResult
  pool.hpp            Pool allocator with intrusive free list (LIFO + FIFO)
  order_book.hpp      OrderBook — direct-indexed array + intrusive FIFO
  ref_order_book.hpp  RefOrderBook — std::map reference model
  photon_book.hpp     PhotonBook — GTD, lifecycle events, FlatMap, FIFO pool
  flat_map.hpp        Fibonacci hash map (open-addressed, golden-ratio hash)
  spsc_queue.hpp      Lock-free SPSC ring buffer (acquire/release atomics)
  publisher.hpp       Outbound event publisher (batched drain, thread-safe)
  parsers.hpp         LOBSTER CSV + ITCH 5.0 binary parsers
  rdtsc.hpp           TSC calibration + ScopedSample with core-migration drop

src/core/
  order_book.cpp      OrderBook implementation (matching, cancel, modify, STP)

tests/
  test_order_book.cpp Typed tests across OrderBook, RefOrderBook, PhotonBook
  test_photon.cpp     PhotonBook-specific: GTD, lifecycle events, FlatMap
  test_spsc_queue.cpp SPSC queue correctness tests
  fuzz_cross_check.cpp 1M+ random op cross-check with binary-search shrinking

benchmarks/
  bench_main.cpp      Per-operation rdtscp latency measurement
  ablation_main.cpp   One-variable-at-a-time ablation study
  replay_main.cpp     LOBSTER/ITCH replay driver

demos/
  demo_main.cpp       Quick terminal demo
  dashboard_main.cpp  3-thread SPSC terminal UI

tools/
  report_main.cpp     Report/dashboard generation
  gen_lobster.js      LOBSTER sample data generator

data/lobster/         LOBSTER-format sample data (9 symbols, ~35K events each)
docs/                 Bare-metal benchmarking runbook
index.html            Landing page
report.html           Interactive 13-tab dashboard
```

---

## What a Production System Adds

This is a faithful demonstration of the *principles*, not a drop-in venue. A
production low-latency stack additionally needs:

- **Linux with core isolation** (`isolcpus` / `nohz_full`) and NUMA pinning
- **Kernel-bypass networking** (Solarflare/onload, DPDK) — Windows is not a
  low-latency OS
- **NIC hardware timestamping** for true tick-to-trade measurement
- **Hybrid price structure** for instruments with very large tick ranges
- **Persistence / sequencing / gap recovery** for crash resilience
- **Risk checks and circuit breakers** before matching
- **FPGA acceleration** on the most latency-sensitive paths

Naming what's *not* here is part of the point — the interactive dashboard
includes a [Production Gap](report.html#gap) tab that documents each gap and
why it's deliberately out of scope.

---

## Measurement Methodology

1. **`rdtscp` for per-operation latency** — cycle counts converted to
   nanoseconds via once-at-startup TSC calibration. Core ID checked on every
   sample; cross-core migrations are discarded.
2. **Warmup discarded** — first N operations run before measurement so
   first-touch page faults and cold caches don't pollute the distribution.
3. **Percentile reporting** — p50 / p99 / p99.9 / max. The mean is not
   reported because it hides the tail jitter these designs exist to remove.
4. **Steady-state measurement** — book depth reaches representative levels
   before timing begins; each operation category measured independently.
5. **Isolated core** — `taskset` pins the process; `isolcpus` boot parameter
   keeps the OS scheduler off the measurement core.

See [docs/benchmarking.md](docs/benchmarking.md) for the full runbook including
hardware provisioning, TSC verification, and cost breakdown.

---

Built by [Jaden Brescia](https://github.com/JadenBresciaVillanova)
