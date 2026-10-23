/*
 * Deep Space Convolution - the whole effect.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 * What DEPTH moves (all together, smoothed):
 *   predelay            3 ms .. 180 ms (x 0.85..1.9 with SIZE)
 *   early / late        ER gain 1.0 -> 0.3, late gain 0.5 -> 1.3
 *   air absorption      wet lowpass 22 kHz (off) -> ~2.6 kHz
 *   reflection density  ER and FDN-input diffusion rise with depth
 *   direct / reverb     the direct sound loses up to 12 dB and its top end
 *                       (only as MIX brings the space in: MIX 0 is bypass)
 *   loudness            the wet gains up to +4.4 dB to make up for what the
 *                       direct sound and the air absorption take away
 */
#include "ds_engine.h"

#include <math.h>
#include <string.h>

#include "ds_dsp.h"

#define DS_GEN_CHUNK 4096
#define DS_CLEAR_CHUNK (128u * 1024u)
#define DS_WAKE_CHUNK (512u * 1024u)    /* waking: the listener is waiting */
/* RT60 while frozen: -0.6 dB in five minutes; the slow loss is what keeps
 * the loop strictly stable. */
#define DS_FREEZE_RT 30000.0f

/* Allpass lengths, ms. Left and right differ so the two sides decorrelate. */
static const float k_ap_in_ms[2][DS_AP_IN] = {{4.77f, 3.59f, 12.73f, 9.31f}, {4.31f, 3.83f, 11.97f, 9.77f}};
static const float k_ap_er_ms[2][DS_AP_ER] = {{1.31f, 2.93f}, {1.53f, 2.71f}};

static void ap_init(ds_allpass *a, float ms, float fs) {
    memset(a->buf, 0, sizeof a->buf);
    a->w = 0;
    a->len = (int)(ms * 0.001f * fs);
    if (a->len < 1) a->len = 1;
    if (a->len > DS_AP_SIZE - 1) a->len = DS_AP_SIZE - 1;
}

static inline float ap_run(ds_allpass *a, float x, float g) {
    const float s = a->buf[(a->w - a->len) & (DS_AP_SIZE - 1)];
    const float v = x + g * s;
    a->buf[a->w] = v;
    a->w = (a->w + 1) & (DS_AP_SIZE - 1);
    return s - g * v;
}

/* One-pole lowpass coefficient that is exactly 1 (bypass) near the top. */
static float air_coef(float hz, float fs) {
    if (hz >= 0.45f * fs) return 1.0f;
    return ds_lp_coef(hz, fs);
}

/* Equal power, exact at the ends: MIX 0 is a bit-exact bypass, MIX 100 has
 * no dry at all. */
static float target_dry(float mix) {
    return mix <= 0.0f ? 1.0f : (mix >= 1.0f ? 0.0f : cosf(0.5f * DS_PI * mix));
}
static float target_wet(float mix) {
    return mix <= 0.0f ? 0.0f : (mix >= 1.0f ? 1.0f : sinf(0.5f * DS_PI * mix));
}

/* Signal-history state only: filters, delay positions. Keeps the IR. */
static void history_state(ds_engine *e) {
    const float fs = e->fs;
    memset(e->wet_fifo, 0, sizeof e->wet_fifo);
    for (int ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < DS_AP_IN; ++i) ap_init(&e->ap_in[ch][i], k_ap_in_ms[ch][i], fs);
        for (int i = 0; i < DS_AP_ER; ++i) ap_init(&e->ap_er[ch][i], k_ap_er_ms[ch][i], fs);
        e->air1[ch] = e->air2[ch] = 0.0f;
        e->wdc_x[ch] = e->wdc_y[ch] = 0.0f;
        e->dir_lp[ch] = 0.0f;
    }
    e->pd_w = 0;
    e->lim_env = 0.0f;
    ds_fdn_init(&e->fdn, fs);
}

static void small_state(ds_engine *e) {
    e->fifo_pos = 0;
    memset(e->in_fifo, 0, sizeof e->in_fifo);
    history_state(e);
    ds_conv_init_small(&e->conv);
    e->built_space = -1;
    e->build_set = -1;
    e->asleep = 0;
}

