# mem-plus

*Compact, one-line JSON memory/CPU/power snapshot for processes.*

A macOS utility that prints a compact, machine-readable snapshot of memory, CPU, and power usage for running processes — handy for watching local inference servers like llama.cpp and MLX.

## Setup
The `mem-plus` script runs as-is. Compile the bundled helpers once:
```bash
clang -O2 -o memfoot memfoot.c
clang -O2 -o memsys memsys.c
clang -O2 -framework CoreFoundation -o membw membw.c
```
If a helper is missing, only its fields read `N/A` — everything else still works:

| Missing helper | Fields that read `N/A` |
|---|---|
| `memfoot` | `Mem`, `Mem Peak` |
| `memsys` | `Mem Used`, `Mem Pressure`, `Mem Free%` |
| `membw` | `Mem BW` |

`powermetrics` needs `sudo` for CPU power, GPU power, and GPU%. Set `MEMPLUS_POWER=0` to skip that sample; those three fields then read `N/A` and mem-plus does not ask for a password. `membw` does not use sudo: base M4 reads AMC Stats byte counters as a normal user, and M4 Pro / M4 Max use PMP histograms.

## Usage
```
mem-plus <process-name-or-pattern>
```

### Examples:
```bash
# Get info for llama-server process
mem-plus llama-server

# Get info for mlx_lm.generate process
mem-plus mlx_lm.generate

# Use pattern matching (passed to pgrep -f)
mem-plus "llama-server|python"

# Skip sudo powermetrics. CPU Power, GPU Power, and GPU% read N/A.
MEMPLUS_POWER=0 mem-plus llama-server
```

## What It Prints
One JSON object PER MATCHING PROCESS, each on a single line. The single-line format is intentional so the output is easy to grep, pipe, and append to logs.

Each line looks like:

```json
{
  "<comm>": {
    "PID": 12345,
    "Mem": "845.5M",
    "Mem Peak": "912.0M",
    "RSS": "3098.3 MB",
    "CPU%": "142.0%",
    "NCPU%": "14.8%"
  },
  "Total": {
    "CPU Power": "5200mW",
    "GPU Power": "8100mW",
    "GPU%": "62.3%",
    "Mem BW": "17.4 GB/s",
    "Mem BW Max": "42.1 GB/s",
    "Mem Used": "8.61 GB",
    "Mem Pressure": "Normal",
    "Mem Free%": "80.0%"
  }
}
```

