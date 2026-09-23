// =============================================================================
// membw - print system-wide DRAM bandwidth (GB/s) from IOReport
// =============================================================================
//
// Apple Silicon exposes no memory-bandwidth sampler via powermetrics. The only
// source is the private IOReport framework.
//
// Two chip-dependent sources, tried in order. Neither path calls sudo.
//
//   1. AMC Stats byte counters (base M4, and any chip where the group
//      subscribes and the delta contains DCS byte channels):
//         "DCS RD" / "DCS WR"            aggregate DRAM bytes
//         "DCS F<n> RD" / "DCS F<n> WR"  per-frequency-bin bytes
//         "DCS"                          combined RD+WR bytes
//      On a base M4 Mac mini (Mac16,10, macOS 27) this subscribes as a
//      normal user. Idle is a few GB/s; an all-core copy reads ~106 GB/s,
//      matching DCS RD+WR.
//
//   2. PMP "DCS BW" / "AMCC RD|WR|RD+WR" rate histograms (M4 Pro / M4 Max,
//      and any chip where AMC Stats will not subscribe). These are 32-bucket
//      residencies labeled "16GB/s".."512GB/s" on M4 Pro. The first bucket is
//      an underflow bin (everything below the first label, including true
//      idle ~0.1 GB/s); treating its label as 16 made idle look like a
//      phantom 16 GB/s. Bucket 0 is counted as 0; higher buckets keep their
//      labels. PMP subscribes without root.
//
// Why M4 Pro cannot use (1): the M4 Pro AMC Stats group has ~190 channels
// including DCS F1–F6 bins, and IOReportCreateSubscription on that group
// alone returns NULL. The aggregate names "DCS RD"/"DCS WR" exist in the
// channel catalog but never appear in a sample delta. PMP is the working path.
//
// This helper never opens an NVMe/disk IOKit user client, so it cannot lock
// out smartctl the way a full mactop sample can.
//
// IOReport is a PRIVATE framework with no on-disk .framework or SDK stub to
// link against (it lives only in the dyld shared cache), so we resolve its
// symbols at runtime with dlopen()/dlsym() instead of -framework IOReport.
//
// Build:
//     clang -O2 -Wall -framework CoreFoundation -o membw membw.c
//
// Usage:
//     ./membw [interval_seconds]      # default interval 1.0; no sudo
// =============================================================================

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
typedef CFStringRef (*chan_name_t)(CFDictionaryRef);
typedef CFStringRef (*chan_sub_t)(CFDictionaryRef);
typedef int64_t (*simple_int_t)(CFDictionaryRef, int32_t);
typedef int32_t (*state_count_t)(CFDictionaryRef);
typedef CFStringRef (*state_name_t)(CFDictionaryRef, int32_t);
typedef int64_t (*state_res_t)(CFDictionaryRef, int32_t);

static copy_chans_t IOReportCopyChannelsInGroup;
static create_sub_t IOReportCreateSubscription;
static create_samples_t IOReportCreateSamples;
static samples_delta_t IOReportCreateSamplesDelta;
static chan_name_t IOReportChannelGetChannelName;
static chan_sub_t IOReportChannelGetSubGroup;
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

// IOReportTypes.h: kIOReportInvalidIntValue == INT64_MIN
static int valid_counter(int64_t v) { return v >= 0; }

static int is_freq_bin_dcs(const char *n, const char *dir) {
    // "DCS F1 RD", "DCS F6 WR" — not duration/CAS/RAS.
    if (strncmp(n, "DCS F", 5) != 0)
        return 0;
    if (n[5] < '0' || n[5] > '9')
        return 0;
    if (strstr(n, "duration") || strstr(n, "CAS") || strstr(n, "RAS"))
        return 0;
    size_t dlen = strlen(dir);
    size_t nlen = strlen(n);
    return nlen >= dlen && strcmp(n + nlen - dlen, dir) == 0;
}