void ds_engine_init(ds_engine *e, float fs, int fresh) {
    e->fs = fs;
    for (int i = 0; i < DS_P_COUNT; ++i) e->p[i] = DS_PARAMS[i].def;
    e->mix_k = ds_tau_coef(0.03f, fs);
    e->ctl_k = ds_tau_coef(0.08f, fs / DS_NA);
    e->pd_k = ds_tau_coef(0.25f, fs);
    e->dir_lp_k = ds_lp_coef(3500.0f, fs);
    e->wdc_r = 1.0f - 2.0f * DS_PI * 12.0f / fs;
    e->lim_rel = expf(-1.0f / (0.25f * fs));
    e->nan_resets = 0;
    e->ir_builds = 0;
    e->reg_count = ds_conv_regions(&e->conv, e->reg_ptr, e->reg_bytes, DS_REGIONS - 2);
    e->reg_ptr[e->reg_count] = e->fdn.line;
    e->reg_bytes[e->reg_count++] = sizeof e->fdn.line;
    e->reg_ptr[e->reg_count] = e->pd;
    e->reg_bytes[e->reg_count++] = sizeof e->pd;
    e->reg_idx = 0;
    e->reg_off = 0;
    small_state(e);
    e->mem_ready = fresh;
    e->job = fresh ? DS_JOB_IDLE : DS_JOB_CLEAR;
    ds_engine_snap(e);
}

void ds_engine_set(ds_engine *e, int idx, float v) {
    if (idx < 0 || idx >= DS_P_COUNT || !isfinite(v)) return;
    const ds_param_info *p = &DS_PARAMS[idx];
    v = ds_clampf(v, p->min, p->max);
    if (p->integer) v = floorf(v + 0.5f);
    e->p[idx] = v;
    if (idx == DS_P_MIX) {
        e->g_dry_t = target_dry(v);
        e->g_wet_t = target_wet(v);
    }
    if (idx == DS_P_SIZE) ds_fdn_set_scale(&e->fdn, powf(v, 0.75f), 0);
}

static float predelay_target(const ds_engine *e, float depth) {
    const float ms = (3.0f + 177.0f * powf(depth, 1.5f)) * (0.7f + 0.3f * e->p[DS_P_SIZE]);
    float d = floorf(ms * 0.001f * e->fs + 0.5f) - (float)DS_NA;   /* the FIFO already delays 128 */
    return d < 0.0f ? 0.0f : d;
}

static float dir_target(const ds_engine *e) {
    return ds_clampf(e->p[DS_P_MIX] * 2.5f, 0.0f, 1.0f) * e->p[DS_P_DEPTH];
}

void ds_engine_snap(ds_engine *e) {
    e->g_dry_t = e->g_dry = target_dry(e->p[DS_P_MIX]);
    e->g_wet_t = e->g_wet = target_wet(e->p[DS_P_MIX]);
    e->dir_w = e->dir_w_t = dir_target(e);
    e->depth_s = e->p[DS_P_DEPTH];
    e->diff_s = e->p[DS_P_DIFFUSION];
    e->dark_s = e->p[DS_P_DARKNESS];
    e->frz_s = e->p[DS_P_FREEZE];
    e->in_gain = 1.0f - e->p[DS_P_FREEZE];
    e->fdn_gain = e->in_gain;
    e->pd_d = predelay_target(e, e->depth_s);
    ds_fdn_set_scale(&e->fdn, powf(e->p[DS_P_SIZE], 0.75f), 1);
    e->g_er = -1.0f;   /* recomputed on the next block, without a ramp */
}



/* ---------- background work, one slice per block ---------- */

static int ir_wanted_differs(const ds_engine *e) {
    if (e->built_space < 0) return 1;
    const int space = (int)e->p[DS_P_SPACE];
    const float size = e->p[DS_P_SIZE];
    const float dec = ds_irgen_decay_used(space, size, e->p[DS_P_DECAY]);
    return space != e->built_space || fabsf(size - e->built_size) > 0.015f ||
           fabsf(dec / e->built_decay - 1.0f) > 0.03f;
}

