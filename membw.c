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
//      Subscribed as that filtered set, not the whole group. A large catalog
//      makes IOReportCreateSubscription return NULL (M4 Pro ~190 channels;
//      M3 Ultra 305, and on that chip even one AMC channel is rejected).
//      On a base M4 Mac mini (Mac16,10, macOS 27) this subscribes as a
//      normal user. Idle is a few GB/s; an all-core copy reads ~106 GB/s,
//      matching DCS RD+WR.
//
//   2. PMP "DCS BW" / "AMCC[n] RD|WR|RD+WR" rate histograms, when AMC byte
//      counters will not subscribe. The group is "PMP" on a single die
//      (M4 Pro / M4 Max: "AMCC RD+WR") and "PMP0", "PMP1", ... on an Ultra
//      (M3 Ultra: "AMCC0 RD+WR" on PMP0 and "AMCC1 RD+WR" on PMP1). Those
//      are per-die memory-controller aggregates, so they are summed.
//      32-bucket residencies. Labels start at "16GB/s" on M4 Pro and
//      "64GB/s" on M3 Ultra; the top of the M3 Ultra scale is "2048GB/s".
//      The first bucket is an underflow bin (everything below the first
//      label, including true idle). Treating its label as the rate made
//      idle look like a phantom 16 or 64 GB/s per die. Bucket 0 is counted
//      as 0; higher buckets keep their labels. PMP subscribes without root.
//
// Why M4 Pro and M3 Ultra cannot use (1): IOReportCreateSubscription on
// AMC Stats returns NULL. On M4 Pro the aggregate names exist in the
// catalog but never appear in a sample delta. On M3 Ultra (Mac15,14) a
// one-channel subscription fails the same way, while the per-die PMP
// histograms move: desktop idle is 0, an 8-bit Qwen3.8-27B prefill stays
// in the low tens of GB/s, and decode holds ~560 GB/s (~70% of the
// 819 GB/s peak).
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
typedef void (*merge_chans_t)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
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
static merge_chans_t IOReportMergeChannels;
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

// "AMCC RD+WR" on a single die, "AMCC0 RD+WR" / "AMCC1 RD" on an Ultra.
// Returns 3 for RD+WR, 1 for RD, 2 for WR, 0 if this is not an AMCC rate.
static int amcc_direction(const char *name) {
    if (strncmp(name, "AMCC", 4) != 0)
        return 0;
    const char *p = name + 4;
    while (*p >= '0' && *p <= '9')
        p++;
    if (*p != ' ')
        return 0;
    p++;
    if (strcmp(p, "RD+WR") == 0)
        return 3;
    if (strcmp(p, "RD") == 0)
        return 1;
    if (strcmp(p, "WR") == 0)
        return 2;
    return 0;
}

static int is_amc_byte_channel(const char *name) {
    if (strcmp(name, "DCS RD") == 0 || strcmp(name, "DCS WR") == 0 ||
        strcmp(name, "DCS") == 0)
        return 1;
    return is_freq_bin_dcs(name, " RD") || is_freq_bin_dcs(name, " WR");
}

// Keep channels selected by `keep_name`. The result is a mutable copy of
// `src` whose IOReportChannels array is only the matches.
static CFMutableDictionaryRef filter_channels(CFDictionaryRef src,
                                              int (*keep_name)(const char *,
                                                               const char *)) {
    CFArrayRef arr = channels_of(src);
    if (!arr || !keep_name)
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
        if (keep_name(sub, name))
            CFArrayAppendValue(keep, ch);
    }
    if (CFArrayGetCount(keep) == 0) {
        CFRelease(keep);
        return NULL;
    }
    CFMutableDictionaryRef out =
        CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, src);
    if (!out) {
        CFRelease(keep);
        return NULL;
    }
    CFDictionarySetValue(out, CFSTR("IOReportChannels"), keep);
    CFRelease(keep);
    return out;
}

static int keep_amc_bytes(const char *sub, const char *name) {
    (void)sub;
    return is_amc_byte_channel(name);
}

// DCS BW only. AFC/AFI/AFR AMCC histograms are fabric, not DRAM, and read
// a different number under the same GPU load.
static int keep_pmp_amcc(const char *sub, const char *name) {
    return strcmp(sub, "DCS BW") == 0 && amcc_direction(name) != 0;
}

