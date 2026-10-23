/*
 * Deep Space Convolution - offline renders for listening and inspection.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 *   render_demo <outdir>
 *     irs/<SPACE>.wav            the six synthetic IRs (SIZE 1.5, DECAY 30),
 *                                peak-normalised to -1 dBFS
 *     dry.wav                    the test groove
 *     space_<SPACE>.wav          the groove through each preset, defaults
 *     depth_0.wav / depth_100    CATHEDRAL, MIX 50 %, DEPTH 0 vs 100 %
 *     freeze.wav                 groove, FREEZE on at 4 s while it keeps playing, off at 12 s
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds_engine.h"
#include "signals.h"

#define FS 44100

static ds_engine e;
static int used;

static void setup(const float *v) {
    ds_engine_init(&e, FS, !used);
    used = 1;
    for (int p = 0; p < DS_P_COUNT; ++p) ds_engine_set(&e, p, v[p]);
    ds_engine_snap(&e);
    float l[128], r[128];
    for (int i = 0; i < 4000 && !ds_engine_settled(&e); ++i) {
        memset(l, 0, sizeof l);
        memset(r, 0, sizeof r);
        ds_engine_process_float(&e, l, r, 128);
    }
}

static void process(float *buf, long frames, long freeze_on, long freeze_off) {
    float l[128], r[128];
    for (long f = 0; f < frames; f += 128) {
        int n = frames - f < 128 ? (int)(frames - f) : 128;
        if (freeze_on >= 0 && f == freeze_on) ds_engine_set(&e, DS_P_FREEZE, 1);
        if (freeze_off >= 0 && f == freeze_off) ds_engine_set(&e, DS_P_FREEZE, 0);
        for (int i = 0; i < n; ++i) {
            l[i] = buf[2 * (f + i)];
            r[i] = buf[2 * (f + i) + 1];
        }
        ds_engine_process_float(&e, l, r, n);
        for (int i = 0; i < n; ++i) {
            buf[2 * (f + i)] = l[i];
            buf[2 * (f + i) + 1] = r[i];
        }
    }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[512];
    ds_fft_global_init();
    const long play = 8L * FS, total = 20L * FS;
    float *groove = calloc(2 * (size_t)total, sizeof(float));
    float *buf = calloc(2 * (size_t)total, sizeof(float));
    fill_mix(groove, play, 0.5f);
    snprintf(path, sizeof path, "%s/dry.wav", dir);
    wav_write(path, groove, (int)total, FS);

    static float irl[DS_IR_MAX], irr[DS_IR_MAX];
    for (int s = 0; s < DS_SPACE_COUNT; ++s) {
        ds_irgen g;
        ds_irgen_start(&g, s, 1.5f, 30.0f, FS, DS_IR_MAX);
        while (ds_irgen_run(&g, irl, irr, 4096)) {}
        float peak = 1e-9f, *ir = calloc(2 * (size_t)g.len, sizeof(float));
        for (int i = 0; i < g.len; ++i) peak = fmaxf(peak, fmaxf(fabsf(irl[i]), fabsf(irr[i])));
        const float gain = 0.89f / peak;   /* -1 dBFS peak */
        for (int i = 0; i < g.len; ++i) {
            ir[2 * i] = irl[i] * gain;
            ir[2 * i + 1] = irr[i] * gain;
        }
        snprintf(path, sizeof path, "%s/irs/%s.wav", dir, DS_SPACE_NAMES[s]);
        wav_write(path, ir, g.len, FS);
        free(ir);

        const float v[DS_P_COUNT] = {(float)s, 0.6f, 1.5f, 8.0f, 0.7f, 0.4f, 0.0f, 0.4f};
        setup(v);
        memcpy(buf, groove, sizeof(float) * 2 * total);
        process(buf, total, -1, -1);
        snprintf(path, sizeof path, "%s/space_%s.wav", dir, DS_SPACE_NAMES[s]);
        wav_write(path, buf, (int)total, FS);
    }
    for (int d = 0; d <= 1; ++d) {
        const float v[DS_P_COUNT] = {0, (float)d, 1.5f, 8.0f, 0.7f, 0.4f, 0.0f, 0.5f};
        setup(v);
        memcpy(buf, groove, sizeof(float) * 2 * total);
        process(buf, total, -1, -1);
        snprintf(path, sizeof path, "%s/depth_%d.wav", dir, d * 100);
        wav_write(path, buf, (int)total, FS);
    }
    {
        const float v[DS_P_COUNT] = {4, 0.6f, 2.5f, 12.0f, 0.8f, 0.4f, 0.0f, 0.5f};
        setup(v);
        fill_mix(buf, total, 0.5f);
        process(buf, total, 4L * FS / 128 * 128, 12L * FS / 128 * 128);
        snprintf(path, sizeof path, "%s/freeze.wav", dir);
        wav_write(path, buf, (int)total, FS);
    }
    free(groove);
    free(buf);
    printf("demo written to %s\n", dir);
    return 0;
}
