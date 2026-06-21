// =============================================================================
// membw - print system-wide DRAM bandwidth (GB/s) from IOReport
// =============================================================================
//
// Apple Silicon exposes no memory-bandwidth sampler via powermetrics. The only
// source is the private IOReport framework, which carries per-agent and
// aggregate DRAM traffic counters. This helper reads ONLY the two aggregate
// memory-controller channels:
//
//     group "AMC Stats", subgroup "Perf Counters", channel "DCS RD"  (bytes)
//     group "AMC Stats", subgroup "Perf Counters", channel "DCS WR"  (bytes)
//
// It subscribes to nothing else. In particular it never opens an NVMe/disk
// IOKit user client, so (unlike a full mactop sample) it cannot lock out
// `smartctl`'s access to the SMART user client.
//
// Method: take a counter sample, sleep <interval> seconds, take a second
// sample, diff them with IOReportCreateSamplesDelta, sum DCS RD + DCS WR bytes,
// and divide by the measured wall-clock delta. Prints combined read+write GB/s
// as a single number (e.g. "2.34") on stdout.
//
// IOReport is a PRIVATE framework with no on-disk .framework or SDK stub to
// link against (it lives only in the dyld shared cache), so we resolve its
// symbols at runtime with dlopen()/dlsym() instead of -framework IOReport.
//
// Requires root (IOReport AMC channels are privileged) — run via sudo.
//
// Build:
//     clang -O2 -Wall -framework CoreFoundation -o membw membw.c
//
// Usage:
//     sudo ./membw [interval_seconds]      # default interval 1.0
// =============================================================================

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// --- Private IOReport API signatures (resolved via dlsym at runtime) ---------
typedef struct IOReportSubscription *IOReportSubscriptionRef;

typedef CFMutableDictionaryRef (*copy_chans_t)(CFStringRef, CFStringRef,
                                               uint64_t, uint64_t, uint64_t);
typedef IOReportSubscriptionRef (*create_sub_t)(void *, CFMutableDictionaryRef,
                                                CFMutableDictionaryRef *,
                                                uint64_t, CFTypeRef);
typedef CFDictionaryRef (*create_samples_t)(IOReportSubscriptionRef,
                                            CFMutableDictionaryRef, CFTypeRef);
typedef CFDictionaryRef (*samples_delta_t)(CFDictionaryRef, CFDictionaryRef,
                                           CFTypeRef);
typedef void (*iterate_t)(CFDictionaryRef, int (^)(CFDictionaryRef));
typedef CFStringRef (*chan_name_t)(CFDictionaryRef);
typedef int64_t (*simple_int_t)(CFDictionaryRef, int32_t);

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    double interval = (argc > 1) ? atof(argv[1]) : 1.0;
    if (interval <= 0) interval = 1.0;

    // The IOReport symbols live in libIOReport.dylib (resolved from the dyld
    // shared cache; no file exists on disk). The PrivateFrameworks framework
    // path is NOT in the cache on recent macOS, so try the dylib first.
    void *lib = dlopen("/usr/lib/libIOReport.dylib", RTLD_LAZY);
    if (!lib)
        lib = dlopen("libIOReport.dylib", RTLD_LAZY);
    if (!lib)
        lib = dlopen(
            "/System/Library/PrivateFrameworks/IOReport.framework/IOReport",
            RTLD_LAZY);
    if (!lib) {
        fprintf(stderr, "membw: cannot load IOReport: %s\n", dlerror());
        return 1;
    }

    copy_chans_t     IOReportCopyChannelsInGroup  = (copy_chans_t)    dlsym(lib, "IOReportCopyChannelsInGroup");
    create_sub_t     IOReportCreateSubscription   = (create_sub_t)    dlsym(lib, "IOReportCreateSubscription");
    create_samples_t IOReportCreateSamples        = (create_samples_t)dlsym(lib, "IOReportCreateSamples");
    samples_delta_t  IOReportCreateSamplesDelta   = (samples_delta_t) dlsym(lib, "IOReportCreateSamplesDelta");
    iterate_t        IOReportIterate              = (iterate_t)       dlsym(lib, "IOReportIterate");
    chan_name_t      IOReportChannelGetChannelName= (chan_name_t)     dlsym(lib, "IOReportChannelGetChannelName");
    simple_int_t     IOReportSimpleGetIntegerValue= (simple_int_t)    dlsym(lib, "IOReportSimpleGetIntegerValue");

    if (!IOReportCopyChannelsInGroup || !IOReportCreateSubscription ||
        !IOReportCreateSamples || !IOReportCreateSamplesDelta ||
        !IOReportIterate || !IOReportChannelGetChannelName ||
        !IOReportSimpleGetIntegerValue) {
        fprintf(stderr, "membw: missing IOReport symbol(s)\n");
        return 1;
    }

    // Newer chips report the group as "AMC Stats"; older ones used "AMC".
    CFMutableDictionaryRef chans =
        IOReportCopyChannelsInGroup(CFSTR("AMC Stats"), NULL, 0, 0, 0);
    if (!chans)
        chans = IOReportCopyChannelsInGroup(CFSTR("AMC"), NULL, 0, 0, 0);
    if (!chans) {
        fprintf(stderr, "membw: IOReportCopyChannelsInGroup failed\n");
        return 1;
    }

    CFMutableDictionaryRef subbed = NULL;
    IOReportSubscriptionRef sub =
        IOReportCreateSubscription(NULL, chans, &subbed, 0, NULL);
    if (!sub) {
        fprintf(stderr, "membw: IOReportCreateSubscription failed (need sudo?)\n");
        return 1;
    }

    double t0 = now_sec();
    CFDictionaryRef s1 = IOReportCreateSamples(sub, subbed, NULL);

    struct timespec req;
    req.tv_sec = (time_t)interval;
    req.tv_nsec = (long)((interval - (double)req.tv_sec) * 1e9);
    nanosleep(&req, NULL);

    CFDictionaryRef s2 = IOReportCreateSamples(sub, subbed, NULL);
    double dt = now_sec() - t0;

    CFDictionaryRef delta = IOReportCreateSamplesDelta(s1, s2, NULL);
    if (!delta || dt <= 0) {
        fprintf(stderr, "membw: no sample delta\n");
        return 1;
    }

    __block long long rd = 0, wr = 0;
    IOReportIterate(delta, ^int(CFDictionaryRef ch) {
        CFStringRef name = IOReportChannelGetChannelName(ch);
        if (!name) return 0;  // kIOReportIterOk: continue
        char buf[128];
        if (!CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8))
            return 0;
        // Match the AGGREGATE channels only (exact compare excludes per-agent
        // variants like "ECPU DCS RD", "GFX DCS WR", etc.).
        if (strcmp(buf, "DCS RD") == 0)
            rd += IOReportSimpleGetIntegerValue(ch, 0);
        else if (strcmp(buf, "DCS WR") == 0)
            wr += IOReportSimpleGetIntegerValue(ch, 0);
        return 0;
    });

    double gbs = (double)(rd + wr) / dt / 1e9;
    printf("%.2f\n", gbs);
    return 0;
}