// "PMP" plus "PMP0", "PMP1", ... until a numbered group is empty. An empty
// plain "PMP" (M3 Ultra) is skipped; the numbered groups are the dies.
static CFMutableDictionaryRef copy_pmp_amcc(void) {
    CFMutableDictionaryRef merged = NULL;
    for (int die = -1; die < 8; die++) {
        char gname[8];
        if (die < 0)
            snprintf(gname, sizeof(gname), "PMP");
        else
            snprintf(gname, sizeof(gname), "PMP%d", die);
        CFStringRef g = CFStringCreateWithCString(kCFAllocatorDefault, gname,
                                                  kCFStringEncodingUTF8);
        CFDictionaryRef pmp = IOReportCopyChannelsInGroup(g, NULL, 0, 0, 0);
        CFRelease(g);
        CFArrayRef arr = channels_of(pmp);
        long n = arr ? (long)CFArrayGetCount(arr) : 0;
        if (n == 0) {
            if (pmp)
                CFRelease(pmp);
            if (die >= 0)
                break;
            continue;
        }
        CFMutableDictionaryRef kept = filter_channels(pmp, keep_pmp_amcc);
        CFRelease(pmp);
        if (!kept)
            continue;
        if (!merged) {
            merged = kept;
            continue;
        }
        if (!IOReportMergeChannels) {
            fprintf(stderr, "membw: IOReportMergeChannels missing; "
                            "per-die bandwidth is incomplete\n");
            CFRelease(kept);
            break;
        }
        IOReportMergeChannels(merged, kept, NULL);
        CFRelease(kept);
    }
    return merged;
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
// Per-die channels (AMCC0, AMCC1, ...) each cover one memory controller, so
// they are summed. RD+WR wins over RD plus WR: those are the same traffic
// split two ways, and each direction has its own underflow bin.
static double gbs_from_pmp(CFDictionaryRef delta) {
    CFArrayRef arr = channels_of(delta);
    if (!arr)
        return -1.0;

    double rd = 0.0, wr = 0.0, comb = 0.0;
    int nrd = 0, nwr = 0, ncomb = 0;
    CFIndex n = CFArrayGetCount(arr);
    for (CFIndex i = 0; i < n; i++) {
        CFDictionaryRef ch = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, i);
        char sub[64] = {0}, name[128] = {0};
        cf_to_buf(IOReportChannelGetSubGroup(ch), sub, sizeof(sub));
        cf_to_buf(IOReportChannelGetChannelName(ch), name, sizeof(name));
        int dir = amcc_direction(name);
        if (strcmp(sub, "DCS BW") != 0 || dir == 0)
            continue;
        double avg = hist_avg_gbs(ch);
        if (avg < 0.0)
            continue;
        if (dir == 3) {
            comb += avg;
            ncomb++;
        } else if (dir == 1) {
            rd += avg;
            nrd++;
        } else if (dir == 2) {
            wr += avg;
            nwr++;
        }
    }
    if (ncomb)
        return comb;
    if (nrd || nwr)
        return rd + wr;
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
    IOReportMergeChannels =
        (merge_chans_t)dlsym(lib, "IOReportMergeChannels");
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

// Prints a line and returns 1 when this channel set yields a usable AMC
// reading, including a genuine idle 0. Subscription failure and a delta
// with no DCS byte channels both return 0 so the caller can try PMP.
static int emit_amc(CFMutableDictionaryRef chans, double interval) {
    if (!chans)
        return 0;
    CFMutableDictionaryRef subbed = NULL;
    IOReportSubscriptionRef sub =
        IOReportCreateSubscription(NULL, chans, &subbed, 0, NULL);
    if (!sub)
        return 0;
    CFMutableDictionaryRef sample_chans = subbed ? subbed : chans;
    double dt = 0;
    CFDictionaryRef delta = take_delta(sub, sample_chans, interval, &dt);
    if (!delta || dt <= 0)
        return 0;
    double gbs = gbs_from_amc(delta, dt);
    if (gbs < 0.0)
        return 0;
    printf("%.2f\n", gbs);
    return 1;
}

int main(int argc, char **argv) {
    double interval = (argc > 1) ? atof(argv[1]) : 1.0;
    if (interval <= 0)
        interval = 1.0;

    if (!load_ioreport())
        return 1;

    // --- Path 1: AMC Stats byte counters (M1 / M4 base) ----------------------
    // The filtered set is what gbs_from_amc reads. Subscribing to the whole
    // group fails when the catalog is large. If the filtered subscribe fails,
    // the unfiltered group is still tried so a small M4 catalog keeps the
    // path that was checked there.
    CFMutableDictionaryRef amc =
        IOReportCopyChannelsInGroup(CFSTR("AMC Stats"), NULL, 0, 0, 0);
    if (!amc)
        amc = IOReportCopyChannelsInGroup(CFSTR("AMC"), NULL, 0, 0, 0);

    if (amc) {
        CFMutableDictionaryRef filtered = filter_channels(amc, keep_amc_bytes);
        if (emit_amc(filtered, interval))
            return 0;
        if (filtered && emit_amc(amc, interval))
            return 0;
    }

    // --- Path 2: PMP DCS-BW AMCC histograms (M4 Pro / M4 Max / M3 Ultra) -----
    CFMutableDictionaryRef pmp = copy_pmp_amcc();
    if (!pmp) {
        fprintf(stderr, "membw: no AMC Stats sample and no PMP AMCC histogram\n");
        return 1;
    }

    CFMutableDictionaryRef subbed = NULL;
    IOReportSubscriptionRef sub =
        IOReportCreateSubscription(NULL, pmp, &subbed, 0, NULL);
    if (!sub) {
        fprintf(stderr, "membw: IOReportCreateSubscription failed\n");
        return 1;
    }
    CFMutableDictionaryRef sample_chans = subbed ? subbed : pmp;
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
