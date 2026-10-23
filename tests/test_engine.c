/*
 * Deep Space Convolution - engine and plugin tests (desktop).
 *
 * Links the DSP sources directly. The API tests go through
 * move_audio_fx_init_v2() exactly as the chain host does (int16, 128-frame
 * blocks); the precision tests use the convolver and the float engine.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ds_dsp.h"
#include "ds_engine.h"
#include "host/audio_fx_api_v2.h"
#include "signals.h"

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host);

#define FS 44100
#define BLOCK 128

static int failures = 0;
static void check(int ok, const char *what, double v) {
    printf("%s  %-66s %.5g\n", ok ? "ok  " : "FAIL", what, v);
    if (!ok) failures++;
}

static audio_fx_api_v2_t *api;
static host_api_v1_t host;

static void setp(void *in, const char *k, double v) {
    char b[32];
    snprintf(b, sizeof(b), "%g", v);
    api->set_param(in, k, b);
}
static void sets(void *in, const char *k, const char *v) { api->set_param(in, k, v); }
static const char *getp(void *in, const char *k) {
    static char b[4096];
    if (api->get_param(in, k, b, sizeof(b)) < 0) return "<err>";
    return b;
}

/* float stereo interleaved <-> the plugin's int16 blocks */
static void run(void *in, const float *src, float *dst, long frames) {
    int16_t blk[2 * BLOCK];
    for (long f = 0; f < frames; f += BLOCK) {
        int n = frames - f < BLOCK ? (int)(frames - f) : BLOCK;
        for (int i = 0; i < 2 * n; ++i) {
            float x = src ? src[2 * f + i] : 0.0f;
            long v = lrintf(x * 32767.0f);
            blk[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        api->process_block(in, blk, n);
        if (dst) for (int i = 0; i < 2 * n; ++i) dst[2 * f + i] = blk[i] / 32768.0f;
    }
}

/* Process silence until the requested IR is built, faded in and steady. */
static int settle(void *in) {
    int16_t blk[2 * BLOCK];
    for (int i = 0; i < 4000; ++i) {
        if (ds_engine_settled((ds_engine *)in)) return i;
        memset(blk, 0, sizeof blk);
        api->process_block(in, blk, BLOCK);
    }
    return -1;
}

static double rms_of(const float *b, long from, long to) {
    double s = 0;
    for (long i = 2 * from; i < 2 * to; ++i) s += (double)b[i] * b[i];
    return sqrt(s / (2.0 * (double)(to - from)));
}
static double peak_of(const float *b, long from, long to) {
    double p = 0;
    for (long i = 2 * from; i < 2 * to; ++i) p = fmax(p, fabs(b[i]));
    return p;
}
static double db(double r) { return 20 * log10(r + 1e-12); }
static int all_finite(const float *b, long frames) {
    for (long i = 0; i < 2 * frames; ++i)
        if (!isfinite(b[i])) return 0;
    return 1;
}

static float *A, *B, *C;   /* scratch, 400 s stereo each */
#define SCRATCH (400L * FS)

static void *make_default(void) {
    void *in = api->create_instance("", NULL);
    return in;
}

/* A new instance (silent history) with mix set and everything settled. */
static void *fresh(double mix) {
    void *in = make_default();
    setp(in, "mix", mix);
    settle(in);
    return in;
}

/* ---------------------------------------------------------------- 1 */
static ds_conv g_conv;
static void test_convolution(void) {
    puts("-- 1. partitioned convolution == direct convolution");
    const int lens[] = {3000, 4096, 4097, 40000, DS_IR_MAX};
    for (unsigned t = 0; t < sizeof lens / sizeof lens[0]; ++t) {
        const int len = lens[t];
        memset(&g_conv, 0, sizeof g_conv);
        ds_conv_init_small(&g_conv);
        float *h[2] = {A, A + DS_IR_MAX};
        uint32_t r = 77 + t;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < len; ++i) h[ch][i] = ds_rng_bipolar(&r) * expf(-(float)i / 15000.0f);
        ds_conv_load_now(&g_conv, h[0], h[1], len, 0.75f);
        const long n = 3L * 44100;
        float *x[2] = {B, B + n}, *y[2] = {C, C + n};
        for (int ch = 0; ch < 2; ++ch)
            for (long i = 0; i < n; ++i) x[ch][i] = (i == 0 && ch == 0) ? 1.0f : 0.3f * ds_rng_bipolar(&r);
        for (long b = 0; b + DS_NA <= n; b += DS_NA)
            ds_conv_process(&g_conv, x[0] + b, x[1] + b, y[0] + b, y[1] + b);
        double err = 0, ref = 0;
        for (int ch = 0; ch < 2; ++ch) {
            for (long i = 0; i < n - DS_NA; i += 997) {
                double s = 0;
                for (long k = 0; k < len && k <= i; ++k) s += (double)h[ch][k] * x[ch][i - k];
                s *= 0.75;
                err = fmax(err, fabs(s - y[ch][i]));
                ref = fmax(ref, fabs(s));
            }
        }
        char what[96];
        snprintf(what, sizeof what, "IR %5d samples: max error vs direct (dB below peak)", len);
        check(db(err / ref) < -100.0, what, db(err / ref));
    }
    /* a pure impulse returns the IR itself, sample for sample */
    memset(&g_conv, 0, sizeof g_conv);
    ds_conv_init_small(&g_conv);
    uint32_t r = 5;
    for (int i = 0; i < 20000; ++i) A[i] = ds_rng_bipolar(&r);
    ds_conv_load_now(&g_conv, A, A, 20000, 1.0f);
    float imp[DS_NA] = {1.0f}, z[DS_NA] = {0}, out[2][DS_NA];
    double e = 0;
    for (int b = 0; b < 20000 / DS_NA; ++b) {
        ds_conv_process(&g_conv, b ? z : imp, b ? z : imp, out[0], out[1]);
        for (int i = 0; i < DS_NA; ++i) e = fmax(e, fabs(out[0][i] - A[b * DS_NA + i]));
    }
    check(e < 1e-5, "impulse in -> the IR out (max abs error)", e);
}

/* ---------------------------------------------------------------- 2 */
static ds_engine g_e1, g_e2;
static void test_block_size(void) {
    puts("-- 2. any host block size gives the same output");
    const long n = 4L * FS;
    fill_mix(A, n, 0.5f);
    const int sizes[][2] = {{128, 128}, {1, 1}, {37, 37}, {64, 64}, {300, 300}, {1, 300}};
    float *ref = B, *got = C;
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; ++s) {
        ds_engine *e = s == 0 ? &g_e1 : &g_e2;
        memset(e, 0, sizeof *e);
        ds_engine_init(e, FS, 1);
        ds_engine_set(e, DS_P_MIX, 0.6f);
        ds_engine_set(e, DS_P_SPACE, 3);
        ds_engine_snap(e);
        float *dst = s == 0 ? ref : got;
        static float l[400], rr[400];
        uint32_t rng = 9;
        for (long f = 0; f < n;) {
            int bs = sizes[s][0] == sizes[s][1] ? sizes[s][0]
                     : sizes[s][0] + (int)(ds_rng_next(&rng) % (unsigned)(sizes[s][1] - sizes[s][0] + 1));
            if (bs > n - f) bs = (int)(n - f);
            for (int i = 0; i < bs; ++i) {
                l[i] = A[2 * (f + i)];
                rr[i] = A[2 * (f + i) + 1];
            }
            ds_engine_process_float(e, l, rr, bs);
            for (int i = 0; i < bs; ++i) {
                dst[2 * (f + i)] = l[i];
                dst[2 * (f + i) + 1] = rr[i];
            }
            f += bs;
        }
        if (s == 0) {
            check(rms_of(ref, 2 * FS, n) > 0.01, "reference run carries reverb", rms_of(ref, 2 * FS, n));
            continue;
        }
        double d = 0;
        for (long i = 0; i < 2 * n; ++i) d = fmax(d, fabs(ref[i] - got[i]));
        char what[96];
        snprintf(what, sizeof what, "blocks of %d..%d frames vs 128: max difference", sizes[s][0], sizes[s][1]);
        check(d == 0.0, what, d);
    }
}

