# mem-plus

*Compact, one-line JSON memory/CPU/power snapshot for processes.*

A macOS utility that prints a compact, machine-readable snapshot of memory, CPU, and power usage for running processes — handy for watching local inference servers like llama.cpp and MLX.

## Setup
The `mem-plus` script runs as-is. Compile the bundled helpers once:
```bash
clang -O2 -o memfoot memfoot.c
clang -O2 -framework CoreFoundation -o membw membw.c
```
Without `memfoot`, `Mem` / `Mem Peak` read `N/A`. Without `membw`, `Mem BW` reads `N/A GB/s`. `memfoot` uses `proc_pid_rusage` (same source as Activity Monitor, no VM region walk). `membw` and powermetrics need `sudo`.

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
    "Mem BW Max": "42.1 GB/s"
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
  - `CPU Power`: Package CPU power draw (powermetrics).
  - `GPU Power`: GPU power draw (powermetrics).
  - `GPU%`: GPU HW active residency (powermetrics).
  - `Mem BW`: DRAM read+write bandwidth in GB/s, measured over a 1s interval (`membw` helper — see "Memory Bandwidth" below).
  - `Mem BW Max`: Decaying high-water mark for `Mem BW` — the highest value seen, but it *forgets* a peak that hasn't been matched or beaten for `MEMPLUS_BW_WINDOW_SEC` seconds (default 900). Persisted in `/tmp/mem-plus-membw-max`; delete that file to reset it. See "Mem BW Max — the Decaying Peak" below.

Note: the "Total" block is system-wide (a single sample), not per-process, so it repeats identically on every process line.

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

## Memory Bandwidth (the `membw` helper)
powermetrics on Apple Silicon exposes **no** memory-bandwidth sampler (its samplers are only tasks, battery, network, disk, interrupts, cpu_power, thermal, sfi, gpu_power, ane_power). DRAM bandwidth lives only in the private **IOReport** framework — the same source asitop/macmon/mactop read.

So mem-plus ships a tiny companion, **`membw.c`**, that you compile once:
```bash
clang -O2 -framework CoreFoundation -o membw membw.c
```
It `dlopen`s `libIOReport.dylib` and reads **only** the aggregate AMC `DCS RD` / `DCS WR` byte counters over a 1-second interval, prints combined read+write GB/s, and exits. mem-plus calls it (via `sudo`) once per invocation to fill `Mem BW`; `Mem BW Max` is then a time-decaying peak derived from it (see "Mem BW Max — the Decaying Peak" below).

Why a bespoke helper instead of shelling out to mactop: a full mactop sample also opens the NVMe/disk IOKit user client, which holds it exclusively and **locks out `smartctl`** for the duration of the sample. `membw` touches *only* the memory-controller counters and never opens a disk user client, so it does not interfere with `smartctl` (verified: smartctl reads all disks fine while `membw` runs in a tight loop).

Accuracy tracks mactop's `dram_bw_combined_gbs` within a few percent under load (no scale factor applied; small gaps are just non-overlapping sample windows).

If the `membw` binary isn't built/present, `Mem BW` reads `N/A GB/s` and everything else still works.

## Mem BW Max — the Decaying Peak
`Mem BW Max` is a high-water mark with a **time-decay**, so a one-off spike from long ago doesn't dominate forever. It is the highest `Mem BW` observed, but it forgets a peak that has not been matched or beaten for `MEMPLUS_BW_WINDOW_SEC` seconds (default **900**):
```bash
MEMPLUS_BW_WINDOW_SEC=300 mem-plus llama-server   # 5-minute decay window
```

How it works — the state is a single number in `/tmp/mem-plus-membw-max`, and the file's **mtime marks when that peak was last set**. Each run:
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
while :; do mem-plus "llama-server|python" | jq .; sleep 10; done
```
Running it on a short interval like this is also what makes `Mem BW Max` behave as a rolling peak — keep the loop interval well under `MEMPLUS_BW_WINDOW_SEC` so a busy workload keeps refreshing the mark before it decays.

## Requirements / Notes
- macOS on Apple Silicon (uses `memfoot`, powermetrics, ps -o comm/rss, and IOReport for bandwidth).
- powermetrics needs sudo; it is called once per invocation. You may be prompted for your password (or configure passwordless sudo for it).
- `Mem` / `Mem Peak` require the bundled `memfoot` helper (`clang -O2 -o memfoot memfoot.c`). It reads `phys_footprint` via `proc_pid_rusage` and does not walk VM regions like `vmmap`, so it won't hitch live apps.
- The `Mem BW` fields require the bundled `membw` helper, compiled once with `clang -O2 -framework CoreFoundation -o membw membw.c`. It also needs sudo (IOReport access). The compiled binaries are gitignored — only the `.c` sources are tracked. If missing, those fields read `N/A`.
- jq is only needed if you want pretty output; the tool itself emits valid JSON without it.