# mem-plus

*Compact, one-line JSON memory/CPU/power snapshot for processes.*

A macOS utility that prints a compact, machine-readable snapshot of memory, CPU, and power usage for running processes — handy for watching local inference servers like llama.cpp and MLX.

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
    "GPU%": "62.3%"
  }
}
```

## Fields Explanation
- `<comm>`: Last 80 chars of the process command path (the JSON key).
- `PID`: Process id.
- `Mem`: Physical footprint (vmmap). What Activity Monitor calls "Memory". The number you should trust for real RAM cost.
- `Mem Peak`: Peak physical footprint since the process started.
- `RSS`: Resident Set Size (ps -o rss), in MB. A cross-check metric; see "RSS vs Mem" below for why it can differ wildly.
- `CPU%`: Instantaneous %CPU, htop-style. Measured with `top -l 2 -s 1` and reading the SECOND sample, which is a delta over a 1s interval. Sums across cores, so a process pinning N threads can exceed 100%. (See "Why CPU% Is Measured This Way" below.)
- `NCPU%`: CPU% normalized by logical core count (hw.ncpu): CPU% / ncpu. Caps at ~100% = the process is using the entire machine.
- `Total`: System-wide power, sampled ONCE per invocation:
  - `CPU Power`: Package CPU power draw (powermetrics).
  - `GPU Power`: GPU power draw (powermetrics).
  - `GPU%`: GPU HW active residency (powermetrics).

Note: the "Total" power block is system-wide (a single powermetrics sample), not per-process, so it repeats identically on every process line.

## RSS vs Mem — Why They Disagree (and Which to Trust)
These two numbers measure different things, and different inference engines stress opposite ends of that difference:

- **RSS** (`ps -o rss`): Every physical RAM page mapped into the process, INCLUDING shared libraries and clean, file-backed pages (e.g. an mmap'd model file). EXCLUDES compressed/swapped memory.

- **Mem** / **Mem Peak**: Apple's "physical footprint" — the memory the process is actually CHARGED for (Activity Monitor's "Memory" column). EXCLUDES clean reclaimable file-backed pages; INCLUDES dirty pages, compressed memory, and IOKit/GPU (Metal) allocations.

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

## Requirements / Notes
- macOS (uses vmmap, powermetrics, ps -o comm/rss).
- powermetrics needs sudo; it is called once per invocation. You may be prompted for your password (or configure passwordless sudo for it).
- jq is only needed if you want pretty output; the tool itself emits valid JSON without it.