/* ---------------------------------------------------------------- 3 */
static void test_ir_switch(void) {
    puts("-- 3. SPACE / SIZE / DECAY changes crossfade, no discontinuity");
    void *in = make_default();
    setp(in, "mix", 1);
    settle(in);
    const long n = 16L * FS;
    fill_pink(A, n, 0.12f);
    /* switch every 1.5 s from 3 s on, the last one a SIZE jump */
    long at[12] = {0};
    int nat = 0;
    float *src = A;
    for (long f = 0, k = 0; f < n; f += BLOCK) {
        if (f >= 3L * FS + k * (3L * FS / 2) && k < 8) {
            if (k < 6) setp(in, "space", (double)((k + 1) % 6));
            else if (k == 6) setp(in, "size", 3.5);
            else setp(in, "decay", 2.0);
            at[nat++] = f;
            ++k;
        }
        run(in, src + 2 * f, B + 2 * f, BLOCK);
    }
    /* baseline: the largest sample step and 20 ms level change in a steady
     * stretch before the first switch */
    double base_step = 0, base_lvl = 0;
    for (long i = FS; i < 3L * FS; ++i)
        for (int c = 0; c < 2; ++c) base_step = fmax(base_step, fabs(B[2 * i + c] - B[2 * (i - 1) + c]));
    const long win = FS / 50;
    for (long i = FS + win; i + win < 3L * FS; i += win)
        base_lvl = fmax(base_lvl, fabs(db(rms_of(B, i, i + win)) - db(rms_of(B, i - win, i))));
    double sw_step = 0, sw_lvl = 0;
    for (long i = 3L * FS; i < n; ++i)
        for (int c = 0; c < 2; ++c) sw_step = fmax(sw_step, fabs(B[2 * i + c] - B[2 * (i - 1) + c]));
    for (long i = 3L * FS + win; i + win < n; i += win)
        sw_lvl = fmax(sw_lvl, fabs(db(rms_of(B, i, i + win)) - db(rms_of(B, i - win, i))));
    check(nat == 8 && at[7] > at[0], "eight changes made", nat);
    check(sw_step < 1.5 * base_step, "largest sample step across switches / steady baseline", sw_step / base_step);
    check(sw_lvl < base_lvl + 3.0, "largest 20 ms level jump across switches (dB) vs baseline+3",
          sw_lvl - base_lvl);
    check(all_finite(B, n), "finite", 1);
    check(atoi(getp(in, "ir_builds")) >= 8, "an IR was rebuilt for each change", atoi(getp(in, "ir_builds")));
    api->destroy_instance(in);
}

