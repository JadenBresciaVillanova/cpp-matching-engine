# Design Notes

Architecture and rationale for a single-threaded, allocation-free limit order
book & matching engine in C++20.

---

## Core Design Decisions

- **Integer ticks, never floating point** for price/quantity. Floats can't
  exactly represent decimal money and break the equality comparisons matching
  depends on. `Price` is a signed tick index; `Quantity` is unsigned.
- **Direct-indexed array of price levels** (`levels_[tick]`), NOT a tree or hash
  map. O(1) access, contiguous memory so the inward matching walk is a
  cache-friendly sequential scan. Cost is memory proportional to tick *range*, not live order
  count — fine for a single instrument. (Production hybrid for huge ranges is
  noted as a known extension.)
- **Intrusive doubly-linked FIFO per price level.** Links live inside the Order
  record in the pool, so no per-order list-node allocation. O(1) append (time
  priority preserved), O(1) pop on fill, O(1) cancel given a handle.
- **Pool allocator; zero malloc on the hot path.** All order slots pre-allocated;
  intrusive free list hands out slots by index in O(1). A general allocator can
  lock/fault/fragment — any of which is a tail-latency spike.
- **Single-threaded matching core, on purpose.** Fastest engines run the core on
  one pinned thread, no locks (cf. LMAX Disruptor); cross-core coordination IS
  the latency. Concurrency belongs at the edges (ingest/publish) via SPSC ring
  buffers, not in the core.
- **Cycle-accurate timing via `rdtscp`**, NOT `high_resolution_clock` (can be
  non-monotonic / aliased to system_clock). `rdtscp` returns the core id so
  samples spanning a core migration are detected and discarded. TSC-to-ns
  conversion uses a once-at-startup calibration against steady_clock (assumes
  invariant TSC).

## Repo Layout

```
include/lob/   types.hpp, pool.hpp, rdtsc.hpp, order_book.hpp,
               photon_book.hpp, flat_map.hpp, spsc_queue.hpp,
               publisher.hpp, parsers.hpp, ref_order_book.hpp   (public headers)
src/core/      order_book.cpp                                   (implementation)
tests/         test_order_book.cpp, test_photon.cpp,
               test_spsc_queue.cpp, fuzz_cross_check.cpp        (tests)
benchmarks/    bench_main.cpp, ablation_main.cpp, replay_main.cpp
demos/         demo_main.cpp, dashboard_main.cpp                (terminal UIs)
tools/         report_main.cpp, gen_lobster.js                  (tooling)
data/lobster/  LOBSTER-format sample data (9 symbols)
docs/          benchmarking.md                                  (bare-metal runbook)
CMakeLists.txt  modern target-based; FetchContent pulls GoogleTest
```

Namespace, directory, and intended binary all share the name `lob`.

