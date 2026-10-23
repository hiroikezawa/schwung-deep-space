/*
 * Deep Space Convolution - synthetic test material shared by the tests and
 * the demo renderer, plus a tiny WAV writer. All generated here; nothing sampled.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 */
#ifndef DS_SIGNALS_H
#define DS_SIGNALS_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SIG_PI 3.14159265358979323846

static uint32_t sig_rng = 12345u;
static inline float sig_noise(void) {
    sig_rng ^= sig_rng << 13; sig_rng ^= sig_rng >> 17; sig_rng ^= sig_rng << 5;
    return (float)(int32_t)sig_rng * (1.0f / 2147483648.0f);
}

/* Stereo interleaved buffers of `frames` frames. Each generator ADDS a hit at
 * frame `at` so a pattern can be built from several calls. */
static void sig_kick(float *buf, int frames, int at, float fs, float amp) {
    double ph = 0.0;
    for (int i = at; i < frames && i < at + (int)(0.6f * fs); ++i) {
        double t = (i - at) / fs;
        double f = 48.0 + 110.0 * exp(-t * 28.0);
        ph += 2.0 * SIG_PI * f / fs;
        double env = exp(-t * 6.5);
        double click = (t < 0.004) ? (1.0 - t / 0.004) * sig_noise() * 0.3 : 0.0;
        float v = (float)(amp * (sin(ph) * env + click));
        buf[2 * i] += v;
        buf[2 * i + 1] += v;
    }
}

static void sig_snare(float *buf, int frames, int at, float fs, float amp) {
    double ph = 0.0;
    float lp = 0.0f;
    for (int i = at; i < frames && i < at + (int)(0.35f * fs); ++i) {
        double t = (i - at) / fs;
        ph += 2.0 * SIG_PI * 190.0 / fs;
        float n = sig_noise();
        lp += 0.5f * (n - lp);
        float noise = n - lp;   /* a bright noise band */
        double v = amp * (0.45 * sin(ph) * exp(-t * 30.0) + 0.7 * noise * exp(-t * 14.0));
        buf[2 * i] += (float)v;
        buf[2 * i + 1] += (float)v;
    }
}

/* A strummed six-string chord, Karplus-Strong plucks (E minor-ish). */
static void sig_chord(float *buf, int frames, int at, float fs, float amp) {
    static const double hz[6] = {82.41, 123.47, 164.81, 196.00, 246.94, 329.63};
    static float line[6][2048];
    for (int s = 0; s < 6; ++s) {
        int len = (int)(fs / hz[s]);
        if (len > 2047) len = 2047;
        for (int k = 0; k < len; ++k) line[s][k] = sig_noise();
        int start = at + s * (int)(0.012f * fs);
        int pos = 0;
        for (int i = start; i < frames && i < start + (int)(2.5f * fs); ++i) {
            int nx = (pos + 1) % len;
            float y = 0.4985f * (line[s][pos] + line[s][nx]);
            line[s][pos] = y;
            pos = nx;
            float pan = (float)s / 5.0f;
            buf[2 * i] += amp * y * (1.0f - 0.4f * pan) * 0.35f;
            buf[2 * i + 1] += amp * y * (0.6f + 0.4f * pan) * 0.35f;
        }
    }
}

static void sig_noise_burst(float *buf, int frames, int at, float fs, float amp, float secs) {
    for (int i = at; i < frames && i < at + (int)(secs * fs); ++i) {
        double t = (i - at) / fs;
        double env = t < 0.005 ? t / 0.005 : exp(-(t - 0.005) * 3.0);
        buf[2 * i] += (float)(amp * env * sig_noise());
        buf[2 * i + 1] += (float)(amp * env * sig_noise());
    }
}

/* Voss-McCartney-ish pink noise (filter approximation), continuous. */
static void sig_pink(float *buf, int frames, float amp) {
    float b0 = 0, b1 = 0, b2 = 0;
    for (int i = 0; i < frames; ++i) {
        float w = sig_noise();
        b0 = 0.99765f * b0 + w * 0.0990460f;
        b1 = 0.96300f * b1 + w * 0.2965164f;
        b2 = 0.57000f * b2 + w * 1.0526913f;
        float p = (b0 + b1 + b2 + w * 0.1848f) * 0.25f;
        buf[2 * i] = amp * p;
        buf[2 * i + 1] = amp * p;
    }
}

static void sig_sine(float *buf, int frames, float fs, float hz, float amp) {
    for (int i = 0; i < frames; ++i) {
        float v = amp * (float)sin(2.0 * SIG_PI * hz * i / fs);
        buf[2 * i] = v;
        buf[2 * i + 1] = v;
    }
}

static int wav_write(const char *path, const float *buf, int frames, int fs) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t data = (uint32_t)frames * 4u, riff = 36u + data, rate = (uint32_t)fs, br = rate * 4u;
    uint16_t fmt = 1, ch = 2, ba = 4, bits = 16;
    uint32_t sixteen = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&sixteen, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (int i = 0; i < 2 * frames; ++i) {
        float x = buf[i] * 32767.0f;
        if (x > 32767.0f) x = 32767.0f;
        if (x < -32768.0f) x = -32768.0f;
        int16_t s = (int16_t)lrintf(x);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return 0;
}

/* A drum + chord groove at 120 BPM over `frames` frames, peak-normalised to
 * amp: kick on every beat, snare on 2 and 4, a strummed chord every bar. */
static void fill_mix(float *buf, long frames, float amp) {
    memset(buf, 0, sizeof(float) * 2 * (size_t)frames);
    const float fs = 44100.0f;
    const long beat = 22050;
    for (long b = 0; b * beat < frames; ++b) {
        sig_kick(buf, (int)frames, (int)(b * beat), fs, 0.8f);
        if (b % 2 == 1) sig_snare(buf, (int)frames, (int)(b * beat), fs, 0.6f);
        if (b % 4 == 0) sig_chord(buf, (int)frames, (int)(b * beat), fs, 0.5f);
    }
    float peak = 1e-9f;
    for (long i = 0; i < 2 * frames; ++i) peak = fabsf(buf[i]) > peak ? fabsf(buf[i]) : peak;
    for (long i = 0; i < 2 * frames; ++i) buf[i] *= amp / peak;
}

static void fill_pink(float *buf, long frames, float amp) { sig_pink(buf, (int)frames, amp); }

#endif