/* ---------------------------------------------------------------- 4 */
/* T30 of a lowpassed (<400 Hz) impulse response: DECAY is the low-band RT60
 * (the high band decays faster, by DARKNESS). */
static double measure_rt(void *in, double decay) {
    setp(in, "decay", decay);
    settle(in);
    const long n = (long)((decay * 1.3 + 2.0) * FS);
    memset(A, 0, sizeof(float) * 2 * n);
    A[0] = A[1] = 0.9f;
    run(in, A, B, n);
    static double edc[60L * FS];
    float lp = 0, lp2 = 0, k = 1.0f - expf(-2.0f * DS_PI * 400.0f / FS);
    for (long i = 0; i < n; ++i) {
        lp += (0.5f * (B[2 * i] + B[2 * i + 1]) - lp) * k;
        lp2 += (lp - lp2) * k;
        edc[i] = (double)lp2 * lp2;
    }
    for (long i = n - 2; i >= 0; --i) edc[i] += edc[i + 1];
    long i10 = -1, i40 = -1;
    for (long i = 0; i < n; ++i) {
        double d = 10 * log10(edc[i] / edc[0] + 1e-30);
        if (i10 < 0 && d < -10) i10 = i;
        if (i40 < 0 && d < -40) { i40 = i; break; }
    }
    if (i10 < 0 || i40 < 0) return -1;
    return (double)(i40 - i10) / FS * 2.0;
}

static void test_rt60(void) {
    puts("-- 4. DECAY sets the RT60 (low band, -10..-40 dB of the EDC)");
    const double decays[] = {1.0, 2.0, 5.0, 10.0, 20.0, 30.0};
    double prev = 0;
    for (unsigned i = 0; i < sizeof decays / sizeof decays[0]; ++i) {
        void *in = fresh(1);
        double rt = measure_rt(in, decays[i]);
        api->destroy_instance(in);
        char what[96];
        snprintf(what, sizeof what, "DECAY %4.1f s -> measured RT60 / DECAY", decays[i]);
        check(rt > 0.8 * decays[i] && rt < 1.2 * decays[i], what, rt / decays[i]);
        check(rt > prev, "  and longer than the previous setting", rt);
        prev = rt;
    }
}