int ds_engine_settled(const ds_engine *e) {
    if (e->asleep) return e->job == DS_JOB_IDLE && !ir_wanted_differs(e) && e->g_dry == e->g_dry_t;
    const int smooth = e->g_dry == e->g_dry_t && e->g_wet == e->g_wet_t && e->dir_w == e->dir_w_t &&
                       e->depth_s == e->p[DS_P_DEPTH] && e->diff_s == e->p[DS_P_DIFFUSION] &&
                       e->dark_s == e->p[DS_P_DARKNESS] && e->frz_s == e->p[DS_P_FREEZE] &&
                       e->fdn_gain == 1.0f - e->p[DS_P_FREEZE] &&
                       e->fdn.scale == e->fdn.scale_target && e->pd_d == predelay_target(e, e->p[DS_P_DEPTH]);
    return smooth && e->mem_ready && e->job == DS_JOB_IDLE && !ir_wanted_differs(e) && !e->conv.pending &&
           !e->conv.fade_state && e->conv.cur == e->build_set;
}

static void job_step(ds_engine *e) {
    switch (e->job) {
    case DS_JOB_CLEAR: {
        unsigned budget = e->wake_clear ? DS_WAKE_CHUNK : DS_CLEAR_CHUNK;
        while (budget && e->reg_idx < e->reg_count) {
            unsigned left = e->reg_bytes[e->reg_idx] - e->reg_off;
            unsigned n = left < budget ? left : budget;
            memset((char *)e->reg_ptr[e->reg_idx] + e->reg_off, 0, n);
            e->reg_off += n;
            budget -= n;
            if (e->reg_off >= e->reg_bytes[e->reg_idx]) {
                ++e->reg_idx;
                e->reg_off = 0;
            }
        }
        if (e->reg_idx >= e->reg_count) {
            e->reg_idx = 0;
            if (e->wake_clear) {
                /* keep the IR and every smoothed control; restart the history */
                history_state(e);
                ds_conv_reset_history(&e->conv);
                ds_fdn_set_scale(&e->fdn, powf(e->p[DS_P_SIZE], 0.75f), 1);
                e->g_er = -1.0f;
                e->wake_clear = 0;
            } else {
                small_state(e);
                ds_engine_snap(e);
            }
            e->mem_ready = 1;
            e->job = DS_JOB_IDLE;
        }
        break;
    }
    case DS_JOB_IDLE: {
        if (!ir_wanted_differs(e)) break;
        const int set = ds_conv_idle_set(&e->conv);
        if (set < 0) break;
        const int space = (int)e->p[DS_P_SPACE];
        const float size = e->p[DS_P_SIZE];
        e->want_space = space;
        e->built_size = size;
        e->built_decay = ds_irgen_decay_used(space, size, e->p[DS_P_DECAY]);
        ds_irgen_start(&e->gen, space, size, e->built_decay, e->fs, DS_IR_MAX);
        e->build_set = set;
        e->built_space = space;   /* "being built": a change from here restarts */
        e->job = DS_JOB_GEN;
        break;
    }
    case DS_JOB_GEN:
        if (ds_irgen_run(&e->gen, e->irbuf[0], e->irbuf[1], DS_GEN_CHUNK) == 0) e->job = DS_JOB_HEAD;
        break;
    case DS_JOB_HEAD:
        for (int ch = 0; ch < 2; ++ch) ds_conv_build_head(&e->conv, e->build_set, ch, e->irbuf[ch], e->gen.len);
        e->build_k = 0;
        e->build_kb = ds_conv_body_parts(e->gen.len);
        e->job = DS_JOB_BODY;
        break;
    case DS_JOB_BODY:
        if (e->build_k < e->build_kb) {
            for (int ch = 0; ch < 2; ++ch)
                ds_conv_build_body(&e->conv, e->build_set, ch, e->build_k, e->irbuf[ch], e->gen.len);
            ++e->build_k;
        }
        if (e->build_k >= e->build_kb) {
            ds_conv_publish(&e->conv, e->build_set, e->gen.len, ds_irgen_gain(&e->gen));
            ++e->ir_builds;
            e->job = DS_JOB_IDLE;
        }
        break;
    }
    /* A setting that moved while building: drop this build, start over. */
    if (e->job >= DS_JOB_GEN && ir_wanted_differs(e)) e->job = DS_JOB_IDLE;
}

/* ---------- the wet path, one 128-sample block ---------- */

/* One-pole step that lands exactly on the target once within 1e-5. */
static float smooth_to(float x, float target, float k) {
    x += (target - x) * k;
    return fabsf(target - x) < 1e-5f ? target : x;
}

