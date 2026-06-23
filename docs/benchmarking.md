# Benchmarking runbook

Correctness is proven anywhere (your laptop, WSL2, a VM). **Latency numbers are
only trustworthy on an isolated core of a physical machine.** This document is
the exact procedure for producing publishable numbers, and the methodology notes
that make those numbers defensible when someone asks "what did you run this on?"

## Why not a VM / WSL2 for numbers

A hypervisor can preempt your vCPU at any instant to service the host or other
tenants. That injects multi-microsecond — sometimes millisecond — stalls into
individual samples. Your p50 will look fine; your **p99.9 and max will be
dominated by virtualization jitter that has nothing to do with the engine**.
Since flat tail latency is the entire point of a lock-free, allocation-free
design, a VM obscures exactly what we are trying to show. Free/burstable tiers
are the worst case: most oversubscribed, plus CPU-credit throttling. Use VMs and
WSL2 freely for development and the correctness suite; do not quote their
latency.

## Recommended: hourly bare metal (~10–20 cents per session)

Bare metal rents by the hour and provisions in ~20–30 minutes. Entry-level
machines start around **$0.09/hour** (Cherry Servers; Hetzner / OVH / Latitude.sh
are comparable). You spin it up, run the suite for under an hour, tear it down.
Total cost per benchmarking session is typically **$0.10–$0.20**. You do NOT
need a fast machine — this is a single-threaded engine and we measure
per-operation latency, not a throughput race, so any modern x86-64 box is fine.
A spare physical machine you already own is the same thing for $0.

> Pricing drifts; reconfirm current rates at provision time. Watch egress
> charges if you download large recorded market-data files onto the box —
> generate synthetic flow on the box instead, or fetch the sample once.

## Machine requirements

- x86-64 CPU with **invariant TSC** (`constant_tsc` + `nonstop_tsc`) — universal
  on modern Intel/AMD; we verify rather than assume (see Step 3).
- At least 2 physical cores so we can isolate one for the engine and leave the
  OS on the others. 4 is comfortable. More does not help — single-threaded core.
- A few GB RAM. Storage and network are irrelevant to the measurement.
- Avoid: shared/burstable instances, anything where you cannot pass kernel boot
  params or run `taskset`.

## Step 0 — provision

Pick a bare-metal host, choose a recent Ubuntu LTS image, provision, SSH in.

```bash
sudo apt update && sudo apt install -y build-essential cmake git linux-tools-common linux-tools-$(uname -r)
```

## Step 1 — verify invariant TSC

```bash
grep -o 'constant_tsc\|nonstop_tsc' /proc/cpuinfo | sort -u
```
Expect both. If missing, TSC→ns conversion is unreliable on this host; pick a
different machine. (The engine detects core migration via rdtscp regardless, but
the ns calibration assumes invariant TSC.)

## Step 2 — isolate a core

Reserve a core so the OS scheduler and most interrupts stay off it. Edit
`/etc/default/grub`, append to `GRUB_CMDLINE_LINUX_DEFAULT`:

```
isolcpus=3 nohz_full=3 rcu_nocbs=3
```

(Use the highest core id; here core 3 on a 4-core box.) Then:

```bash
sudo update-grub && sudo reboot
```

After reboot, confirm the core is isolated:

```bash
cat /sys/devices/system/cpu/isolated     # should show: 3
```

## Step 3 — pin frequency (optional, sharpens the tail further)

Turbo/throttling makes cycle→ns conversion noisier. For the cleanest numbers,
pin the governor to performance:

```bash
sudo cpupower frequency-set -g performance
```

## Step 4 — build Release and run pinned

```bash
git clone https://github.com/JadenBresciaVillanova/cpp-matching-engine.git lob && cd lob
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
taskset -c 3 ./build/lob_bench           # run the engine ON the isolated core
```

`taskset -c 3` pins the process to the isolated core. The benchmark itself
ALSO sets thread affinity in-process (defense in depth) and discards any sample
where rdtscp reports a core change.

## Step 5 — methodology the report must state

These are non-negotiable for the numbers to be believed:

1. **Warmup discarded.** Run N operations before measuring so first-touch page
   faults, cold caches, and branch-predictor training do not pollute the
   distribution.
2. **Report the distribution, not the mean.** p50 / p99 / p99.9 / max via
   HdrHistogram. A good mean hides the jitter the design exists to remove; the
   tail IS the product.
3. **Drop cross-core samples.** rdtscp returns the core id; a sample spanning a
   migration compares two cores' TSCs and is invalid.
4. **State the hardware.** CPU model, core isolation settings, governor, kernel,
   compiler + flags (`-O3 -march=native`), and that it ran on an isolated core.
5. **Steady state.** Measure after the book reaches a representative depth, not
   on an empty book.

## Step 6 — tear down

Destroy the instance immediately after capturing results so billing stops. Copy
the histogram output and the `cat /proc/cpuinfo` / kernel-cmdline provenance into
the repo alongside the numbers so the run is reproducible and auditable.

## Cost summary

| Route                          | Cost            | Tail quality |
| ------------------------------ | --------------- | ------------ |
| Spare physical x86 box (owned) | $0              | gold         |
| Hourly bare metal, <1 hr       | ~$0.10–$0.20    | gold         |
| Dedicated VM (non-burstable)   | low, hourly     | fair         |
| Free-tier / burstable VM       | $0              | unusable     |
| WSL2 / local VM                | $0              | dev only     |