/* ---------------------------------------------------------------- 5 */
static void depth_probe(int space, double depth, double *onset_ms, double *er_late_db, double *centroid,
                        double *total_db) {
    void *in = make_default();
    setp(in, "space", space);
    setp(in, "depth", depth);
    setp(in, "mix", 1);
    settle(in);
    const long n = 6L * FS;
    memset(A, 0, sizeof(float) * 2 * n);
    A[0] = A[1] = 0.9f;
    run(in, A, B, n);
    long on = 0;
    for (long i = 0; i < n; ++i)
        if (fabs(B[2 * i]) > 1e-4 || fabs(B[2 * i + 1]) > 1e-4) { on = i; break; }
    double er = 0, late = 0, de = 0, te = 0;
    for (long i = on; i < n; ++i) {
        double q = (double)B[2 * i] * B[2 * i] + (double)B[2 * i + 1] * B[2 * i + 1];
        if (i < on + (long)(0.12 * FS)) er += q;
        else late += q;
        if (i > on && i < on + 2L * FS) {
            double d = B[2 * i] - B[2 * (i - 1)];
            de += d * d;
            te += (double)B[2 * i] * B[2 * i];
        }
    }
    *onset_ms = on * 1000.0 / FS;
    *er_late_db = 10 * log10(er / late);
    *centroid = FS / (2 * M_PI) * sqrt(de / te);
    api->destroy_instance(in);
    /* loudness at the default MIX with pink noise */
    in = make_default();
    setp(in, "space", space);
    setp(in, "depth", depth);
    setp(in, "mix", 0.4);
    settle(in);
    fill_pink(A, 5L * FS, 0.1f);
    run(in, A, B, 5L * FS);
    *total_db = db(rms_of(B, 3L * FS, 5L * FS) / rms_of(A, 3L * FS, 5L * FS));
    api->destroy_instance(in);
}

static void test_depth(void) {
    puts("-- 5. DEPTH moves predelay, early/late, colour and direct level - not just volume");
    for (int s = 0; s < DS_SPACE_COUNT; ++s) {
        double on0, el0, c0, t0, on1, el1, c1, t1;
        depth_probe(s, 0.0, &on0, &el0, &c0, &t0);
        depth_probe(s, 1.0, &on1, &el1, &c1, &t1);
        printf("     %-9s depth 0 -> 1: onset %5.1f -> %5.1f ms, ER/late %+6.1f -> %+6.1f dB, "
               "centroid %4.0f -> %4.0f Hz, loudness %+4.1f -> %+4.1f dB\n",
               DS_SPACE_NAMES[s], on0, on1, el0, el1, c0, c1, t0, t1);
        check(on1 > on0 + 120.0, "  predelay grows by > 120 ms", on1 - on0);
        check(el1 < el0 - 5.0, "  early/late ratio falls by > 5 dB", el0 - el1);
        check(c1 < 0.6 * c0, "  spectral centroid of the space falls below 60 %", c1 / c0);
        check(fabs(t1 - t0) < 3.0, "  loudness (MIX 40 %, pink noise) within 3 dB", t1 - t0);
    }
}