static void wet_block(ds_engine *e) {
    const float fs = e->fs;
    float *pre_l = e->blk[0], *pre_r = e->blk[1];
    float *er_l = e->blk[2], *er_r = e->blk[3];
    float *late_l = e->blk[4], *late_r = e->blk[5];

    /* block-rate controls */
    const float k = e->ctl_k;
    e->depth_s = smooth_to(e->depth_s, e->p[DS_P_DEPTH], k);
    e->diff_s = smooth_to(e->diff_s, e->p[DS_P_DIFFUSION], k);
    e->dark_s = smooth_to(e->dark_s, e->p[DS_P_DARKNESS], k);
    const float frz_target = e->p[DS_P_FREEZE];
    const float frz_step = (float)DS_NA / (0.5f * fs);
    if (e->frz_s < frz_target) e->frz_s = fminf(frz_target, e->frz_s + frz_step);
    else if (e->frz_s > frz_target) e->frz_s = fmaxf(frz_target, e->frz_s - frz_step);
    const float d = e->depth_s, diff = e->diff_s, dark = e->dark_s, frz = e->frz_s;

    const float decay = e->p[DS_P_DECAY];
    const float rt = expf(ds_lerpf(logf(decay), logf(DS_FREEZE_RT), frz));
    const float hf = ds_lerpf(0.9f, 0.08f, powf(dark, 0.8f));
    ds_fdn_set_decay(&e->fdn, rt, ds_lerpf(hf, fmaxf(hf, 0.5f), frz));
    ds_fdn_set_mod(&e->fdn, 0.25f + 0.75f * diff);

    /* Late level: partly normalised for DECAY (fully normalised, a 30 s tail
     * would be 15 dB quieter at its start than a 1 s one; not at all, 15 dB
     * louder). Frozen, it keeps the unfrozen value. */
    const float gbar = ds_fdn_mean_gain(&e->fdn, decay);
    const float gref = ds_fdn_mean_gain(&e->fdn, 3.0f);
    const float lnorm = powf((1.0f - gbar * gbar) / (1.0f - gref * gref), 0.3f);

    const float g_er_t = ds_lerpf(1.0f, 0.3f, d);
    const float g_late_t = ds_lerpf(0.5f, 1.3f, d) * lnorm * 0.9f;
    const float air_t = air_coef(22000.0f * powf(0.12f, d) * powf(0.35f, dark), fs);
    const float comp_t = 1.0f + 0.6f * d + 0.25f * dark;
    if (e->g_er < 0.0f) {
        e->g_er = g_er_t;
        e->g_late = g_late_t;
        e->air_k_s = air_t;
        e->wet_comp = comp_t;
    }
    const float g_er0 = e->g_er, g_late0 = e->g_late, comp0 = e->wet_comp;
    e->g_er += (g_er_t - e->g_er) * k;
    e->g_late += (g_late_t - e->g_late) * k;
    e->air_k_s += (air_t - e->air_k_s) * k;
    e->wet_comp += (comp_t - e->wet_comp) * k;

    /* freeze gate + predelay */
    const float pd_t = predelay_target(e, d);
    const float gate_t = 1.0f - e->p[DS_P_FREEZE];
    const float gate_step = 1.0f / (0.25f * fs);
    for (int n = 0; n < DS_NA; ++n) {
        if (e->in_gain < gate_t) e->in_gain = fminf(gate_t, e->in_gain + gate_step);
        else if (e->in_gain > gate_t) e->in_gain = fmaxf(gate_t, e->in_gain - gate_step);
        e->pd[0][e->pd_w] = e->in_fifo[0][n] * e->in_gain;
        e->pd[1][e->pd_w] = e->in_fifo[1][n] * e->in_gain;
        e->pd_d += (pd_t - e->pd_d) * e->pd_k;
        if (fabsf(pd_t - e->pd_d) < 1e-3f) e->pd_d = pd_t;
        const int ip = (int)e->pd_d;
        const float fr = e->pd_d - (float)ip;
        const int r0 = (e->pd_w - ip) & (DS_PD_SIZE - 1), r1 = (r0 - 1) & (DS_PD_SIZE - 1);
        pre_l[n] = e->pd[0][r0] + fr * (e->pd[0][r1] - e->pd[0][r0]);
        pre_r[n] = e->pd[1][r0] + fr * (e->pd[1][r1] - e->pd[1][r0]);
        e->pd_w = (e->pd_w + 1) & (DS_PD_SIZE - 1);
    }

    ds_conv_process(&e->conv, pre_l, pre_r, er_l, er_r);

    /* reflection density, then the FDN's own input diffusion */
    const float g_dens = 0.15f + 0.45f * ds_clampf(0.7f * diff + 0.4f * d, 0.0f, 1.0f);
    const float g_in = 0.72f * ds_clampf(diff + 0.25f * d, 0.0f, 1.0f);
    const float fdn_step = 1.0f / (0.12f * fs);
    for (int n = 0; n < DS_NA; ++n) {
        float l = er_l[n], r = er_r[n];
        for (int i = 0; i < DS_AP_ER; ++i) {
            l = ap_run(&e->ap_er[0][i], l, g_dens);
            r = ap_run(&e->ap_er[1][i], r, g_dens);
        }
        er_l[n] = l;
        er_r[n] = r;
        for (int i = 0; i < DS_AP_IN; ++i) {
            l = ap_run(&e->ap_in[0][i], l, g_in);
            r = ap_run(&e->ap_in[1][i], r, g_in);
        }
        /* Frozen, nothing new may reach the loop - not even the IR's own
         * tail still sounding from before: that would pile up in a loop
         * that no longer loses energy. */
        if (e->fdn_gain < gate_t) e->fdn_gain = fminf(gate_t, e->fdn_gain + fdn_step);
        else if (e->fdn_gain > gate_t) e->fdn_gain = fmaxf(gate_t, e->fdn_gain - fdn_step);
        late_l[n] = l * e->fdn_gain;
        late_r[n] = r * e->fdn_gain;
    }
    ds_fdn_process(&e->fdn, late_l, late_r, late_l, late_r, DS_NA);

    /* balance, air, DC, limiter */
    const float inv = 1.0f / (float)DS_NA;
    const float ak = e->air_k_s;
    float bad = 0.0f;
    for (int n = 0; n < DS_NA; ++n) {
        const float t = (float)(n + 1) * inv;
        const float ge = ds_lerpf(g_er0, e->g_er, t), gl = ds_lerpf(g_late0, e->g_late, t);
        const float gc = ds_lerpf(comp0, e->wet_comp, t);
        float y[2] = {er_l[n] * ge + late_l[n] * gl, er_r[n] * ge + late_r[n] * gl};
        for (int ch = 0; ch < 2; ++ch) {
            e->air1[ch] += (y[ch] - e->air1[ch]) * ak;
            e->air2[ch] += (e->air1[ch] - e->air2[ch]) * ak;
            const float x = e->air2[ch] * gc;
            const float h = x - e->wdc_x[ch] + e->wdc_r * e->wdc_y[ch];
            e->wdc_x[ch] = x;
            e->wdc_y[ch] = h;
            y[ch] = h;
        }
        const float peak = fmaxf(fabsf(y[0]), fabsf(y[1]));
        e->lim_env = peak > e->lim_env ? peak : e->lim_env * e->lim_rel;
        const float lg = e->lim_env > 0.8f ? 0.8f / e->lim_env : 1.0f;
        e->wet_fifo[0][n] = y[0] * lg;
        e->wet_fifo[1][n] = y[1] * lg;
        bad += y[0] - y[0] + y[1] - y[1];   /* NaN/Inf propagate, finite gives 0 */
    }
    if (bad != 0.0f || !isfinite(e->lim_env)) {
        /* Never happens by design; if it does, go quiet and rebuild cleanly. */
        ++e->nan_resets;
        memset(e->wet_fifo, 0, sizeof e->wet_fifo);
        e->mem_ready = 0;
        e->job = DS_JOB_CLEAR;
        e->wake_clear = 0;
        e->reg_idx = 0;
        e->reg_off = 0;
    }
}

