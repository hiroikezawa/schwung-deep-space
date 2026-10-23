/*
 * Deep Space Convolution - real FFT (radix-2, split into a half-size complex
 * transform plus the usual even/odd recombination).
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 */
#include "ds_fft.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define DS_FFT_MAX 2048

struct ds_fft {
    int m;
    int log2m;
    float tw_re[DS_FFT_MAX / 2], tw_im[DS_FFT_MAX / 2];   /* exp(-2 pi i j / m) */
    float rt_re[DS_FFT_MAX], rt_im[DS_FFT_MAX];           /* exp(-pi i k / m) */
    uint16_t rev[DS_FFT_MAX];
};

static ds_fft g_small, g_large;
static int g_ready;

static void build(ds_fft *f, int m) {
    f->m = m;
    f->log2m = 0;
    while ((1 << f->log2m) < m) ++f->log2m;
    for (int j = 0; j < m / 2; ++j) {
        double a = -2.0 * M_PI * j / m;
        f->tw_re[j] = (float)cos(a);
        f->tw_im[j] = (float)sin(a);
    }
    for (int k = 0; k < m; ++k) {
        double a = -M_PI * k / m;
        f->rt_re[k] = (float)cos(a);
        f->rt_im[k] = (float)sin(a);
    }
    for (int i = 0; i < m; ++i) {
        int r = 0;
        for (int b = 0; b < f->log2m; ++b)
            if (i & (1 << b)) r |= 1 << (f->log2m - 1 - b);
        f->rev[i] = (uint16_t)r;
    }
}

void ds_fft_global_init(void) {
    if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return;
    build(&g_small, 128);
    build(&g_large, 2048);
    __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
}

const ds_fft *ds_fft_get(int m) {
    return m == 128 ? &g_small : (m == 2048 ? &g_large : 0);
}

/* In-place complex FFT on interleaved z[2m]; sign -1 forward, +1 inverse
 * (unscaled). */
static void cfft(const ds_fft *f, float *z, int inverse) {
    const int m = f->m;
    for (int i = 0; i < m; ++i) {
        int r = f->rev[i];
        if (r > i) {
            float tr = z[2 * i], ti = z[2 * i + 1];
            z[2 * i] = z[2 * r];
            z[2 * i + 1] = z[2 * r + 1];
            z[2 * r] = tr;
            z[2 * r + 1] = ti;
        }
    }
    const float s = inverse ? -1.0f : 1.0f;
    for (int len = 2; len <= m; len <<= 1) {
        const int half = len >> 1;
        const int step = m / len;
        for (int i = 0; i < m; i += len) {
            float *a = z + 2 * i;
            float *b = z + 2 * (i + half);
            for (int j = 0; j < half; ++j) {
                const float wr = f->tw_re[j * step];
                const float wi = s * f->tw_im[j * step];
                const float br = b[2 * j], bi = b[2 * j + 1];
                const float vr = br * wr - bi * wi;
                const float vi = br * wi + bi * wr;
                const float ur = a[2 * j], ui = a[2 * j + 1];
                a[2 * j] = ur + vr;
                a[2 * j + 1] = ui + vi;
                b[2 * j] = ur - vr;
                b[2 * j + 1] = ui - vi;
            }
        }
    }
}

void ds_rfft(const ds_fft *f, const float *x, float *re, float *im, float *z) {
    const int m = f->m;
    /* z[n] = x[2n] + i x[2n+1] is exactly the interleaved layout of x. */
    if (z != x) memcpy(z, x, sizeof(float) * 2 * m);
    cfft(f, z, 0);
    re[0] = z[0] + z[1];
    im[0] = z[0] - z[1];
    for (int k = 1; k < m; ++k) {
        const float ar = z[2 * k], ai = z[2 * k + 1];
        const float br = z[2 * (m - k)], bi = -z[2 * (m - k) + 1];   /* conj Z[m-k] */
        const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
        /* O = (a - b) / (2i) */
        const float or_ = 0.5f * (ai - bi), oi = -0.5f * (ar - br);
        const float wr = f->rt_re[k], wi = f->rt_im[k];
        re[k] = er + (or_ * wr - oi * wi);
        im[k] = ei + (or_ * wi + oi * wr);
    }
}

void ds_irfft(const ds_fft *f, const float *re, const float *im, float *x, float *z) {
    const int m = f->m;
    {
        const float x0 = re[0], xm = im[0];
        z[0] = 0.5f * (x0 + xm);    /* E */
        z[1] = 0.5f * (x0 - xm);    /* O (real), i*O lands in the imaginary part */
    }
    for (int k = 1; k < m; ++k) {
        const float ar = re[k], ai = im[k];
        const float br = re[m - k], bi = -im[m - k];   /* conj X[m-k] */
        const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
        const float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);
        const float wr = f->rt_re[k], wi = -f->rt_im[k];   /* W^-k */
        const float or_ = dr * wr - di * wi, oi = dr * wi + di * wr;
        /* Z = E + i O */
        z[2 * k] = er - oi;
        z[2 * k + 1] = ei + or_;
    }
    cfft(f, z, 1);
    if (x != z) memcpy(x, z, sizeof(float) * 2 * m);
}