static CFArrayRef channels_of(CFDictionaryRef dict) {
    if (!dict)
        return NULL;
    return CFDictionaryGetValue(dict, CFSTR("IOReportChannels"));
}

// Keep only PMP DCS-BW AMCC histograms so we don't subscribe to 500+ channels.
static CFMutableDictionaryRef filter_pmp_amcc(CFDictionaryRef pmp) {
    CFArrayRef arr = channels_of(pmp);
    if (!arr)
        return NULL;
    CFMutableArrayRef keep =
        CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    if (!keep)
        return NULL;
    CFIndex n = CFArrayGetCount(arr);
    for (CFIndex i = 0; i < n; i++) {
        CFDictionaryRef ch = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, i);
        char sub[64] = {0}, name[128] = {0};
        cf_to_buf(IOReportChannelGetSubGroup(ch), sub, sizeof(sub));
        cf_to_buf(IOReportChannelGetChannelName(ch), name, sizeof(name));
        if (strcmp(sub, "DCS BW") == 0 && strncmp(name, "AMCC ", 5) == 0)
            CFArrayAppendValue(keep, ch);
    }
    if (CFArrayGetCount(keep) == 0) {
        CFRelease(keep);
        return NULL;
    }
    CFMutableDictionaryRef out =
        CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, pmp);
    if (!out) {
        CFRelease(keep);
        return NULL;
    }
    CFDictionarySetValue(out, CFSTR("IOReportChannels"), keep);
    CFRelease(keep);
    return out;
}

static CFDictionaryRef take_delta(IOReportSubscriptionRef sub,
                                  CFMutableDictionaryRef chans, double interval,
                                  double *dt_out) {
    double t0 = now_sec();
    CFDictionaryRef s1 = IOReportCreateSamples(sub, chans, NULL);
    struct timespec req;
    req.tv_sec = (time_t)interval;
    req.tv_nsec = (long)((interval - (double)req.tv_sec) * 1e9);
    nanosleep(&req, NULL);
    CFDictionaryRef s2 = IOReportCreateSamples(sub, chans, NULL);
    *dt_out = now_sec() - t0;
    if (!s1 || !s2 || *dt_out <= 0)
        return NULL;
    return IOReportCreateSamplesDelta(s1, s2, NULL);
}

// Returns combined GB/s from AMC byte counters, or -1 if none were usable.
static double gbs_from_amc(CFDictionaryRef delta, double dt) {
    CFArrayRef arr = channels_of(delta);
    if (!arr)
        return -1.0;

    long long exact_rd = 0, exact_wr = 0, exact_comb = 0;
    long long bin_rd = 0, bin_wr = 0;
    int has_exact_rd = 0, has_exact_wr = 0, has_exact_comb = 0;
    int has_bin_rd = 0, has_bin_wr = 0;

    CFIndex n = CFArrayGetCount(arr);
    for (CFIndex i = 0; i < n; i++) {
        CFDictionaryRef ch = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, i);
        char name[128] = {0};
        if (!cf_to_buf(IOReportChannelGetChannelName(ch), name, sizeof(name)))
            continue;
        int64_t v = IOReportSimpleGetIntegerValue(ch, 0);
        if (!valid_counter(v))
            continue;

        if (strcmp(name, "DCS RD") == 0) {
            exact_rd += v;
            has_exact_rd = 1;
        } else if (strcmp(name, "DCS WR") == 0) {
            exact_wr += v;
            has_exact_wr = 1;
        } else if (strcmp(name, "DCS") == 0) {
            exact_comb += v;
            has_exact_comb = 1;
        } else if (is_freq_bin_dcs(name, " RD")) {
            bin_rd += v;
            has_bin_rd = 1;
        } else if (is_freq_bin_dcs(name, " WR")) {
            bin_wr += v;
            has_bin_wr = 1;
        }
    }

    long long bytes = 0;
    int ok = 0;
    if (has_exact_rd || has_exact_wr) {
        bytes = exact_rd + exact_wr;
        ok = 1;
    } else if (has_bin_rd || has_bin_wr) {
        bytes = bin_rd + bin_wr;
        ok = 1;
    } else if (has_exact_comb) {
        bytes = exact_comb;
        ok = 1;
    }
    if (!ok)
        return -1.0;
    return (double)bytes / dt / 1e9;
}

