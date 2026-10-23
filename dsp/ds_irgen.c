/*
 * Deep Space Convolution - synthetic stereo impulse responses.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 * Model, per channel, per sample:
 *
 *   noise   = sqrt(1-w) * shared + sqrt(w) * own        (w = stereo width)
 *   tail    = LP(noise) * env_lo + (noise - LP(noise)) * env_hi
 *             (two exponential decays split at a crossover: frequency-
 *             dependent absorption; the high band dies first)
 *           * bloom(t) * slow amplitude drift
 *   + early reflections (sparse signed impulses, separate per channel)
 *   + flutter echoes (periodic impulses between parallel walls)
 *   -> two one-pole lowpasses (the room's overall tone), one-pole highpass
 *   + decaying sinusoids (structural / metallic modes)
 *   -> raised-cosine fade over the last quarter.
 */
#include "ds_irgen.h"

#include <math.h>
#include <string.h>

#include "ds_dsp.h"
#include "ds_params.h"

typedef struct {
    float len_s;          /* IR length at SIZE 1 */
    float rt_lo, rt_hi;   /* tail RT60, low / high band, seconds */
    float xover;          /* Hz */
    float gap_ms;         /* initial time delay gap (first reflection) */
    float er_end_ms;      /* early reflections end */
    int er_count;
    float er_level;
    float er_shape;       /* <1 crowds the reflections early */
    float onset_ms;       /* bloom time constant of the diffuse tail */
    float width;          /* 0 mono .. 1 fully decorrelated */
    float flutter_ms, flutter_amt;
    float tone_lp, tone_hp;
    float am_depth, am_rate;
    int nmodes;
    float mode_hz[DS_IR_MODES];
    float mode_amt, mode_rt;
    uint32_t seed;
    float trim_db;        /* loudness match between presets (pink noise, DEPTH 50 %) */
} ds_space;

static const ds_space k_spaces[DS_SPACE_COUNT] = {
    /* CATHEDRAL: late first reflection, stone-bright early field, long
     * medium-dark tail, a slow bloom. */
    {1.7f, 4.2f, 2.2f, 1800.0f, 22.0f, 160.0f, 30, 0.60f, 0.8f, 70.0f, 0.85f,
     0.0f, 0.0f, 8500.0f, 60.0f, 0.08f, 0.6f, 0, {0}, 0.0f, 0.0f, 0xC47ED2A1u, -2.6f},
    /* CAVERN: very early, crowded irregular reflections, dark, wandering. */
    {1.8f, 4.8f, 1.3f, 1000.0f, 9.0f, 240.0f, 44, 0.75f, 0.45f, 30.0f, 0.70f,
     0.0f, 0.0f, 5200.0f, 35.0f, 0.35f, 1.3f, 0, {0}, 0.0f, 0.0f, 0xCA7E2911u, -7.4f},
    /* TUNNEL: narrow, flutter between parallel walls, tube resonances. */
    {1.6f, 3.4f, 1.5f, 1500.0f, 6.0f, 70.0f, 10, 0.50f, 1.0f, 20.0f, 0.30f,
     41.0f, 0.70f, 6500.0f, 110.0f, 0.05f, 0.4f, 3, {187.0f, 374.0f, 561.0f}, 0.12f, 1.2f, 0x7A11E1B3u, -3.8f},
    /* HANGAR: big distinct slaps, bright, metallic ringing modes. */
    {1.4f, 3.4f, 2.8f, 2500.0f, 30.0f, 340.0f, 9, 1.00f, 1.0f, 12.0f, 0.80f,
     0.0f, 0.0f, 15000.0f, 55.0f, 0.04f, 0.5f, 6, {143.0f, 377.0f, 619.0f, 1013.0f, 1871.0f, 2903.0f},
     0.18f, 2.0f, 0x4A26A3E5u, -1.4f},
    /* ABYSS: no reflections at all, a slow swell out of the dark, sub heavy. */
    {2.0f, 7.5f, 1.1f, 650.0f, 40.0f, 0.0f, 0, 0.0f, 1.0f, 480.0f, 0.90f,
     0.0f, 0.0f, 2600.0f, 22.0f, 0.25f, 0.25f, 0, {0}, 0.0f, 0.0f, 0xAB7550D7u, -10.5f},
    /* VOID: abstract - fully decorrelated, airy, no low end, no early
     * structure, a long fade-in. */
    {2.0f, 6.0f, 4.5f, 3000.0f, 15.0f, 0.0f, 0, 0.0f, 1.0f, 260.0f, 1.00f,
     0.0f, 0.0f, 12000.0f, 160.0f, 0.15f, 0.15f, 0, {0}, 0.0f, 0.0f, 0x701D0A0Bu, +0.8f},
};

static float decay_per_sample(float rt60, float fs) {
    return expf(-6.907755f / (rt60 * fs));
}

