// =============================================================================
// memgpu - GPU power (mW) and HW active residency (%) from IOReport
// =============================================================================
//
// powermetrics reports both, and it needs sudo. The CPU package counter behind
// its "CPU Power" line (Energy Model "CPU Energy" / "ECPU" / "PCPU", unit mJ)
// is visible to a normal user but does not advance, so an unsudoed delta is
// a hard 0 W while the P-cores are busy. This helper does not print CPU power.
//
// Two GPU signals do advance without root. Checked on a base M4 Mac mini
// (Mac16,10, macOS 27):
//
//   GPU Power  Energy Model channel "GPU Energy", unit nJ.
//              Watts = delta / window. Idle ~0.1 W; a Metal compute burn
//              read ~13 W, matching mactop's gpu_power on the same load.
//              The sibling channel "GPU" (mJ) stays frozen on that OS, which
//              is the counter powermetrics has historically labeled GPU Power.
//              If "GPU Energy" does not move and "GPU" does, the mJ channel
//              is used instead. "GPU SRAM" is never included.
//
//   GPU%       GPU Stats subgroup "GPU Performance States", channel "GPUPH".
//              Active residency = time outside OFF / IDLE / DOWN, over total
//              residency. That is powermetrics' "GPU HW active residency"
//              and mactop's gpu_active. The boost-controller channel
//              BSTGPUPH is a different residency and is ignored (it can read
//              100% while GPUPH is mostly OFF).
//
// This helper never opens an NVMe/disk IOKit user client, so it cannot lock
// out smartctl the way a full mactop sample can.
//
// IOReport is a private framework with no on-disk SDK stub, so symbols are
// resolved with dlopen()/dlsym().
//
// Build:
//     clang -O2 -Wall -framework CoreFoundation -o memgpu memgpu.c
//
// Usage:
//     ./memgpu [interval_seconds]     # default 0.3; no sudo
// Prints one line: "<milliwatts> <percent>"  (percent is "NA" if GPUPH is
// absent). Exit status is 0 when at least one value was produced.
// =============================================================================

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct IOReportSubscription *IOReportSubscriptionRef;

typedef CFMutableDictionaryRef (*copy_chans_t)(CFStringRef, CFStringRef,
                                               uint64_t, uint64_t, uint64_t);
typedef void (*merge_chans_t)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
typedef IOReportSubscriptionRef (*create_sub_t)(void *, CFMutableDictionaryRef,
                                                CFMutableDictionaryRef *,
                                                uint64_t, CFTypeRef);
typedef CFDictionaryRef (*create_samples_t)(IOReportSubscriptionRef,
                                            CFMutableDictionaryRef, CFTypeRef);
typedef CFDictionaryRef (*samples_delta_t)(CFDictionaryRef, CFDictionaryRef,
                                           CFTypeRef);
typedef CFStringRef (*chan_str_t)(CFDictionaryRef);
typedef int64_t (*simple_int_t)(CFDictionaryRef, int32_t);
typedef int32_t (*state_count_t)(CFDictionaryRef);
typedef CFStringRef (*state_name_t)(CFDictionaryRef, int32_t);
typedef int64_t (*state_res_t)(CFDictionaryRef, int32_t);

static copy_chans_t IOReportCopyChannelsInGroup;
static merge_chans_t IOReportMergeChannels;
static create_sub_t IOReportCreateSubscription;
static create_samples_t IOReportCreateSamples;
static samples_delta_t IOReportCreateSamplesDelta;
static chan_str_t IOReportChannelGetSubGroup;
static chan_str_t IOReportChannelGetChannelName;
static chan_str_t IOReportChannelGetUnitLabel;
static simple_int_t IOReportSimpleGetIntegerValue;
static state_count_t IOReportStateGetCount;
static state_name_t IOReportStateGetNameForIndex;
static state_res_t IOReportStateGetResidency;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int cf_to_buf(CFStringRef s, char *buf, size_t n) {
    buf[0] = 0;
    if (!s)
        return 0;
    return CFStringGetCString(s, buf, (CFIndex)n, kCFStringEncodingUTF8);
}

static int valid_counter(int64_t v) { return v >= 0; }