/* ---------------------------------------------------------------- 6 */
static void test_freeze(void) {
    puts("-- 6. FREEZE: smooth, no rise, stable for 5 minutes, releases");
    void *in = make_default();
    setp(in, "mix", 1);
    setp(in, "decay", 4);
    settle(in);
    const long pre = 4L * FS, hold = 300L * FS, post = 12L * FS;
    fill_mix(A, pre + hold + post, 0.5f);
    run(in, A, B, pre);
    sets(in, "freeze", "ON");
    check(!strcmp(getp(in, "freeze"), "ON"), "freeze reads ON", 0);
    run(in, A + 2 * pre, B + 2 * pre, hold);   /* input keeps playing: it must not get in */
    sets(in, "freeze", "OFF");
    run(in, NULL, B + 2 * (pre + hold), post);
    const double before = rms_of(B, pre - FS, pre);
    const double held3 = rms_of(B, pre + 3L * FS, pre + 4L * FS);
    double lo = 1e9, hi = 0, dc = 0;
    for (long t = pre + 3L * FS; t + FS <= pre + hold; t += FS) {
        double r = rms_of(B, t, t + FS);
        lo = fmin(lo, r);
        hi = fmax(hi, r);
    }
    double sum = 0;
    for (long i = pre + hold - 10L * FS; i < pre + hold; ++i) sum += B[2 * i] + B[2 * i + 1];
    dc = fabs(sum / (20.0 * FS));
    double step = 0, base = 0;
    for (long i = pre - FS; i < pre; ++i) base = fmax(base, fabs(B[2 * i] - B[2 * i - 2]));
    for (long i = pre; i < pre + FS; ++i) step = fmax(step, fabs(B[2 * i] - B[2 * i - 2]));
    check(all_finite(B, pre + hold + post), "finite throughout", 1);
    check(db(held3 / before) < 1.5, "level 3 s into freeze vs before (dB, no rise)", db(held3 / before));
    check(db(hi / held3) < 1.0, "highest 1 s level over 5 min vs 3 s in (dB)", db(hi / held3));
    check(db(lo / held3) > -6.0, "lowest 1 s level over 5 min vs 3 s in (dB, sustains)", db(lo / held3));
    check(peak_of(B, pre, pre + hold) < 0.99, "peak during freeze", peak_of(B, pre, pre + hold));
    check(dc < 1e-3, "DC after 5 minutes frozen", dc);
    check(step < 1.5 * base, "largest step entering freeze / steady baseline", step / base);
    const double tail = rms_of(B, pre + hold + post - FS, pre + hold + post);
    check(db(tail / held3) < -30.0, "released: 11 s later the tail has decayed (dB)", db(tail / held3));
    check(atoi(getp(in, "nan_resets")) == 0 && atoi(getp(in, "fdn_clamps")) == 0, "no NaN reset, no safety clamp",
          atoi(getp(in, "fdn_clamps")));
    api->destroy_instance(in);
}

/* ---------------------------------------------------------------- 7 */
static void test_extremes(void) {
    puts("-- 7. every min/max corner is stable");
    const char *keys[] = {"depth", "size", "decay", "diffusion", "darkness", "mix"};
    const double lo[] = {0, 0.5, 1, 0, 0, 0}, hi[] = {1, 4, 30, 1, 1, 1};
    double worst = 0;
    int bad = 0;
    const long n = 3L * FS, tail = 2L * FS;
    fill_mix(A, n, 1.0f);   /* full scale */
    for (int m = 0; m < 64; ++m) {
        void *in = make_default();
        for (int k = 0; k < 6; ++k) setp(in, keys[k], (m >> k) & 1 ? hi[k] : lo[k]);
        setp(in, "space", m % 6);
        setp(in, "freeze", (m / 6) % 2);
        settle(in);
        run(in, A, B, n);
        run(in, NULL, B + 2 * n, tail);
        if (!all_finite(B, n + tail)) ++bad;
        worst = fmax(worst, peak_of(B, 0, n + tail));
        if (atoi(getp(in, "nan_resets"))) ++bad;
        api->destroy_instance(in);
    }
    check(bad == 0, "64 corners x full-scale drums: no NaN/Inf/reset", bad);
    check(worst <= 1.0, "peak output (int16 never wraps)", worst);
}

/* ---------------------------------------------------------------- 8 */
static void test_stereo(void) {
    puts("-- 8. stereo in, stereo out");
    void *in = make_default();
    setp(in, "mix", 1);
    settle(in);
    const long n = 4L * FS;
    memset(A, 0, sizeof(float) * 2 * n);
    A[0] = 0.9f;   /* left only */
    run(in, A, B, n);
    double sl = 0, sr = 0, slr = 0;
    for (long i = 0; i < n; ++i) {
        sl += (double)B[2 * i] * B[2 * i];
        sr += (double)B[2 * i + 1] * B[2 * i + 1];
        slr += (double)B[2 * i] * B[2 * i + 1];
    }
    check(db(sqrt(sr / sl)) > -10.0, "left-only impulse: right side carries the space (dB R/L)",
          db(sqrt(sr / sl)));
    check(fabs(slr / sqrt(sl * sr)) < 0.5, "and the two sides are decorrelated (|corr|)", fabs(slr / sqrt(sl * sr)));
    /* a mono impulse is decorrelated too */
    api->destroy_instance(in);
    in = fresh(1);
    memset(A, 0, sizeof(float) * 2 * n);
    A[0] = A[1] = 0.9f;
    run(in, A, B, n);
    sl = sr = slr = 0;
    for (long i = 0; i < n; ++i) {
        sl += (double)B[2 * i] * B[2 * i];
        sr += (double)B[2 * i + 1] * B[2 * i + 1];
        slr += (double)B[2 * i] * B[2 * i + 1];
    }
    check(fabs(slr / sqrt(sl * sr)) < 0.5, "mono impulse -> wide reverb (|corr|)", fabs(slr / sqrt(sl * sr)));
    /* dry keeps its side */
    api->destroy_instance(in);
    in = make_default();
    setp(in, "depth", 0);
    setp(in, "mix", 0.4);
    settle(in);
    memset(A, 0, sizeof(float) * 2 * n);
    A[0] = 0.9f;
    run(in, A, B, 64);
    check(fabs(B[1]) < 1e-4 && fabs(B[0] - 0.9 * cos(0.2 * M_PI)) < 1e-3,
          "left-only input: dry stays on the left (right at t=0)", B[1]);
    api->destroy_instance(in);
}