// Residency-weighted average of a "16GB/s" / "1GB/s" histogram. Returns -1
// if this channel is not a usable histogram.
//
// The first bucket is an underflow/floor bin: on M4 Pro the labels start at
// 16 GB/s with no 0 bucket, so idle residency sits entirely in "16GB/s"
// even when true DRAM traffic is ~0.1 GB/s (the M4-base AMC idle reading).
// Count bucket 0 as 0 GB/s ("below the first label"). Higher buckets keep
// their labels; under load bucket 0 is empty so the reading is unchanged.
static double hist_avg_gbs(CFDictionaryRef ch) {
    if (!IOReportStateGetCount || !IOReportStateGetNameForIndex ||
        !IOReportStateGetResidency)
        return -1.0;
    int32_t n = IOReportStateGetCount(ch);
    if (n <= 1)
        return -1.0;
    int64_t tot = 0;
    double weighted = 0.0;
    for (int32_t s = 0; s < n; s++) {
        int64_t r = IOReportStateGetResidency(ch, s);
        if (r <= 0)
            continue;
        char sn[64] = {0};
        cf_to_buf(IOReportStateGetNameForIndex(ch, s), sn, sizeof(sn));
        // atof skips leading spaces: "  16GB/s" -> 16.0
        double gbps = (s == 0) ? 0.0 : atof(sn);
        if (s != 0 && gbps <= 0.0)
            continue;
        weighted += gbps * (double)r;
        tot += r;
    }
    if (tot <= 0)
        return 0.0;
    return weighted / (double)tot;
}