## Build / Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
./build/lob_bench
```

`-O3 -march=native` in Release (non-portable binary — fine for local benchmarks,
note for CI). Code compiles clean under `-Wall -Wextra`.

Sanitizer build:

```bash
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DLOB_SANITIZE=ON
cmake --build build-san -j
ctest --test-dir build-san
```

## Component Summary

### Core Engine (OrderBook)
- Core types, pool allocator (with deferred trivially-copyable static_assert to
  avoid a circular type-completeness bug — Order holds `PoolHandle`, a standalone
  type, NOT `Pool<Order>::Handle`, which is why that indirection exists).
- TSC calibration + ScopedSample timing with core-migration drop.
- Resting limit orders; best_bid_/best_ask_ caching with inward reprice walk on
  empty (sentinel -1 = no side).
- Cancel (O(1) via id→handle index; correct interior-node unlink; reprices best
  if the top level empties).
- **Matching sweep**: price-then-time priority, partial fills (both incoming and
  resting sides), multi-order and multi-level sweeps, limit-price cap (no trading
  through the limit), price improvement (trades execute at the RESTING price).
- **Market / IOC / FOK** order types — all three reuse the existing sweep:
  - Market: worst-case limit price so the cross test always passes; never rests.
  - IOC: sweeps with the user's limit price; never rests.
  - FOK: read-only `can_fill()` pre-check, then all-or-nothing sweep.
- **modify()**: in-place size *decrease* keeps time priority; price change or
  size *increase* = cancel + resubmit (loses priority). Modify that crosses the
  book routes through the matching sweep automatically.
- **Self-trade prevention** — cancel-incoming (cancel-aggressor) policy. An
  `OwnerId` field on each order; `match()` breaks on same-owner; `can_fill()`
  walks the FIFO to exclude unreachable same-owner liquidity so FOK stays
  all-or-nothing. `owner=0` means anonymous / no STP.

### PhotonBook — Production-Hardened Variant
- GTD (Good-Til-Date) expiry with `check_expiry(now_ns)`
- FIX-style lifecycle events (Ack, Fill, Canceled, Replaced, Expired) via
  compile-time-polymorphic event sink (zero-overhead `NullSink` option)
- Fibonacci hash map (`FlatMap`) for order ID lookup
- FIFO pool policy, cache-line-aligned order struct

### Infrastructure
- **SPSC lock-free ring buffer**: acquire/release atomics, power-of-2 capacity,
  cache-line-padded head/tail. Connects ingest → core → publisher.
- **Publisher thread**: drains outbound queue in batches.
- **LOBSTER parser**: reads academic L3 market data (CSV) with configurable
  tick divisor.
- **ITCH 5.0 parser**: real NASDAQ binary protocol — big-endian readers,
  fixed-size message types (A/F/E/C/X/D/U), symbol filtering, 2-byte length
  prefix support.
- **Format auto-detection**: file extension + content sniffing.
- **3-thread terminal dashboard** (`dashboard_main.cpp`): SPSC ingest → core →
  publisher with live queue depths.

### Benchmarking & Validation
- **Benchmark harness** (`bench_main.cpp`): per-operation-type latency
  measurement using rdtscp. Separate benchmarks for submit (resting), submit
  (match), cancel, modify — each with its own seeded steady-state book, warmup
  phase, and replenishment strategy. Reports p50/p99/p99.9/max.
- **Ablation study** (`ablation_main.cpp`): one-variable-at-a-time isolation of
  each optimization's latency contribution — std::map vs direct array, new/delete
  vs pool allocator, linear BBO scan vs cached BBO.
- **Replay driver** (`replay_main.cpp`): deterministic replay of LOBSTER/ITCH
  data through the engine.
- **Reference-model cross-check** (`ref_order_book.hpp` + `fuzz_cross_check.cpp`):
  a deliberately slow, obviously-correct `std::map`/`std::deque` book run
  alongside the fast one on 1M+ seeded random operations. Compares every
  observable: ExecResult fields, trade stream, best bid/ask, open orders, and
  full bid/ask depth per tick. Includes a binary-search shrinker that finds the
  minimal failing prefix on divergence.
- **173 tests** across 4 binaries — 47 typed GoogleTest cases × 3 book
  types + 2 standalone tests + 19 PhotonBook tests + 10 SPSC queue tests +
  1 fuzz suite.

### Measured Latency (AWS c5.metal bare metal)

| Operation        | p50    | p99    | p99.9  |
| ---------------- | ------ | ------ | ------ |
| Submit (resting) | 48 ns  | 60 ns  | 68 ns  |
| Submit (match)   | 78 ns  | 144 ns | 164 ns |
| Cancel           | 100 ns | 140 ns | 157 ns |
| Modify           | 136 ns | 184 ns | 200 ns |

Hardware: Intel Xeon Platinum 8275CL @ 3.00 GHz, kernel 6.17.0-1012-aws,
Ubuntu 24.04, `-O3 -march=native`, `cpupower performance` governor.

## Benchmarking Methodology

Correctness runs anywhere (Windows, WSL2, VM). **Latency numbers are only
trustworthy on an isolated core of a PHYSICAL machine.** VMs/free-tiers inject
multi-microsecond/ms jitter into the tail — exactly what the design exists to
remove — so p99.9 in a VM is meaningless. Recommended: hourly bare metal
(~$0.09/hr, torn down inside an hour = ~$0.10–$0.20 per session), or a spare
physical x86 box for $0. Full runbook (isolcpus, taskset, governor, TSC
verification, methodology) is in `docs/benchmarking.md`. If re-measured on
different hardware, update README.md (latency table), DESIGN.md (table above),
and report.html (Linux Results tab).