// Joules per one count. mactop treats a missing/unknown unit as microjoules.
static double joules_per_count(const char *unit) {
    char u[32];
    snprintf(u, sizeof(u), "%s", unit ? unit : "");
    for (int i = 0; u[i]; i++) {
        if (u[i] == ' ') {
            u[i] = 0;
            break;
        }
    }
    if (strcmp(u, "mJ") == 0)
        return 1e-3;
    if (strcmp(u, "uJ") == 0)
        return 1e-6;
    if (strcmp(u, "nJ") == 0)
        return 1e-9;
    return 1e-6;
}

static int is_gpu_idle_state(const char *sn) {
    return strcmp(sn, "OFF") == 0 || strcmp(sn, "IDLE") == 0 ||
           strcmp(sn, "DOWN") == 0;
}

static int load_ioreport(void) {
    void *lib = dlopen("/usr/lib/libIOReport.dylib", RTLD_LAZY);
    if (!lib)
        lib = dlopen("libIOReport.dylib", RTLD_LAZY);
    if (!lib)
        lib = dlopen(
            "/System/Library/PrivateFrameworks/IOReport.framework/IOReport",
            RTLD_LAZY);
    if (!lib) {
        fprintf(stderr, "memgpu: cannot load IOReport: %s\n", dlerror());
        return 0;
    }
#define LOAD(name)                                                             \
    do {                                                                       \
        name = (typeof(name))dlsym(lib, #name);                               \
        if (!name) {                                                           \
            fprintf(stderr, "memgpu: missing %s\n", #name);                    \
            return 0;                                                          \
        }                                                                      \
    } while (0)
    LOAD(IOReportCopyChannelsInGroup);
    LOAD(IOReportMergeChannels);
    LOAD(IOReportCreateSubscription);
    LOAD(IOReportCreateSamples);
    LOAD(IOReportCreateSamplesDelta);
    LOAD(IOReportChannelGetSubGroup);
    LOAD(IOReportChannelGetChannelName);
    LOAD(IOReportChannelGetUnitLabel);
    LOAD(IOReportSimpleGetIntegerValue);
    LOAD(IOReportStateGetCount);
    LOAD(IOReportStateGetNameForIndex);
    LOAD(IOReportStateGetResidency);
#undef LOAD
    return 1;
}

// Keep "GPU Energy", the bare "GPU" alias, and GPUPH. Drop the hundreds of
// per-core / DTL energy channels and the boost-controller residency.
static CFMutableDictionaryRef keep_gpu_channels(CFDictionaryRef group) {
    CFArrayRef arr = group ? CFDictionaryGetValue(group, CFSTR("IOReportChannels"))
                           : NULL;
    if (!arr)
        return NULL;
    CFMutableArrayRef keep =
        CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    if (!keep)
        return NULL;
    CFIndex n = CFArrayGetCount(arr);
    for (CFIndex i = 0; i < n; i++) {
        CFDictionaryRef ch = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, i);
        char sub[96] = {0}, name[128] = {0};
        cf_to_buf(IOReportChannelGetSubGroup(ch), sub, sizeof(sub));
        cf_to_buf(IOReportChannelGetChannelName(ch), name, sizeof(name));
        int energy = strcmp(name, "GPU Energy") == 0 || strcmp(name, "GPU") == 0;
        int perf = strcmp(sub, "GPU Performance States") == 0 &&
                   strcmp(name, "GPUPH") == 0;
        if (energy || perf)
            CFArrayAppendValue(keep, ch);
    }
    if (CFArrayGetCount(keep) == 0) {
        CFRelease(keep);
        return NULL;
    }
    CFMutableDictionaryRef out =
        CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, group);
    if (!out) {
        CFRelease(keep);
        return NULL;
    }
    CFDictionarySetValue(out, CFSTR("IOReportChannels"), keep);
    CFRelease(keep);
    return out;
}