// Combined GB/s from PMP DCS-BW AMCC histograms, or -1 if none.
static double gbs_from_pmp(CFDictionaryRef delta) {
    CFArrayRef arr = channels_of(delta);
    if (!arr)
        return -1.0;

    double rd = -1.0, wr = -1.0, comb = -1.0;
    CFIndex n = CFArrayGetCount(arr);
    for (CFIndex i = 0; i < n; i++) {
        CFDictionaryRef ch = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, i);
        char sub[64] = {0}, name[128] = {0};
        cf_to_buf(IOReportChannelGetSubGroup(ch), sub, sizeof(sub));
        cf_to_buf(IOReportChannelGetChannelName(ch), name, sizeof(name));
        if (strcmp(sub, "DCS BW") != 0)
            continue;
        if (strcmp(name, "AMCC RD+WR") == 0)
            comb = hist_avg_gbs(ch);
        else if (strcmp(name, "AMCC RD") == 0)
            rd = hist_avg_gbs(ch);
        else if (strcmp(name, "AMCC WR") == 0)
            wr = hist_avg_gbs(ch);
    }
    if (comb >= 0.0)
        return comb;
    if (rd >= 0.0 || wr >= 0.0)
        return (rd > 0.0 ? rd : 0.0) + (wr > 0.0 ? wr : 0.0);
    return -1.0;
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
        fprintf(stderr, "membw: cannot load IOReport: %s\n", dlerror());
        return 0;
    }

    IOReportCopyChannelsInGroup =
        (copy_chans_t)dlsym(lib, "IOReportCopyChannelsInGroup");
    IOReportCreateSubscription =
        (create_sub_t)dlsym(lib, "IOReportCreateSubscription");
    IOReportCreateSamples = (create_samples_t)dlsym(lib, "IOReportCreateSamples");
    IOReportCreateSamplesDelta =
        (samples_delta_t)dlsym(lib, "IOReportCreateSamplesDelta");
    IOReportChannelGetChannelName =
        (chan_name_t)dlsym(lib, "IOReportChannelGetChannelName");
    IOReportChannelGetSubGroup =
        (chan_sub_t)dlsym(lib, "IOReportChannelGetSubGroup");
    IOReportSimpleGetIntegerValue =
        (simple_int_t)dlsym(lib, "IOReportSimpleGetIntegerValue");
    IOReportStateGetCount = (state_count_t)dlsym(lib, "IOReportStateGetCount");
    IOReportStateGetNameForIndex =
        (state_name_t)dlsym(lib, "IOReportStateGetNameForIndex");
    IOReportStateGetResidency = (state_res_t)dlsym(lib, "IOReportStateGetResidency");

    if (!IOReportCopyChannelsInGroup || !IOReportCreateSubscription ||
        !IOReportCreateSamples || !IOReportCreateSamplesDelta ||
        !IOReportChannelGetChannelName || !IOReportSimpleGetIntegerValue) {
        fprintf(stderr, "membw: missing IOReport symbol(s)\n");
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    double interval = (argc > 1) ? atof(argv[1]) : 1.0;
    if (interval <= 0)
        interval = 1.0;

    if (!load_ioreport())
        return 1;

    // --- Path 1: AMC Stats byte counters (M1 / M4 base) ----------------------
    CFMutableDictionaryRef amc =
        IOReportCopyChannelsInGroup(CFSTR("AMC Stats"), NULL, 0, 0, 0);
    if (!amc)
        amc = IOReportCopyChannelsInGroup(CFSTR("AMC"), NULL, 0, 0, 0);

    if (amc) {
        CFMutableDictionaryRef subbed = NULL;
        IOReportSubscriptionRef sub =
            IOReportCreateSubscription(NULL, amc, &subbed, 0, NULL);
        CFMutableDictionaryRef sample_chans = subbed ? subbed : amc;
        if (sub) {
            double dt = 0;
            CFDictionaryRef delta = take_delta(sub, sample_chans, interval, &dt);
            if (delta && dt > 0) {
                double gbs = gbs_from_amc(delta, dt);
                // A usable AMC reading (including genuine idle 0) wins.
                // -1 means the delta had no DCS byte channels — fall through
                // to PMP (the M4 Pro case: subscription may succeed when
                // merged, but AMC never appears in the delta).
                if (gbs >= 0.0) {
                    printf("%.2f\n", gbs);
                    return 0;
                }
            }
        }
    }

    // --- Path 2: PMP DCS-BW AMCC histograms (M4 Pro / M4 Max) ----------------
    CFMutableDictionaryRef pmp =
        IOReportCopyChannelsInGroup(CFSTR("PMP"), NULL, 0, 0, 0);
    if (!pmp) {
        fprintf(stderr, "membw: no AMC Stats sample and no PMP group\n");
        return 1;
    }
    CFMutableDictionaryRef pmp_amcc = filter_pmp_amcc(pmp);
    CFMutableDictionaryRef pmp_chans = pmp_amcc ? pmp_amcc : pmp;

    CFMutableDictionaryRef subbed = NULL;
    IOReportSubscriptionRef sub =
        IOReportCreateSubscription(NULL, pmp_chans, &subbed, 0, NULL);
    if (!sub) {
        fprintf(stderr, "membw: IOReportCreateSubscription failed\n");
        return 1;
    }
    CFMutableDictionaryRef sample_chans = subbed ? subbed : pmp_chans;
    double dt = 0;
    CFDictionaryRef delta = take_delta(sub, sample_chans, interval, &dt);
    if (!delta || dt <= 0) {
        fprintf(stderr, "membw: no sample delta\n");
        return 1;
    }
    double gbs = gbs_from_pmp(delta);
    if (gbs < 0.0) {
        fprintf(stderr, "membw: no DCS / AMCC bandwidth counters\n");
        return 1;
    }
    printf("%.2f\n", gbs);
    return 0;
}
