// =============================================================================
// memsys - system memory used + pressure (Activity Monitor Memory tab)
// =============================================================================
//
// Prints three fields on one line:
//     <memory_used_GB> <pressure_label> <free_percent>
// e.g. "8.61 Normal 80.0"
//
// Memory Used matches Activity Monitor's "Memory Used" (App + Wired + Compressed):
//     (wire_count + (internal_page_count - purgeable_count) + compressor_page_count)
//         * page_size
// App Memory is anonymous pages minus volatile purgeable pages. Activity Monitor
// counts those purgeable pages with Cached Files, not Memory Used. wire_count
// is Wired, compressor_page_count is Compressed. File-backed pages
// (external_page_count) stay out — AM lists them as Cached Files.
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

    uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    /* Volatile purgeable pages sit inside internal_page_count. AM moves them
       into Cached Files, so Memory Used must not include them. */
    uint64_t app_pages = vm.internal_page_count;
    if (vm.purgeable_count < app_pages)
        app_pages -= vm.purgeable_count;
    else
        app_pages = 0;
    uint64_t used_pages = vm.wire_count + app_pages + vm.compressor_page_count;
    double used_gb = (double)(used_pages * page) / (1024.0 * 1024.0 * 1024.0);

    printf("%.2f %s %.1f\n", used_gb, pressure_from_percent(free_pct),
           (double)free_pct);
    return 0;
}