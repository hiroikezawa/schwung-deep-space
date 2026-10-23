/*
 * Deep Space Convolution - 16-line feedback delay network (late reverb).
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 */
#include "ds_fdn.h"

#include <math.h>
#include <string.h>

#include "ds_dsp.h"

/* Line lengths at SIZE 1, milliseconds: roughly geometric, no two sharing a
 * simple ratio. */
static const float k_base_ms[DS_FDN_LINES] = {
    17.3f, 19.9f, 22.7f, 25.4f, 28.9f, 32.1f, 35.8f, 39.7f,
    44.3f, 49.1f, 54.8f, 60.6f, 67.4f, 74.9f, 83.1f, 96.7f};

/* Modulation rates, Hz: slow and mutually incommensurate. */
static const float k_mod_hz[DS_FDN_LINES] = {
    0.071f, 0.113f, 0.157f, 0.089f, 0.211f, 0.131f, 0.263f, 0.097f,
    0.179f, 0.229f, 0.061f, 0.193f, 0.149f, 0.241f, 0.083f, 0.127f};

/* Sylvester Hadamard row k, entry i. */
static inline float had(int k, int i) {
    return (__builtin_popcount((unsigned)(k & i)) & 1) ? -1.0f : 1.0f;
}

void ds_fdn_init(ds_fdn *f, float fs) {
    f->w = 0;
    f->fs = fs;
    for (int i = 0; i < DS_FDN_LINES; ++i) {
        f->base[i] = k_base_ms[i] * 0.001f * fs;
        f->lp[i] = 0.0f;
        f->g[i] = 0.0f;
        f->b[i] = 0.0f;
        const float ph = 0.37f * (float)i;
        f->mod_c[i] = cosf(ph);
        f->mod_s[i] = sinf(ph);
        const float w = 2.0f * DS_PI * k_mod_hz[i] / fs;
        f->mod_rc[i] = cosf(w);
        f->mod_rs[i] = sinf(w);
    }
    f->scale = f->scale_target = 1.0f;
    f->scale_k = ds_tau_coef(0.35f, fs);
    f->mod_depth = f->mod_depth_target = 0.0f;
    f->dc_x[0] = f->dc_x[1] = f->dc_y[0] = f->dc_y[1] = 0.0f;
    f->dc_r = 1.0f - 2.0f * DS_PI * 8.0f / fs;
    f->clamps = 0;
}

void ds_fdn_set_scale(ds_fdn *f, float scale, int snap) {
    f->scale_target = scale;
    if (snap) f->scale = scale;
}

static float line_gain(float len, float fs, float rt) {
    return expf(-6.907755f * len / (fs * rt));
}

void ds_fdn_set_decay(ds_fdn *f, float rt60, float hf_ratio) {
    const float rt_hf = rt60 * ds_clampf(hf_ratio, 0.02f, 1.0f);
    for (int i = 0; i < DS_FDN_LINES; ++i) {
        const float len = f->base[i] * f->scale;
        const float g = line_gain(len, f->fs, rt60);
        const float gh = line_gain(len, f->fs, rt_hf);
        const float r = gh / g;
        f->g[i] = g;
        f->b[i] = (1.0f - r) / (1.0f + r);
    }
}

void ds_fdn_set_mod(ds_fdn *f, float amount) {
    /* 1.5 .. 9 samples at 44.1 kHz: enough to smear modes, not to detune */
    f->mod_depth_target = (1.5f + 7.5f * ds_clampf(amount, 0.0f, 1.0f)) * f->fs / 44100.0f;
}

float ds_fdn_mean_gain(const ds_fdn *f, float rt60) {
    float sum = 0.0f;
    for (int i = 0; i < DS_FDN_LINES; ++i) sum += line_gain(f->base[i] * f->scale, f->fs, rt60);
    return sum / DS_FDN_LINES;
}

static inline float hermite(const float *buf, float pos) {
    const int ip = (int)floorf(pos);
    const float t = pos - (float)ip;
    const float x0 = buf[(ip - 1) & DS_FDN_MASK], x1 = buf[ip & DS_FDN_MASK];
    const float x2 = buf[(ip + 1) & DS_FDN_MASK], x3 = buf[(ip + 2) & DS_FDN_MASK];
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + x1;
}

static inline void fwht16(float *v) {
    for (int h = 1; h < DS_FDN_LINES; h <<= 1) {
        for (int i = 0; i < DS_FDN_LINES; i += h << 1) {
            for (int j = i; j < i + h; ++j) {
                const float a = v[j], b = v[j + h];
                v[j] = a + b;
                v[j + h] = a - b;
            }
        }
    }
}

static inline float safety(float x, int *hits) {
    const float ax = fabsf(x);
    if (ax <= 4.0f) return x;
    ++*hits;
    const float over = ax - 4.0f;
    return copysignf(4.0f + over / (1.0f + over), x);
}

void ds_fdn_process(ds_fdn *f, const float *in_l, const float *in_r, float *out_l, float *out_r, int n) {
    float v[DS_FDN_LINES];
    const float md_k = 0.002f;
    for (int s = 0; s < n; ++s) {
        f->scale += ((double)f->scale_target - f->scale) * f->scale_k;
        if (fabs(f->scale - f->scale_target) < 1e-7) f->scale = f->scale_target;
        const float scale = (float)f->scale;
        f->mod_depth += (f->mod_depth_target - f->mod_depth) * md_k;
        const float wpos = (float)f->w;
        for (int i = 0; i < DS_FDN_LINES; ++i) {
            const float c = f->mod_c[i] * f->mod_rc[i] - f->mod_s[i] * f->mod_rs[i];
            const float sn = f->mod_s[i] * f->mod_rc[i] + f->mod_c[i] * f->mod_rs[i];
            f->mod_c[i] = c;
            f->mod_s[i] = sn;
            const float d = f->base[i] * scale + f->mod_depth * (1.0f + sn);
            const float y = hermite(f->line[i], wpos - d);
            f->lp[i] = (1.0f - f->b[i]) * y + f->b[i] * f->lp[i];
            v[i] = f->g[i] * f->lp[i];
        }
        float ol = 0.0f, or_ = 0.0f;
        for (int i = 0; i < DS_FDN_LINES; ++i) {
            ol += v[i] * had(5, i);
            or_ += v[i] * had(10, i);
        }
        fwht16(v);
        const float il = in_l[s] * 0.25f, ir = in_r[s] * 0.25f;
        for (int i = 0; i < DS_FDN_LINES; ++i)
            f->line[i][f->w] = safety(0.25f * v[i] + il * had(3, i) + ir * had(12, i), &f->clamps);
        f->w = (f->w + 1) & DS_FDN_MASK;

        ol *= 0.25f;
        or_ *= 0.25f;
        const float yl = ol - f->dc_x[0] + f->dc_r * f->dc_y[0];
        const float yr = or_ - f->dc_x[1] + f->dc_r * f->dc_y[1];
        f->dc_x[0] = ol;
        f->dc_x[1] = or_;
        f->dc_y[0] = yl;
        f->dc_y[1] = yr;
        out_l[s] = yl;
        out_r[s] = yr;
    }
    for (int i = 0; i < DS_FDN_LINES; ++i) {
        const float r = 1.0f / sqrtf(f->mod_c[i] * f->mod_c[i] + f->mod_s[i] * f->mod_s[i]);
        f->mod_c[i] *= r;
        f->mod_s[i] *= r;
    }
}
