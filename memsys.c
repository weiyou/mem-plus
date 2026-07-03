// =============================================================================
// memsys - system memory used + pressure (Activity Monitor Memory tab)
// =============================================================================
//
// Prints three fields on one line:
//     <memory_used_GB> <pressure_label> <free_percent>
// e.g. "23.21 Warn 3.3"
//
// Memory Used matches Activity Monitor's bottom-line "Memory Used":
//     (hw.memsize / page_size - free_count) * page_size
// i.e. physical RAM minus free pages. Cached file memory still counts as
// "used" in this headline figure until reclaimed.
//
// Memory Pressure comes from sysctl kern.memorystatus_vm_pressure_level
// (same source Activity Monitor's Memory Pressure graph uses):
//     0 = Normal, 1 = Warn, 2 = Critical
//
// Free % is 100 * free_count / total_pages (same basis as memory_pressure(1)).
//
// Build:
//     clang -O2 -o memsys memsys.c
//
// Usage:
//     memsys
// =============================================================================

#include <mach/mach.h>
#include <mach/mach_host.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/sysctl.h>
#include <unistd.h>

static const char *pressure_label(uint32_t level) {
    switch (level) {
    case 0: return "Normal";
    case 1: return "Warn";
    case 2: return "Critical";
    default: return "Unknown";
    }
}

int main(void) {
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    struct vm_statistics64 vm;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &count) != KERN_SUCCESS) {
        fprintf(stderr, "memsys: host_statistics64 failed\n");
        return 1;
    }

    uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    uint64_t memsize = 0;
    size_t memsize_sz = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &memsize_sz, NULL, 0) != 0) {
        fprintf(stderr, "memsys: hw.memsize sysctl failed\n");
        return 1;
    }

    uint64_t total_pages = memsize / page;
    if (total_pages == 0) {
        fprintf(stderr, "memsys: invalid page count\n");
        return 1;
    }

    uint64_t used_bytes = (total_pages - vm.free_count) * page;
    double used_gb = (double)used_bytes / (1024.0 * 1024.0 * 1024.0);
    double free_pct = 100.0 * (double)vm.free_count / (double)total_pages;

    uint32_t pressure = 0;
    size_t pressure_sz = sizeof(pressure);
    if (sysctlbyname("kern.memorystatus_vm_pressure_level",
                     &pressure, &pressure_sz, NULL, 0) != 0) {
        pressure = 999;
    }

    printf("%.2f %s %.1f\n", used_gb, pressure_label(pressure), free_pct);
    return 0;
}