## Fields Explanation
- `<comm>`: Last 80 chars of the process command path (the JSON key).
- `PID`: Process id.
- `Mem`: Physical footprint (`memfoot` helper / `proc_pid_rusage`). What Activity Monitor calls "Memory". The number you should trust for real RAM cost.
- `Mem Peak`: Peak physical footprint since the process started.
- `RSS`: Resident Set Size (ps -o rss), in MB. A cross-check metric; see "RSS vs Mem" below for why it can differ wildly.
- `CPU%`: Instantaneous %CPU, htop-style. Measured with `top -l 2 -s 1` and reading the SECOND sample, which is a delta over a 1s interval. Sums across cores, so a process pinning N threads can exceed 100%. (See "Why CPU% Is Measured This Way" below.)
- `NCPU%`: CPU% normalized by logical core count (hw.ncpu): CPU% / ncpu. Caps at ~100% = the process is using the entire machine.
- `Total`: System-wide metrics, sampled ONCE per invocation:
  - `CPU Power`: Package CPU power draw (powermetrics). `N/A` when `MEMPLUS_POWER=0`.
  - `GPU Power`: GPU power draw (powermetrics). `N/A` when `MEMPLUS_POWER=0`.
  - `GPU%`: GPU HW active residency (powermetrics). `N/A` when `MEMPLUS_POWER=0`.
  - `Mem BW`: DRAM read+write bandwidth in GB/s, measured over `MEMPLUS_BW_INTERVAL` seconds (default **0.2**; see "Memory Bandwidth" below).
  - `Mem BW Max`: Decaying high-water mark for `Mem BW` — the highest value seen, but it *forgets* a peak that hasn't been matched or beaten for `MEMPLUS_BW_WINDOW_SEC` seconds (default 900). Persisted in `/tmp/mem-plus-membw-max`; delete that file to reset it. See "Mem BW Max — the Decaying Peak" below.
  - `Mem Used`: System-wide RAM used — the "Memory Used" label in Activity Monitor. That label is physical RAM minus file-backed pages minus empty free pages. It is larger than App + Wired + Compressed: volatile purgeable pages stay in the label, and so does RAM `vm_stat` never assigns to a bucket. Cached Files is file-backed plus purgeable, so it is not the term subtracted here.
  - `Mem Pressure`: `Normal`, `Warn`, or `Critical` — derived from `memorystatus_get_level()` free % (Activity Monitor's pressure graph).
  - `Mem Free%`: Percent of RAM available (same API as `memory_pressure(1)`'s "System-wide memory free percentage").

Note: the "Total" block is system-wide (a single sample), not per-process, so it repeats identically on every process line.

## Units (GB vs GiB)
Memory **sizes** (`Mem`, `Mem Peak`, `Mem Used`, `RSS`) are computed with **1024-based divisors** (MiB/GiB per IEC). They are labeled **MB** / **GB** anyway — the same convention Activity Monitor, `top`, `vmmap`, and `footprint` use — so you can compare numbers directly without converting.

| Field | Divisor | Label | Compare with |
|-------|---------|-------|--------------|
| `Mem`, `Mem Peak`, `Mem Used` | 1024³ | `G` / `GB` | Activity Monitor memory columns |
| `RSS` | 1024² | `MB` | `ps` / AM (same basis) |
| `Mem BW` | 10⁹ | `GB/s` | Throughput convention (`membw`, mactop); **not** 1024-based |

We keep **GB** rather than **GiB** in the output so `Mem Used: 8.61 GB` lines up with Activity Monitor's **8.61 GB** — same value, same label. Bandwidth is the one exception: `GB/s` there means decimal gigabytes per second.

## RSS vs Mem — Why They Disagree (and Which to Trust)
These two numbers measure different things, and different inference engines stress opposite ends of that difference:

- **RSS** (`ps -o rss`): Every physical RAM page mapped into the process, INCLUDING shared libraries and clean, file-backed pages (e.g. an mmap'd model file). EXCLUDES compressed/swapped memory.

- **Mem** / **Mem Peak**: Apple's "physical footprint" — the memory the process is actually CHARGED for (Activity Monitor's "Memory" column). Read via `memfoot` / `proc_pid_rusage`. EXCLUDES clean reclaimable file-backed pages; INCLUDES dirty pages, compressed memory, and IOKit/GPU (Metal) allocations.

Real data from this tool:

- llama-server → "Mem": "845.5M", "RSS": "3098.3 MB" (RSS >> Mem)
- mlx_lm python → "Mem": "7.4G", "RSS": "1789.4 MB" (Mem >> RSS)

Why each diverges:

* **llama.cpp** (llama-server) mmaps the GGUF model by default. Those model pages are clean and file-backed, so they count fully in RSS (they sit in RAM) but barely in physical footprint (the OS can drop them for free, so the process isn't billed). The gap IS the mmap'd weights.

* **MLX** (mlx_lm) uses Apple unified memory: tensors live in Metal/IOKit GPU buffers. Those allocations are charged to physical footprint (watch "Mem Peak" jump during inference) but the GPU-wired/IOKit memory is not counted in the traditional ps resident set, so RSS looks small. Compressed pages widen the gap further.

Takeaway: trust "Mem" (physical footprint) for real RAM cost on both engines. RSS over-reports for mmap-based loaders (llama.cpp) and under-reports for GPU/unified-memory engines (MLX). Seeing both side by side is a quick way to tell whether a process is mmap-heavy or GPU-allocation-heavy at a glance.

## Why CPU% Is Measured This Way (and Not With `ps`)
Earlier versions read `ps -o %cpu` (and `ps -o cpu`). Both are wrong for a live "what is it doing right now" snapshot:

- `ps -o %cpu`: A decaying average over up to a minute. On Darwin it behaves essentially as CPU-time / elapsed-time for long-lived processes. A server up for hours that is mostly idle with occasional bursts averages to ~0%. That is why CPU% reads 0.0% even while htop showed real load.

- `ps -o cpu`: "short-term CPU usage factor (for scheduling)" — the kernel's raw scheduler-decay counter (p_estcpu), NOT a percentage. It is near 0 almost always.

An accurate instantaneous %CPU requires sampling cumulative CPU time TWICE over an interval and dividing the delta by wall-clock time — exactly what htop/top do and what a single `ps` snapshot cannot. So this tool runs ONE `top -l 2 -s 1` for all matched PIDs at once and reads the SECOND sample (the first sample is cumulative; only the second is a true 1s delta).

Cost: This adds ~1 second of latency per invocation (top must wait one interval to measure a delta). That is the unavoidable price of a real CPU%.

## Physical Footprint (the `memfoot` helper)
`Mem` and `Mem Peak` are Apple's **physical footprint** — the memory a process is actually charged for (Activity Monitor's "Memory" column). mem-plus reads it via the bundled **`memfoot.c`** helper:
```bash
clang -O2 -o memfoot memfoot.c
```
`memfoot` calls `proc_pid_rusage(3)` (`RUSAGE_INFO_V4`) and prints `ri_phys_footprint` + `ri_lifetime_max_phys_footprint`. That is the same kernel ledger Activity Monitor and `top`'s MEM column use: one syscall per process, no VM region walk.

**Why not `vmmap`?** Earlier versions ran `vmmap --summary` per PID. Even the summary mode walks every mapped region via `task_read_for_pid` + `mach_vm_region_recurse` to recompute the same headline number. On processes with thousands of regions (mpv, llama-server) that sweep can hitch live playback for a fraction of a second — even though the displayed footprint is identical.

**When you still want `vmmap`:** the invasive sweep is for *debugging*, not monitoring. `vmmap` (or `footprint -v`) gives per-region addresses, mapped file paths, dirty/resident/swapped breakdown, and category totals (`MALLOC`, `VM_ALLOCATE`, `IOAccelerator`, etc.) — useful for finding leaks or understanding *where* memory lives. Use those tools manually; mem-plus sticks to the lightweight ledger read so it can run in a tight loop without stalling apps.

If the `memfoot` binary isn't built/present, `Mem` / `Mem Peak` read `N/A` and everything else still works.

## System Memory (the `memsys` helper)
Activity Monitor's Memory tab shows two headline figures mem-plus now mirrors in `Total`:

- **Memory Used** → `Mem Used` — `hw.memsize − external_page_count × page_size − (free_count − speculative_count) × page_size`.
- **Memory Pressure** → `Mem Pressure` + `Mem Free%` — from `memorystatus_get_level()` (the same call `memory_pressure(1)` uses). Pressure labels use Apple's documented thresholds: ≥ 60% free = Normal, ≥ 30% = Warn, else Critical. The `kern.memorystatus_vm_pressure_level` sysctl lags and is not used.

The bullet beside "Memory Used" looks like a sum of App Memory, Wired Memory, and Compressed. The label is a different total: physical RAM that is neither file-backed nor empty. `free_count` already includes speculative pages, so the empty term is `free_count − speculative_count`, the same quantity `vm_stat` prints as "Pages free". Cached Files is file-backed pages plus volatile purgeable pages, so those purgeable pages stay in the label. So does the part of `hw.memsize` that `vm_stat` never assigns to a bucket — about 0.5 GB on the 24 GB machine this was checked against. There the three lines added up to about 0.7 GB under the label (the purgeable slice, plus that unclassified 0.5 GB). `memsys` follows the label. Summing the three lines reports committed VM pages and leaves both of those out.

Build once:
```bash
clang -O2 -o memsys memsys.c
```

No `sudo` required. If the binary is missing, those three fields read `N/A`.

`Mem Used` is `%.2f GB` (one step is ~11 MB on a 16 KB page machine). It is **not** cached by the watch loop — each `mem-plus` spawns a fresh `memsys`. What *can* freeze it is XNU: `host_statistics64` is rate-limited for non-platform (adhoc-signed) binaries like `memsys`, and serves a **global 1-second snapshot** after a random 2–10 live queries in that window. `/usr/bin/vm_stat` is a platform binary and is not throttled. `Mem Pressure` / `Mem Free%` use `memorystatus_get_level()`, a different path, so they can still move while `Mem Used` is glued. See "Why Mem Used can look frozen" below.

## Memory Bandwidth (the `membw` helper)
powermetrics on Apple Silicon has **no** memory-bandwidth sampler (its samplers are tasks, battery, network, disk, interrupts, cpu_power, thermal, sfi, gpu_power, ane_power). Live DRAM GB/s lives only in the private **IOReport** framework.

Compile the helper once:
```bash
clang -O2 -framework CoreFoundation -o membw membw.c
```

`membw` `dlopen`s `libIOReport.dylib`, samples for `MEMPLUS_BW_INTERVAL` seconds (default **0.2**), and prints combined read+write GB/s. It does not call sudo. Two sources, tried in order:

| Chip | IOReport source | sudo |
|------|-----------------|------|
| **M4** | AMC Stats byte counters `DCS RD` / `DCS WR` | **no** |
| **M4 Pro, M4 Max** | PMP `DCS BW` / `AMCC RD+WR` rate histograms | **no** |

On a base M4 the AMC Stats group subscribes as a normal user, and `DCS RD` + `DCS WR` are live byte counters. Checked on a Mac mini (Mac16,10, macOS 27): desktop idle was a few GB/s, and an 8-thread copy held 105.8 GB/s, matching the `DCS` aggregate for the same window.

On M4 Pro the AMC Stats group will not subscribe (`IOReportCreateSubscription` returns NULL — ~190 channels including `DCS F1`–`F6` bins; the names `DCS RD`/`DCS WR` exist in the catalog but never appear in a sample delta). That is why `Mem BW` used to read `N/A`. PMP subscribes without root. Its AMCC histograms are 32 residency buckets labeled `16GB/s`…`512GB/s`. The first bucket is an underflow bin (everything below 16 GB/s, including true idle ~0.1 GB/s on an M4-base AMC reading); membw counts that bucket as 0 and takes a residency-weighted average of the rest, otherwise idle would be a phantom 16 GB/s. Traffic that stays entirely under 16 GB/s is therefore indistinguishable from idle.

`Mem BW Max` is a time-decaying peak of those samples (see below).

Why not shell out to mactop: a full mactop sample also opens the NVMe/disk IOKit user client and **locks out `smartctl`** for the duration. `membw` never opens a disk user client (verified: smartctl reads all disks while `membw` runs in a tight loop). On M1/M4, AMC byte counts track mactop's `dram_bw_combined_gbs` within a few percent (non-overlapping windows). On M4 Pro, mactop has the same AMC gap; the PMP histogram is the working path.

If the `membw` binary isn't built/present, `Mem BW` reads `N/A GB/s` and everything else still works.

## Mem BW Max — the Decaying Peak
`Mem BW Max` is a high-water mark with a **time-decay**, so a one-off spike from long ago doesn't dominate forever. It is the highest `Mem BW` observed, but it forgets a peak that has not been matched or beaten for `MEMPLUS_BW_WINDOW_SEC` seconds (default **900**):
```bash
MEMPLUS_BW_WINDOW_SEC=300 mem-plus llama-server   # 5-minute decay window
```

How it works — the state is a single number in `/tmp/mem-plus-membw-max`, and the file's **mtime marks when that peak was last set**. If that file exists but is not writable (a previous `sudo mem-plus` leaves it owned by root), the mark is kept in `/tmp/mem-plus-membw-max.<uid>` instead. Each run:
- file missing or older than the window → **reseed** the max to the current sample;
- current `Mem BW` > stored max → **new high**, rewrite (restarting the decay clock);
- otherwise → leave the file (and its mtime) **untouched**, so the decay clock keeps counting from the last peak.

Because mem-plus is short-lived and usually run on a short interval (e.g. `while :; do mem-plus … ; sleep 10; done`), this acts as a **quasi rolling window**: the displayed max tracks the highest value in roughly the trailing `MEMPLUS_BW_WINDOW_SEC` seconds — without storing any timestamped history.

Caveat: on decay it reseeds from the *current* sample, not the second-highest value still inside the window. So immediately after a peak expires the max can briefly under-report, then recover as fresh samples arrive. That is the deliberate trade for skipping all timestamp bookkeeping; it never over-reports. Delete the file to reset the mark immediately.

## Beautifying the Output (jq)
The raw output is intentionally one line per process. Pipe through jq to pretty-print:
```bash
mem-plus llama-server | jq .
```

## Watching / Repeating (no `watch` Needed)
A plain zsh loop re-runs it on an interval. Ctrl-C to stop:
```bash
while :; do date "+%Y%m%d-%H%M%S"; mem-plus "llama-server|python" | jq .; sleep 10; done
```

`sleep 10` is the recommended default for a **human-facing snapshot**, not a high-resolution profiler.

Each `mem-plus` already burns about **1.5–2 s** of wall time before the sleep (`powermetrics` 300 ms, `membw` 200 ms, `top -l 2 -s 1` a full second). So a `sleep 10` loop is really sampling about every **12 s**. That is well above XNU's **1 s** `host_statistics64` cache window, so `Mem Used` will not freeze because of that cache.

**10 s is a good fit for:**

- `Mem Used` / pressure / free % — system RAM moves slowly except at model load/unload. At `%.2f GB` you only see ~11 MB steps.
- Per-process `Mem` / RSS once models are resident.
- A terminal you glance at — faster just fills the screen with another wall of `jq`.

**10 s is a poor fit for:**

- `Mem BW` — each sample is only a **0.2 s** window, then you wait 10 s. Prefill spikes are easy to miss. `Mem BW Max` exists for that, but with this duty cycle it is a lottery, not a peak meter. Keep the loop interval well under `MEMPLUS_BW_WINDOW_SEC` (default 900) so a *busy* workload still has a chance to refresh the mark before it decays.
- `CPU%` / `GPU%` / power — one short snapshot per cycle. A decode that lasts a few seconds can land entirely between samples.

**Other intervals:** drop to **2–3 s** only if you are watching a live generation and care about GPU%/BW. Go to **30–60 s** if it is just a babysitter. Do not go under ~2 s unless you drop `top`/`powermetrics`; you would spend more time sampling than watching, and you would start colliding with the 1 s kernel cache.

### Why `Mem Used` can look frozen in a watch loop
A `while` loop does **not** hold `Mem Used`. Each iteration is a new `mem-plus` → new `memsys` process. There is no loop-local cache for that field (the only `/tmp` state is `Mem BW Max`). Killing the loop and starting another one does not flush a bash cache; it just takes a new kernel sample after a pause.

`Mem Used` comes from `memsys` via `hw.memsize` and `host_statistics64(HOST_VM_INFO64)` (physical − file-backed − empty free). XNU rate-limits that statistics call for **non-platform** binaries and serves a **global** snapshot: 1-second window, a random 2–10 live replies, then cached copies for the rest of the window. `memsys` is adhoc linker-signed, so it is in that bucket. `/usr/bin/vm_stat` is a platform binary and is not.

Measured on macOS 27 (xnu-13432): a tight burst of 40 `host_statistics64` calls (or 25 new `memsys` processes) all printed the same `used_pages`; spacing of 1–2 s tracked an 800 MB allocation (`33.13 → 33.52 → 33.81` GB). A `sleep 10` loop should therefore get a **live** sample every time. The cache can make `Mem Used` look glued only if something calls `host_statistics64` more than a few times **within the same second** (a tight loop, two overlapping `mem-plus`es, a Python `psutil` poller). It cannot hold one value across many 10-second iterations.

If a 10 s loop *looks* stuck, it is almost always one of these:

1. **The rounded number really did not move.** Dual-serve idle can sit on the same hundredths digit for a long time. Per-process `Mem` (Metal / `phys_footprint` via `memfoot`) can still change. `Mem Used` will not, until file-backed or empty-free pages move by ~11 MB.
2. **A wall of identical `Total` blocks.** `pgrep -f` is a regex on the full command line. A pattern like `llama-server|python` matches the servers, **and** `mem-plus` itself (the pattern is on its argv), `sudo mem-plus …`, and a parent shell whose command line contains that string. Every JSON line repeats the same `Total.Mem Used`. Easy to eye-lock on an old block; a new loop puts a fresh block at the cursor.
3. **A hung iteration, not a stale sampler.** If you print `date` *before* `mem-plus` and that `sudo mem-plus` blocks (sudo password after the 5-minute timestamp, `powermetrics`, or `top -l 2 -s 1`), you get a new timestamp and the previous jq blob still on screen. Ctrl-C kills the stuck child; the next run completes and looks like a refresh. Check stderr: you should see `Sampling power metrics...` finish with `Done` each cycle.

**Diagnostic:** `Mem Free%` / `Mem Pressure` come from `memorystatus_get_level()`, which is **not** the `host_statistics64` cache. If those two were moving while `Mem Used` was glued, that is the 1 s kernel cache. If the date froze too, `mem-plus` never finished. Compare `memsys` to `vm_stat` on the same tick (`hw.memsize` minus file-backed pages minus "Pages free"): if `vm_stat` moved and `memsys` did not, it is the rate-limit cache.

## Requirements / Notes
- macOS on Apple Silicon (uses `memfoot`, `memsys`, powermetrics, ps -o comm/rss, and IOReport for bandwidth).
- powermetrics needs sudo; it is called once per invocation unless `MEMPLUS_POWER=0`. You may be prompted for your password (or configure passwordless sudo for it). With `MEMPLUS_POWER=0`, CPU Power, GPU Power, and GPU% read `N/A` and there is no password prompt.
- `Mem` / `Mem Peak` require the bundled `memfoot` helper (`clang -O2 -o memfoot memfoot.c`). It reads `phys_footprint` via `proc_pid_rusage` and does not walk VM regions like `vmmap`, so it won't hitch live apps.
- `Mem Used` / `Mem Pressure` / `Mem Free%` require `memsys` (`clang -O2 -o memsys memsys.c`).
- The `Mem BW` fields require the bundled `membw` helper (`clang -O2 -framework CoreFoundation -o membw membw.c`). No sudo: base M4 uses AMC Stats byte counters, M4 Pro / M4 Max use PMP histograms. Compiled binaries are gitignored — only the `.c` sources are tracked. If missing, those fields read `N/A`.
- jq is only needed if you want pretty output; the tool itself emits valid JSON without it.