/* MIX 0, fully faded, and nothing frozen to keep: the wet path is silent
 * anyway, so it is not computed. */
static int can_sleep(const ds_engine *e) {
    return e->g_wet == 0.0f && e->g_wet_t == 0.0f && e->p[DS_P_FREEZE] == 0.0f && e->frz_s == 0.0f;
}

static void internal_block(ds_engine *e) {
    if (e->asleep) {
        if (can_sleep(e)) {
            /* An IR built meanwhile is simply taken: nothing is audible. */
            if (e->conv.pending || e->conv.fade_state) ds_conv_reset_history(&e->conv);
            job_step(e);
            return;
        }
        /* MIX came back: start from an empty space. The history is cleared
         * a slice per block (wet silent meanwhile); a half-built IR is
         * dropped and rebuilt. */
        e->asleep = 0;
        if (e->job >= DS_JOB_GEN) e->built_space = -1;
        e->wake_clear = 1;
        e->job = DS_JOB_CLEAR;
        e->reg_idx = 0;
        e->reg_off = 0;
        e->mem_ready = 0;
    } else if (e->mem_ready && can_sleep(e)) {
        e->asleep = 1;
        memset(e->wet_fifo, 0, sizeof e->wet_fifo);
        job_step(e);
        return;
    }
    if (e->mem_ready) wet_block(e);
    else memset(e->wet_fifo, 0, sizeof e->wet_fifo);
    job_step(e);
}