void ds_irgen_start(ds_irgen *g, int space, float size, float decay, float fs, int max_len) {
    memset(g, 0, sizeof *g);
    if (space < 0 || space >= DS_SPACE_COUNT) space = 0;
    const ds_space *p = &k_spaces[space];
    const float s = ds_clampf(size, 0.5f, 4.0f);
    const float rt_scale = powf(s, 0.8f);
    float rt_lo = p->rt_lo * rt_scale;
    float ratio = 1.0f;
    if (decay > 0.0f && rt_lo > decay) {
        ratio = decay / rt_lo;
        rt_lo = decay;
    }
    const float rt_hi = p->rt_hi * rt_scale * ratio;

    float len_s = p->len_s * s;
    float by_decay = 1.3f * rt_lo + 0.3f;   /* past ~-50 dB the FDN carries it */
    if (len_s > by_decay) len_s = by_decay;
    if (len_s > 2.0f) len_s = 2.0f;
    if (len_s < 0.25f) len_s = 0.25f;
    g->fs = fs;
    g->len = (int)(len_s * fs);
    if (g->len > max_len) g->len = max_len;
    g->fade_start = g->len - g->len / 4;
    g->trim = powf(10.0f, p->trim_db / 20.0f);

    g->rng_shared = p->seed;
    g->rng_own[0] = p->seed ^ 0x9E3779B9u;
    g->rng_own[1] = p->seed ^ 0x85EBCA6Bu;
    g->rng_am = p->seed ^ 0xC2B2AE35u;
    g->rng_flutter = p->seed ^ 0x27D4EB2Fu;
    g->c_shared = sqrtf(1.0f - p->width);
    g->c_own = sqrtf(p->width);

    g->xo_k = ds_lp_coef(p->xover, fs);
    g->env_lo = g->env_hi = 1.0f;
    g->dec_lo = decay_per_sample(rt_lo, fs);
    g->dec_hi = decay_per_sample(rt_hi, fs);
    g->tail_start = (int)(p->gap_ms * s * 0.001f * fs);
    g->onset = 1.0f;
    g->onset_k = expf(-1.0f / (p->onset_ms * s * 0.001f * fs));

    g->am = 1.0f;
    g->am_target = 1.0f;
    g->am_depth = p->am_depth;
    g->am_period = (int)(fs / (p->am_rate / s));
    if (g->am_period < 64) g->am_period = 64;
    g->am_k = 4.0f / (float)g->am_period;

    g->lp_k = ds_lp_coef(p->tone_lp, fs);
    g->hp_k = ds_lp_coef(p->tone_hp, fs);

    g->nmodes = p->nmodes;
    for (int m = 0; m < p->nmodes; ++m) {
        const float hz = p->mode_hz[m] / sqrtf(s);
        const float w = 2.0f * DS_PI * hz / fs;
        const float ph = 2.0f * DS_PI * ds_rng_unit(&g->rng_flutter);
        g->mode_c[m] = cosf(ph);
        g->mode_s[m] = sinf(ph);
        g->mode_rc[m] = cosf(w);
        g->mode_rs[m] = sinf(w);
        g->mode_amp[m] = p->mode_amt * (0.6f + 0.4f * ds_rng_unit(&g->rng_flutter));
    }
    g->mode_env = 1.0f;
    g->mode_dec = decay_per_sample(p->mode_rt * rt_scale * ratio, fs);

    if (p->flutter_ms > 0.0f) {
        g->flutter_period = p->flutter_ms * s * 0.001 * fs;
        g->flutter_next = g->tail_start;
        g->flutter_amp = p->flutter_amt * 5.0f;
    } else {
        g->flutter_next = -1.0;
    }

    /* Early reflections: separate times per channel, density rising with
     * time, amplitude falling with distance, random sign. */
    for (int ch = 0; ch < 2; ++ch) {
        uint32_t r = p->seed ^ (ch ? 0x165667B1u : 0xD3A2646Cu);
        int n = p->er_count > DS_IR_TAPS ? DS_IR_TAPS : p->er_count;
        const float t0 = p->gap_ms * s * 0.001f, t1 = p->er_end_ms * s * 0.001f;
        int count = 0;
        for (int i = 0; i < n; ++i) {
            float u = (i + ds_rng_unit(&r)) / (float)n;
            float t = t0 + (t1 - t0) * powf(u, p->er_shape);
            int pos = (int)(t * fs);
            if (pos >= g->len) continue;
            float a = p->er_level * 8.0f * (0.45f + 0.55f * ds_rng_unit(&r)) * sqrtf(t0 / (t > t0 ? t : t0));
            a *= (ds_rng_next(&r) & 1) ? 1.0f : -1.0f;
            g->tap_pos[ch][count] = pos;
            g->tap_amp[ch][count] = a;
            ++count;
        }
        /* sort by time (n <= 48, insertion sort) */
        for (int i = 1; i < count; ++i) {
            int pp = g->tap_pos[ch][i];
            float aa = g->tap_amp[ch][i];
            int j = i - 1;
            while (j >= 0 && g->tap_pos[ch][j] > pp) {
                g->tap_pos[ch][j + 1] = g->tap_pos[ch][j];
                g->tap_amp[ch][j + 1] = g->tap_amp[ch][j];
                --j;
            }
            g->tap_pos[ch][j + 1] = pp;
            g->tap_amp[ch][j + 1] = aa;
        }
        g->ntaps[ch] = count;
    }
}

