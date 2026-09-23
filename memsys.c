// =============================================================================
// memsys - system memory used + pressure (Activity Monitor Memory tab)
// =============================================================================
//
// Prints three fields on one line:
//     <memory_used_GB> <pressure_label> <free_percent>
// e.g. "8.61 Normal 80.0"
//
// Memory Used matches the "Memory Used" label in Activity Monitor, not the sum
// of the three lines under it. Those lines are App + Wired + Compressed, and
// App Memory has volatile purgeable pages removed. The label is larger:
//
//     hw.memsize - external_page_count * page_size
//                - (free_count - speculative_count) * page_size
//
// external_page_count is file-backed pages. Cached Files is that plus purgeable,
// so subtracting Cached Files would drop purgeable pages the label still counts.
// free_count already includes speculative pages; vm_stat's "Pages free" does not.
// The result equals App + Wired + Compressed + purgeable + the physical pages
// vm_stat never assigns to a bucket (firmware and other carveouts).
//
// Free % comes from memorystatus_get_level() — the same API memory_pressure(1)
// uses for "System-wide memory free percentage" (NOT raw free_count/total_pages).
//
// Memory Pressure is derived from that percent using the same thresholds Apple
// documents for normal/warn/critical in memory_pressure(1):
//     >= 60% free -> Normal, >= 30% -> Warn, else Critical.
// Do NOT use kern.memorystatus_vm_pressure_level — it lags behind the graph.
//
// host_statistics64 is rate-limited for non-platform (adhoc-signed) binaries:
// XNU serves a global 1 s snapshot after a random 2–10 live queries in that
// window. memsys is a new process each call, so a watch loop with sleep >= 2 s
// gets live data. A tight burst of memsys processes will all print the same
// Mem Used. /usr/bin/vm_stat is a platform binary and is not rate-limited.
// memorystatus_get_level() (Free% / Pressure) is a different path and is not
// covered by that cache.
//
// Build:
//     clang -O2 -o memsys memsys.c
//
// Units: Mem Used is 1024-based (GiB), labeled GB like Activity Monitor.
//
// Usage:
//     memsys
// =============================================================================

#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_types.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern int memorystatus_get_level(user_addr_t level);

static const char *pressure_from_percent(unsigned int pct) {
    if (pct >= 60) return "Normal";
    if (pct >= 30) return "Warn";
    return "Critical";
}

int main(void) {
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    struct vm_statistics64 vm;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &count) != KERN_SUCCESS) {
        fprintf(stderr, "memsys: host_statistics64 failed\n");
        return 1;
    }

    unsigned int free_pct = 0;
    if (memorystatus_get_level((user_addr_t)&free_pct) != 0) {
        fprintf(stderr, "memsys: memorystatus_get_level failed\n");
        return 1;
    }

    uint64_t memsize = 0;
    size_t memsize_len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &memsize_len, NULL, 0) != 0 ||
        memsize == 0) {
        fprintf(stderr, "memsys: hw.memsize failed\n");
        return 1;
    }

    uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    /* vm.free_count includes speculative pages. Activity Monitor's empty-free
       term, and vm_stat's "Pages free", are free_count - speculative_count. */
    uint64_t free_pages = vm.free_count;
    if (vm.speculative_count < free_pages)
        free_pages -= vm.speculative_count;
    else
        free_pages = 0;
    uint64_t aside = ((uint64_t)vm.external_page_count + free_pages) * page;
    uint64_t used = (memsize > aside) ? memsize - aside : 0;
    double used_gb = (double)used / (1024.0 * 1024.0 * 1024.0);

    printf("%.2f %s %.1f\n", used_gb, pressure_from_percent(free_pct),
           (double)free_pct);
    return 0;
}