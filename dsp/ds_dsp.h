/*
 * Deep Space Convolution - small shared DSP helpers.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 */
#ifndef DS_DSP_H
#define DS_DSP_H

#include <math.h>
#include <stdint.h>

#define DS_PI 3.14159265358979323846f

static inline float ds_clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float ds_lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

/* One-pole coefficient for a time constant in seconds: y += (x - y) * k. */
static inline float ds_tau_coef(float seconds, float rate) {
    if (seconds <= 0.0f) return 1.0f;
    return 1.0f - expf(-1.0f / (seconds * rate));
}

/* One-pole lowpass coefficient for a cutoff in Hz (matched at DC, -3 dB
 * near fc for fc well below Nyquist). */
static inline float ds_lp_coef(float hz, float rate) {
    float fc = ds_clampf(hz, 1.0f, 0.49f * rate);
    return 1.0f - expf(-2.0f * DS_PI * fc / rate);
}

/* xorshift32: deterministic noise, so a preset's synthetic IR is the same
 * on every machine and every run. */
static inline uint32_t ds_rng_next(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Uniform in [-1, 1). */
static inline float ds_rng_bipolar(uint32_t *s) {
    return (float)(int32_t)ds_rng_next(s) * (1.0f / 2147483648.0f);
}

/* Uniform in [0, 1). */
static inline float ds_rng_unit(uint32_t *s) {
    return (float)(ds_rng_next(s) >> 8) * (1.0f / 16777216.0f);
}

/* Approximately Gaussian (sum of three uniforms), unit variance. */
static inline float ds_rng_gauss(uint32_t *s) {
    return (ds_rng_bipolar(s) + ds_rng_bipolar(s) + ds_rng_bipolar(s));
}

/* Flush-to-zero for the duration of a block, restored afterwards: Move's
 * thread gets back exactly the FP mode it had. */
typedef struct {
    uint64_t saved;
} ds_ftz;

static inline void ds_ftz_begin(ds_ftz *f) {
#if defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    f->saved = fpcr;
    fpcr |= (1ull << 24);   /* FZ */
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__x86_64__) || defined(__i386__)
    unsigned int csr;
    __asm__ volatile("stmxcsr %0" : "=m"(csr));
    f->saved = csr;
    csr |= 0x8040;          /* FTZ | DAZ */
    __asm__ volatile("ldmxcsr %0" : : "m"(csr));
#else
    f->saved = 0;
#endif
}

static inline void ds_ftz_end(const ds_ftz *f) {
#if defined(__aarch64__)
    uint64_t fpcr = f->saved;
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__x86_64__) || defined(__i386__)
    unsigned int csr = (unsigned int)f->saved;
    __asm__ volatile("ldmxcsr %0" : : "m"(csr));
#else
    (void)f;
#endif
}

#endif