/* ---------------------------------------------------------------- 9 */
static void test_mix(void) {
    puts("-- 9. MIX");
    void *in = make_default();
    setp(in, "mix", 0);
    settle(in);
    const long n = 3L * FS;
    fill_mix(A, n, 0.9f);
    int16_t blk[2 * BLOCK], orig[2 * BLOCK];
    int diff = 0;
    for (long f = 0; f + BLOCK <= n; f += BLOCK) {
        for (int i = 0; i < 2 * BLOCK; ++i) orig[i] = blk[i] = (int16_t)lrintf(A[2 * f + i] * 32767.0f);
        api->process_block(in, blk, BLOCK);
        diff += memcmp(blk, orig, sizeof blk) != 0;
    }
    check(diff == 0, "MIX 0 is a bit-exact bypass (blocks differing)", diff);
    api->destroy_instance(in);
    in = fresh(1);
    memset(A, 0, sizeof(float) * 2 * n);
    A[0] = A[1] = 0.9f;
    run(in, A, B, FS);
    check(peak_of(B, 0, 100) == 0.0, "MIX 100: no dry (silence before the predelay)", peak_of(B, 0, 100));
    check(rms_of(B, 0, FS) > 0.001, "MIX 100: the space is there", rms_of(B, 0, FS));
    api->destroy_instance(in);
    in = make_default();
    setp(in, "depth", 0);
    setp(in, "mix", 0.5);
    settle(in);
    run(in, A, B, 64);
    check(fabs(B[0] - 0.9 * M_SQRT1_2) < 1e-3, "MIX 50 % (DEPTH 0): dry at -3 dB (equal power)", B[0] / 0.9);
    api->destroy_instance(in);
}

/* ---------------------------------------------------------------- 10 */
static void test_lifecycle(void) {
    puts("-- 10. instances: pool, reuse, no allocation");
    void *ins[9];
    int made = 0;
    for (int i = 0; i < 9; ++i) {
        ins[i] = api->create_instance("", NULL);
        made += ins[i] != NULL;
    }
    check(made == 8, "eight instances, the ninth refused", made);
    for (int i = 0; i < 9; ++i)
        if (ins[i]) api->destroy_instance(ins[i]);
    /* reuse a dirty slot many times: it must clear itself and settle */
    int ok = 1, worst = 0;
    for (int round = 0; round < 200; ++round) {
        void *in = api->create_instance("", NULL);
        if (!in) { ok = 0; break; }
        if (round % 50 == 0) {
            int16_t blk[2 * BLOCK];
            memset(blk, 0, sizeof blk);
            api->process_block(in, blk, BLOCK);
            for (int i = 0; i < 2 * BLOCK; ++i) ok &= blk[i] == 0;
            int s = settle(in);
            worst = s > worst ? s : worst;
            ok &= s >= 0;
        }
        api->destroy_instance(in);
    }
    check(ok, "200 create/destroy cycles; reused slot starts silent and settles", ok);
    check(worst > 0 && worst * BLOCK < FS, "reused slot cleared, IR built and faded in within 1 s (blocks)", worst);
}

