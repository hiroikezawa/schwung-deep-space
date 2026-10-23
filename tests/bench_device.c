/*
 * Deep Space Convolution - CPU benchmark meant to run ON the Move (static
 * aarch64 binary).
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 * Runs the engine as the chain host does - 128-frame int16 blocks at
 * 44.1 kHz - and reports mean and worst block time for the heaviest
 * settings, including the blocks that build a new IR and crossfade to it.
 * Normal priority: it measures the code, it does not compete with Move's
 * realtime threads (pin it with taskset 0x7).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ds_engine.h"

static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static ds_engine e;

int main(void) {
    ds_fft_global_init();
    const struct { const char *name; float v[8]; int switching; } cfg[] = {
        {"defaults", {0.4f, 0, 0.6f, 1.5f, 8, 0.7f, 0.4f, 0}, 0},
        {"max (ABYSS 2 s IR)", {1, 4, 1, 4, 30, 1, 1, 0}, 0},
        {"SPACE every 1 s", {1, 0, 0.6f, 4, 30, 0.7f, 0.4f, 0}, 1},
        {"freeze", {1, 5, 0.6f, 4, 30, 1, 0.4f, 1}, 0},
        {"MIX 0 (asleep)", {0, 4, 1, 4, 30, 1, 1, 0}, 0},
    };
    int16_t buf[256];
    uint32_t rng = 1;
    int fresh = 1;
    for (unsigned c = 0; c < sizeof(cfg) / sizeof(cfg[0]); ++c) {
        ds_engine_init(&e, 44100.0f, fresh);
        fresh = 0;
        for (int p = 0; p < 8; ++p) ds_engine_set(&e, p, cfg[c].v[p]);
        ds_engine_snap(&e);
        double worst = 0, total = 0;
        const int warm = 400, n = 3445 * 2;   /* ~20 s measured */
        for (int i = 0; i < warm + n; ++i) {
            for (int k = 0; k < 256; ++k) {
                rng = rng * 1664525u + 1013904223u;
                buf[k] = (int16_t)((int32_t)rng >> 20);
            }
            if (cfg[c].switching && i % 345 == 0) ds_engine_set(&e, DS_P_SPACE, (float)((i / 345) % 6));
            double t0 = now_us();
            ds_engine_process(&e, buf, 128);
            double dt = now_us() - t0;
            if (i < warm) continue;
            total += dt;
            if (dt > worst) worst = dt;
        }
        printf("%-20s mean %6.1f us (%4.1f %% of 2902 us)  worst %6.1f us  IR builds %d\n", cfg[c].name,
               total / n, 100.0 * total / n / 2902.0, worst, e.ir_builds);
    }
    printf("instance size %.2f MB\n", sizeof(ds_engine) / 1048576.0);
    return 0;
}
