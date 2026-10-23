/*
 * Deep Space Convolution - 16-line feedback delay network (late reverb).
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 *   - 16 delay lines, lengths spread 17..97 ms at SIZE 1, scaled by SIZE^0.75
 *     and glided (never jumped) when SIZE moves;
 *   - a 16x16 Hadamard matrix (orthogonal, applied as a fast Walsh-Hadamard
 *     transform): lossless mixing, maximal density;
 *   - per line, a one-pole absorption filter whose DC and Nyquist gains are
 *     set so that each line decays at exactly RT60 (low) and RT60 * hf_ratio
 *     (high) for its own length: the RT60 does not depend on SIZE;
 *   - slow, small, independent delay modulation (cubic interpolation) to
 *     break up metallic ringing;
 *   - a safety soft limit on the recirculating state far above any normal
 *     level, a DC blocker on the output.
 */
#ifndef DS_FDN_H
#define DS_FDN_H

#define DS_FDN_LINES 16
#define DS_FDN_SIZE 16384     /* per line: 283 ms at SIZE 4 and 48 kHz + modulation */
#define DS_FDN_MASK (DS_FDN_SIZE - 1)

typedef struct {
    float line[DS_FDN_LINES][DS_FDN_SIZE];
    int w;
    float fs;
    float base[DS_FDN_LINES];          /* samples at scale 1 */
    float scale, scale_target, scale_k;
    float lp[DS_FDN_LINES];            /* absorption filter state */
    float g[DS_FDN_LINES], b[DS_FDN_LINES];
    float mod_c[DS_FDN_LINES], mod_s[DS_FDN_LINES], mod_rc[DS_FDN_LINES], mod_rs[DS_FDN_LINES];
    float mod_depth, mod_depth_target;
    float dc_x[2], dc_y[2], dc_r;
    int clamps;                        /* safety-limit hits (diagnostic) */
} ds_fdn;

void ds_fdn_init(ds_fdn *f, float fs);        /* small state only; lines must be zero */
void ds_fdn_set_scale(ds_fdn *f, float scale, int snap);
/* rt60 seconds at DC; hf_ratio = RT60(Nyquist) / RT60(DC), 0 < hf <= 1.
 * mod 0..1. Called once per block (cheap enough per 128 samples). */
void ds_fdn_set_decay(ds_fdn *f, float rt60, float hf_ratio);
void ds_fdn_set_mod(ds_fdn *f, float amount);
/* Mean per-pass loop gain at DC (for output normalisation). */
float ds_fdn_mean_gain(const ds_fdn *f, float rt60);
void ds_fdn_process(ds_fdn *f, const float *in_l, const float *in_r, float *out_l, float *out_r, int n);

#endif