/* ---------------------------------------------------------------- params */
static void test_params(void) {
    puts("-- params, state, contract");
    void *in = make_default();
    check(!strcmp(getp(in, "space"), "CATHEDRAL") && !strcmp(getp(in, "freeze"), "OFF"), "defaults read as names", 0);
    sets(in, "space", "abyss");
    check(!strcmp(getp(in, "space"), "ABYSS"), "SPACE accepts a name", 0);
    sets(in, "space", "2");
    check(!strcmp(getp(in, "space"), "TUNNEL"), "SPACE accepts an index", 0);
    setp(in, "size", 9);
    check(atof(getp(in, "size")) == 4.0, "SIZE clamps to 4", atof(getp(in, "size")));
    setp(in, "decay", 0.1);
    check(atof(getp(in, "decay")) == 1.0, "DECAY clamps to 1 s", atof(getp(in, "decay")));
    sets(in, "depth", "nan");
    check(fabs(atof(getp(in, "depth")) - 0.6) < 1e-4, "NaN rejected", atof(getp(in, "depth")));
    setp(in, "depth", 0.25);
    setp(in, "mix", 0.8);
    sets(in, "freeze", "ON");
    char st[512];
    snprintf(st, sizeof st, "%s", getp(in, "state"));
    void *in2 = make_default();
    sets(in2, "state", st);
    check(!strcmp(getp(in2, "space"), "TUNNEL") && atof(getp(in2, "depth")) == 0.25 &&
          atof(getp(in2, "size")) == 4.0 && fabs(atof(getp(in2, "mix")) - 0.8) < 1e-4,
          "state round trip", 0);
    check(!strcmp(getp(in2, "freeze"), "OFF"), "FREEZE is never restored", 0);
    sets(in2, "state", "{\"space\":1}");
    check(!strcmp(getp(in2, "space"), "TUNNEL"), "incomplete state ignored", 0);
    check(strstr(getp(in, "ui_hierarchy"), "\"knobs\":[\"mix\",\"space\",\"depth\",\"size\",\"decay\","
                                            "\"diffusion\",\"darkness\",\"freeze\"]") != NULL,
          "ui_hierarchy: the eight knobs in order", 0);
    check(strstr(getp(in, "chain_params"), "\"VOID\"") != NULL, "chain_params served", 0);
    api->destroy_instance(in);
    api->destroy_instance(in2);
}

/* ---------------------------------------------------------------- 48k, CPU */
static void test_48k_and_cpu(void) {
    puts("-- 48 kHz host, CPU (this machine)");
    host.sample_rate = 48000;
    audio_fx_api_v2_t *a48 = move_audio_fx_init_v2(&host);
    void *in = a48->create_instance("", NULL);
    a48->set_param(in, "mix", "1");
    int s = settle(in);
    check(s > 0, "48 kHz instance settles", s);
    check(((ds_engine *)in)->gen.len == (int)(fminf(1.7f * 1.5f, 2.0f) * 48000),
          "48 kHz: IR generated at 48 kHz (2.0 s at SIZE 1.5)", ((ds_engine *)in)->gen.len);
    a48->destroy_instance(in);
    host.sample_rate = FS;
    move_audio_fx_init_v2(&host);

    in = make_default();
    setp(in, "decay", 30);
    setp(in, "size", 4);
    setp(in, "space", 4);   /* ABYSS: the longest IR */
    settle(in);
    fill_mix(A, 20L * FS, 0.5f);
    int16_t blk[2 * BLOCK];
    double total = 0, worst = 0;
    const int nb = (int)(20L * FS / BLOCK);
    for (int b = 0; b < nb; ++b) {
        for (int i = 0; i < 2 * BLOCK; ++i) blk[i] = (int16_t)lrintf(A[2 * b * BLOCK + i] * 32767.0f);
        if (b == nb / 2) setp(in, "space", 5);   /* include a crossfade */
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        api->process_block(in, blk, BLOCK);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
        total += us;
        worst = fmax(worst, us);
    }
    printf("     desktop: mean %.1f us / block, worst %.1f us (2902 us budget)\n", total / nb, worst);
    api->destroy_instance(in);
}

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "--quick");
    A = calloc(2 * SCRATCH, sizeof(float));
    B = calloc(2 * SCRATCH, sizeof(float));
    C = calloc(2 * SCRATCH, sizeof(float));
    host.api_version = 1;
    host.sample_rate = FS;
    host.frames_per_block = BLOCK;
    api = move_audio_fx_init_v2(&host);
    if (!api || !A || !B || !C) return 2;

    test_convolution();
    test_block_size();
    test_ir_switch();
    test_rt60();
    test_depth();
    if (!quick) test_freeze();
    test_extremes();
    test_stereo();
    test_mix();
    test_lifecycle();
    test_params();
    test_48k_and_cpu();

    free(A);
    free(B);
    free(C);
    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    puts("ALL ENGINE TESTS PASSED");
    return 0;
}
