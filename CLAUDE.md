# CLAUDE.md — project context & handoff

This file is read automatically by Claude Code. It carries the full context from
the chat where this project was designed and scaffolded, so any session starts
already knowing the *why* behind the code, not just the *what*.

---

## What this is

A single-threaded, allocation-free **limit order book & matching engine** in
C++20. The point of the project is to demonstrate low-latency systems
engineering to a depth that an HFT/low-latency C++ engineer would recognize as
industry-standard — not a toy. Two audiences must both be served: a recruiter
skimming the README (headline latency numbers + a diagram) and an engineer
reading the code (methodology, percentiles, ablation, honest scope).

The owner wants to **learn this deeply** (will be grilled on it), so favor
building features *together*, incrementally, with tests landing alongside each
piece, over dumping large amounts of unexplained code. Embed design rationale as
comments — the repo should teach.

## Core design decisions (and the reasoning — defend these in interviews)

- **Integer ticks, never floating point** for price/quantity. Floats can't
  exactly represent decimal money and break the equality comparisons matching
  depends on. `Price` is a signed tick index; `Quantity` is unsigned.
- **Direct-indexed array of price levels** (`levels_[tick]`), NOT a tree or hash
  map. O(1) access, contiguous memory so the inward matching walk is a
  cache-friendly sequential scan. Cost is memory ∝ tick *range*, not live order
  count — fine for a single instrument. (Production hybrid for huge ranges is
  noted as a known extension.)
- **Intrusive doubly-linked FIFO per price level.** Links live inside the Order
  record in the pool, so no per-order list-node allocation. O(1) append (time
  priority preserved), O(1) pop on fill, O(1) cancel given a handle.
- **Pool allocator; zero malloc on the hot path.** All order slots pre-allocated;
  intrusive free list hands out slots by index in O(1). A general allocator can
  lock/fault/fragment = tail spikes, which is the whole thing we're avoiding.
- **Single-threaded matching core, on purpose.** Fastest engines run the core on
  one pinned thread, no locks (cf. LMAX Disruptor); cross-core coordination IS
  the latency. Concurrency belongs at the edges (ingest/publish) via SPSC ring
  buffers — stage 4, not in the core.
- **cycle-accurate timing via `rdtscp`**, NOT `high_resolution_clock` (can be
  non-monotonic / aliased to system_clock). `rdtscp` returns the core id so
  samples spanning a core migration are detected and DISCARDED. TSC→ns needs a
  once-at-startup calibration against steady_clock (assumes invariant TSC).

## Repo layout

```
include/lob/   types.hpp, pool.hpp, rdtsc.hpp, order_book.hpp   (public headers)
src/core/      order_book.cpp                                   (implementation)
tests/         test_order_book.cpp                              (GoogleTest)
benchmarks/    bench_main.cpp                                   (driver)
docs/          benchmarking.md                                  (bare-metal runbook)
CMakeLists.txt  modern target-based; FetchContent pulls GoogleTest
```

Namespace, directory, and intended binary all share the name `lob`.

## Build / test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # Release is default; Debug benchmarks are meaningless
cmake --build build -j
ctest --test-dir build
./build/lob_bench
```

`-O3 -march=native` in Release (non-portable binary — fine for local benchmarks,
note for CI). Code compiles clean under `-Wall -Wextra`; keep it that way.

## What's DONE (stage 2 in progress)

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
- Tests: 18 committed GoogleTest cases, all verified passing.

## What's NEXT (in order)

1. **Market / IOC / FOK** order types — reuse the existing sweep:
   - Market: pass a worst-case price so the cross test always passes while
     liquidity remains; cancel (don't rest) any remainder.
   - IOC: like a limit sweep but the remainder is cancelled, not rested.
   - FOK: check fillability FIRST (walk to confirm enough crossing liquidity),
     then either fill completely or do nothing — no partial.
2. **modify()**: in-place size *decrease* keeps time priority; price change or
   size *increase* = cancel + resubmit (loses priority). Getting this wrong is a
   classic interview gotcha — be explicit.
3. **Self-trade prevention** (decide a policy: cancel-resting, cancel-incoming,
   or cancel-both; document the choice).
4. **Reference-model cross-check**: a deliberately slow, obviously-correct book
   run alongside the fast one on thousands of *seeded* random operations,
   asserting they always agree. This is the strongest correctness argument to
   show an interviewer.

## Then stage 3 (benchmarks) and stage 4 (edges)

- HdrHistogram percentiles (p50/p99/p99.9/max), warmup discarded, steady-state.
- Deterministic replay of synthetic AND recorded market data (free LOBSTER/ITCH
  sample day).
- **Ablation study**: isolate each optimization's individual latency
  contribution (lock→lock-free, DOM→on-demand parse, tree/sort→array O(1)),
  producing a waterfall. CRITICAL: the original project idea rigged the A/B test
  by changing multiple variables at once (and used std::sort on every update vs
  O(1) indexing) — an interviewer would call that out. Change ONE variable at a
  time so the latency story is honest.
- Stage 4: SPSC ring-buffer edges, optional live dashboard skin.

## Benchmarking — IMPORTANT

Correctness runs anywhere (Windows, WSL2, VM). **Latency numbers are only
trustworthy on an isolated core of a PHYSICAL machine.** VMs/free-tiers inject
multi-µs/ms jitter into the tail — exactly what the design exists to remove — so
p99.9 in a VM is meaningless. Recommended: hourly bare metal (~$0.09/hr, torn
down inside an hour = ~$0.10–$0.20 per session), or a spare physical x86 box for
$0. Full runbook (isolcpus, taskset, governor, TSC verification, methodology) is
in `docs/benchmarking.md`. The README latency table is intentionally left `tbd`
— DO NOT invent numbers; fill only with real measured results and state the
hardware/flags they came from.

## Working style notes

- Implement + test each feature before moving on; keep `-Wall -Wextra` clean.
- Prefer prose explanation of tradeoffs over walls of code.
- When something is rigged/hand-wavy/incorrect, say so plainly — the owner
  explicitly wants honest pushback, not agreement.
