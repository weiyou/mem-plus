// =============================================================================
// memfoot - print physical footprint + peak for a process (Activity Monitor)
// =============================================================================
//
// Reads the kernel-maintained phys_footprint ledger via proc_pid_rusage(3).
// This is the same source Activity Monitor and top(1) use for the "Memory"
// column — one syscall, no VM region walk — so it does not hitch live apps the
// way vmmap(1) does on processes with thousands of mapped regions.
//
// vmmap walks every region with task_read_for_pid + mach_vm_region_recurse to
// recompute the same headline numbers (and much more detail). Use vmmap when
// you need a per-region breakdown; use memfoot for monitoring snapshots.
//
// Build:
//     clang -O2 -o memfoot memfoot.c
//
// Units: 1024-based (KiB/MiB/GiB), labeled KB/MB/GB like Activity Monitor.
//
// Usage:
//     memfoot <pid>     # prints: "<footprint> <peak>"  (e.g. "729.0 MB 1.1 GB")
// =============================================================================

#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

static void format_bytes(uint64_t bytes, char *buf, size_t len) {
    const double gb = 1024.0 * 1024.0 * 1024.0;
    const double mb = 1024.0 * 1024.0;

    if (bytes >= (uint64_t)gb)
        snprintf(buf, len, "%.1f GB", (double)bytes / gb);
    else if (bytes >= (uint64_t)mb)
        snprintf(buf, len, "%.1f MB", (double)bytes / mb);
    else
        snprintf(buf, len, "%.1f KB", (double)bytes / 1024.0);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: memfoot <pid>\n");
        return 1;
    }

    pid_t pid = (pid_t)strtol(argv[1], NULL, 10);
    if (pid <= 0) {
        fprintf(stderr, "memfoot: invalid pid\n");
        return 1;
    }

    struct rusage_info_v4 ru;
    if (proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&ru) < 0) {
        fprintf(stderr, "memfoot: proc_pid_rusage failed (pid %d)\n", (int)pid);
        return 1;
    }

    char footprint[32], peak[32];
    format_bytes(ru.ri_phys_footprint, footprint, sizeof(footprint));
    format_bytes(ru.ri_lifetime_max_phys_footprint, peak, sizeof(peak));
    printf("%s %s\n", footprint, peak);
    return 0;
}