int main(int argc, char **argv) {
    double interval = (argc > 1) ? atof(argv[1]) : 0.3;
    if (interval <= 0)
        interval = 0.3;
    if (!load_ioreport())
        return 1;

    CFMutableDictionaryRef energy =
        IOReportCopyChannelsInGroup(CFSTR("Energy Model"), NULL, 0, 0, 0);
    CFMutableDictionaryRef gpu =
        IOReportCopyChannelsInGroup(CFSTR("GPU Stats"), NULL, 0, 0, 0);
    CFMutableDictionaryRef energy_keep = keep_gpu_channels(energy);
    CFMutableDictionaryRef gpu_keep = keep_gpu_channels(gpu);
    if (energy)
        CFRelease(energy);
    if (gpu)
        CFRelease(gpu);

    CFMutableDictionaryRef chans = energy_keep ? energy_keep : gpu_keep;
    if (energy_keep && gpu_keep)
        IOReportMergeChannels(energy_keep, gpu_keep, NULL);
    if (!chans) {
        fprintf(stderr, "memgpu: no GPU Energy or GPUPH channel\n");
        return 1;
    }

    CFMutableDictionaryRef subbed = NULL;
    IOReportSubscriptionRef sub =
        IOReportCreateSubscription(NULL, chans, &subbed, 0, NULL);
    CFMutableDictionaryRef sample_chans = subbed ? subbed : chans;
    if (!sub) {
        fprintf(stderr, "memgpu: IOReportCreateSubscription failed\n");
        CFRelease(chans);
        return 1;
    }

    double t0 = now_sec();
    CFDictionaryRef s1 = IOReportCreateSamples(sub, sample_chans, NULL);
    struct timespec req;
    req.tv_sec = (time_t)interval;
    req.tv_nsec = (long)((interval - (double)req.tv_sec) * 1e9);
    nanosleep(&req, NULL);
    CFDictionaryRef s2 = IOReportCreateSamples(sub, sample_chans, NULL);
    double dt = now_sec() - t0;
    CFDictionaryRef delta = NULL;
    if (s1 && s2 && dt > 0)
        delta = IOReportCreateSamplesDelta(s1, s2, NULL);
    if (s1)
        CFRelease(s1);
    if (s2)
        CFRelease(s2);
    if (!delta) {
        fprintf(stderr, "memgpu: no sample delta\n");
        CFRelease(chans);
        return 1;
    }

    CFArrayRef arr = CFDictionaryGetValue(delta, CFSTR("IOReportChannels"));
    CFIndex n = arr ? CFArrayGetCount(arr) : 0;
    double named_w = 0, alias_w = 0;
    int have_named = 0, have_alias = 0;
    int have_pct = 0;
    double pct = 0;

    for (CFIndex i = 0; i < n; i++) {
        CFDictionaryRef ch = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, i);
        char subg[96] = {0}, name[128] = {0}, unit[32] = {0};
        cf_to_buf(IOReportChannelGetSubGroup(ch), subg, sizeof(subg));
        cf_to_buf(IOReportChannelGetChannelName(ch), name, sizeof(name));
        cf_to_buf(IOReportChannelGetUnitLabel(ch), unit, sizeof(unit));

        if (strcmp(name, "GPU Energy") == 0 || strcmp(name, "GPU") == 0) {
            int64_t raw = IOReportSimpleGetIntegerValue(ch, 0);
            if (!valid_counter(raw))
                continue;
            double watts = ((double)raw * joules_per_count(unit)) / dt;
            if (strcmp(name, "GPU Energy") == 0) {
                named_w += watts;
                have_named = 1;
            } else {
                alias_w += watts;
                have_alias = 1;
            }
        }

        if (strcmp(subg, "GPU Performance States") == 0 &&
            strcmp(name, "GPUPH") == 0 && IOReportStateGetCount) {
            int32_t sc = IOReportStateGetCount(ch);
            int64_t total = 0, active = 0;
            for (int32_t s = 0; s < sc; s++) {
                int64_t r = IOReportStateGetResidency(ch, s);
                if (!valid_counter(r))
                    continue;
                total += r;
                char sn[64] = {0};
                cf_to_buf(IOReportStateGetNameForIndex(ch, s), sn, sizeof(sn));
                if (sn[0] && !is_gpu_idle_state(sn))
                    active += r;
            }
            if (total > 0) {
                pct = 100.0 * (double)active / (double)total;
                have_pct = 1;
            }
        }
    }
    CFRelease(delta);
    if (energy_keep)
        CFRelease(energy_keep);
    if (gpu_keep)
        CFRelease(gpu_keep);

    if (!have_named && !have_alias && !have_pct) {
        fprintf(stderr, "memgpu: GPU channels missing from delta\n");
        return 1;
    }

    // "GPU Energy" is the live channel on macOS 27. The mJ "GPU" alias is
    // only used when the named channel did not move.
    double watts = (have_named && named_w > 0) ? named_w : alias_w;
    long mw = (long)llround(watts * 1000.0);
    if (mw < 0)
        mw = 0;
    if (have_pct)
        printf("%ld %.1f\n", mw, pct);
    else
        printf("%ld NA\n", mw);
    return 0;
}