void ds_engine_process_float(ds_engine *e, float *l, float *r, int frames) {
    ds_ftz ftz;
    ds_ftz_begin(&ftz);
    const float dir_t = dir_target(e);
    e->dir_w_t = dir_t;
    for (int n = 0; n < frames; ++n) {
        const int pos = e->fifo_pos;
        const float xl = l[n], xr = r[n];
        e->in_fifo[0][pos] = xl;
        e->in_fifo[1][pos] = xr;

        e->g_dry += (e->g_dry_t - e->g_dry) * e->mix_k;
        e->g_wet += (e->g_wet_t - e->g_wet) * e->mix_k;
        e->dir_w += (e->dir_w_t - e->dir_w) * e->mix_k;
        /* Snap at 1e-4 (-80 dB): near 1.0 a float one-pole this slow stalls
         * short of its target (the step falls under half an ulp), and MIX 0
         * must end exactly at 1.0 to be a bit-exact bypass. */
        if (fabsf(e->g_dry - e->g_dry_t) < 1e-4f) e->g_dry = e->g_dry_t;
        if (fabsf(e->g_wet - e->g_wet_t) < 1e-4f) e->g_wet = e->g_wet_t;
        if (fabsf(e->dir_w - e->dir_w_t) < 1e-4f) e->dir_w = e->dir_w_t;

        /* the direct sound, moved away: quieter and duller with distance */
        e->dir_lp[0] += (xl - e->dir_lp[0]) * e->dir_lp_k;
        e->dir_lp[1] += (xr - e->dir_lp[1]) * e->dir_lp_k;
        float dl = xl, dr = xr;
        if (e->dir_w > 0.0f) {
            const float w = e->dir_w;
            const float g = 1.0f - 0.75f * w;
            dl = (xl + 0.7f * w * (e->dir_lp[0] - xl)) * g;
            dr = (xr + 0.7f * w * (e->dir_lp[1] - xr)) * g;
        }
        float yl = e->g_dry * dl + e->g_wet * e->wet_fifo[0][pos];
        float yr = e->g_dry * dr + e->g_wet * e->wet_fifo[1][pos];
        /* gentle peak protection, only in the last 2 % below full scale */
        if (fabsf(yl) > 0.98f) yl = copysignf(0.98f + 0.02f * tanhf((fabsf(yl) - 0.98f) * 50.0f), yl);
        if (fabsf(yr) > 0.98f) yr = copysignf(0.98f + 0.02f * tanhf((fabsf(yr) - 0.98f) * 50.0f), yr);
        l[n] = yl;
        r[n] = yr;

        if (++e->fifo_pos == DS_NA) {
            e->fifo_pos = 0;
            internal_block(e);
        }
    }
    ds_ftz_end(&ftz);
}

void ds_engine_process(ds_engine *e, int16_t *audio, int frames) {
    float l[DS_NA], r[DS_NA];
    while (frames > 0) {
        const int n = frames < DS_NA ? frames : DS_NA;
        for (int i = 0; i < n; ++i) {
            l[i] = (float)audio[2 * i] * (1.0f / 32768.0f);
            r[i] = (float)audio[2 * i + 1] * (1.0f / 32768.0f);
        }
        ds_engine_process_float(e, l, r, n);
        for (int i = 0; i < n; ++i) {
            const float a = l[i] * 32768.0f, b = r[i] * 32768.0f;
            audio[2 * i] = (int16_t)lrintf(ds_clampf(a, -32768.0f, 32767.0f));
            audio[2 * i + 1] = (int16_t)lrintf(ds_clampf(b, -32768.0f, 32767.0f));
        }
        audio += 2 * n;
        frames -= n;
    }
}