int ds_irgen_run(ds_irgen *g, float *ir_l, float *ir_r, int n) {
    if (g->pos + n > g->len) n = g->len - g->pos;
    if (n <= 0) return 0;
    float *out[2] = {ir_l, ir_r};
    for (int i = 0; i < n; ++i) {
        const int pos = g->pos + i;
        float exc[2] = {0.0f, 0.0f};

        if (pos >= g->tail_start) {
            const float shared = ds_rng_gauss(&g->rng_shared);
            if (--g->am_count <= 0) {
                g->am_count = g->am_period;
                g->am_target = 1.0f + g->am_depth * ds_rng_bipolar(&g->rng_am);
            }
            g->am += (g->am_target - g->am) * g->am_k;
            const float bloom = 1.0f - g->onset;
            g->onset *= g->onset_k;
            for (int ch = 0; ch < 2; ++ch) {
                const float nz = g->c_shared * shared + g->c_own * ds_rng_gauss(&g->rng_own[ch]);
                g->xo_s[ch] += (nz - g->xo_s[ch]) * g->xo_k;
                const float lo = g->xo_s[ch], hi = nz - lo;
                exc[ch] = (lo * g->env_lo + hi * g->env_hi) * bloom * g->am;
            }
            g->env_lo *= g->dec_lo;
            g->env_hi *= g->dec_hi;
        }
        if (g->flutter_next >= 0.0 && pos >= (int)g->flutter_next) {
            const float a = g->flutter_amp * g->env_lo;
            exc[0] += a;
            exc[1] += a;
            g->flutter_next += g->flutter_period * (0.97 + 0.06 * ds_rng_unit(&g->rng_flutter));
        }
        for (int ch = 0; ch < 2; ++ch) {
            while (g->tap_idx[ch] < g->ntaps[ch] && g->tap_pos[ch][g->tap_idx[ch]] <= pos) {
                if (g->tap_pos[ch][g->tap_idx[ch]] == pos) exc[ch] += g->tap_amp[ch][g->tap_idx[ch]];
                ++g->tap_idx[ch];
            }
        }
        float mode = 0.0f;
        if (g->nmodes && pos >= g->tail_start) {
            for (int m = 0; m < g->nmodes; ++m) {
                const float c = g->mode_c[m] * g->mode_rc[m] - g->mode_s[m] * g->mode_rs[m];
                const float s = g->mode_s[m] * g->mode_rc[m] + g->mode_c[m] * g->mode_rs[m];
                g->mode_c[m] = c;
                g->mode_s[m] = s;
                mode += g->mode_amp[m] * s;
            }
            mode *= g->mode_env * (1.0f - g->onset);
            g->mode_env *= g->mode_dec;
        }
        float fade = 1.0f;
        if (pos >= g->fade_start) {
            const float t = (float)(pos - g->fade_start) / (float)(g->len - g->fade_start);
            fade = 0.5f + 0.5f * cosf(DS_PI * t);
        }
        for (int ch = 0; ch < 2; ++ch) {
            g->lp1[ch] += (exc[ch] - g->lp1[ch]) * g->lp_k;
            g->lp2[ch] += (g->lp1[ch] - g->lp2[ch]) * g->lp_k;
            g->hp_s[ch] += (g->lp2[ch] - g->hp_s[ch]) * g->hp_k;
            /* modes ring in opposite polarity per side: a wide, not a centred, ring */
            const float y = ((g->lp2[ch] - g->hp_s[ch]) + (ch ? -mode : mode)) * fade;
            out[ch][pos] = y;
            g->energy[ch] += (double)y * y;
        }
    }
    /* keep the mode rotations on the unit circle */
    for (int m = 0; m < g->nmodes; ++m) {
        const float r = 1.0f / sqrtf(g->mode_c[m] * g->mode_c[m] + g->mode_s[m] * g->mode_s[m]);
        g->mode_c[m] *= r;
        g->mode_s[m] *= r;
    }
    g->pos += n;
    return n;
}

float ds_irgen_decay_used(int space, float size, float decay) {
    if (space < 0 || space >= DS_SPACE_COUNT) space = 0;
    const float rt = k_spaces[space].rt_lo * powf(ds_clampf(size, 0.5f, 4.0f), 0.8f);
    return decay < rt ? decay : rt;
}

float ds_irgen_gain(const ds_irgen *g) {
    const double e = 0.5 * (g->energy[0] + g->energy[1]);
    return e > 1e-12 ? g->trim * (float)(1.0 / sqrt(e)) : 0.0f;
}
