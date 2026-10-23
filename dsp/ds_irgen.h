/*
 * Deep Space Convolution - synthetic stereo impulse responses.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 * The six SPACE presets are SYNTHETIC: no recorded IR is shipped. Each one
 * is generated from a fixed seed and a handful of acoustic descriptors
 * (initial gap, discrete early reflections, frequency-dependent decay, bloom,
 * stereo decorrelation, flutter between parallel walls, metallic modes), so
 * the same preset is bit-identical on every run and every machine, and SIZE
 * rescales its time structure instead of resampling it.
 *
 * Generation is incremental: ds_irgen_run() produces the next n samples, so
 * the engine can build an IR a few thousand samples per audio block.
 */
#ifndef DS_IRGEN_H
#define DS_IRGEN_H

#include <stdint.h>

#define DS_IR_TAPS 48
#define DS_IR_MODES 6

typedef struct {
    int len, pos;
    float fs;
    uint32_t rng_shared, rng_own[2];
    float c_shared, c_own;
    float xo_k, xo_s[2];
    float env_lo, env_hi, dec_lo, dec_hi;
    int tail_start;
    float onset, onset_k;
    float am, am_target, am_k, am_depth;
    int am_period, am_count;
    uint32_t rng_am;
    float lp_k, lp1[2], lp2[2];
    float hp_k, hp_s[2];
    int nmodes;
    float mode_c[DS_IR_MODES], mode_s[DS_IR_MODES], mode_rc[DS_IR_MODES], mode_rs[DS_IR_MODES];
    float mode_amp[DS_IR_MODES], mode_dec;
    float mode_env;
    double flutter_next, flutter_period;
    float flutter_amp;
    uint32_t rng_flutter;
    int ntaps[2], tap_idx[2];
    int tap_pos[2][DS_IR_TAPS];
    float tap_amp[2][DS_IR_TAPS];
    int fade_start;
    double energy[2];
    float trim;
} ds_irgen;

/* Start a preset at a SIZE factor and a DECAY (seconds; the IR never rings
 * longer than the late reverb is asked to). Sets g->len. */
void ds_irgen_start(ds_irgen *g, int space, float size, float decay, float fs, int max_len);

/* Write the next n samples (or fewer at the end) at ir_l/ir_r + g->pos.
 * Returns the number written; 0 once complete. */
int ds_irgen_run(ds_irgen *g, float *ir_l, float *ir_r, int n);

/* The DECAY a preset's IR actually uses at this SIZE (its own tail is
 * shortened only when DECAY asks for less): decides whether DECAY needs a
 * rebuild. */
float ds_irgen_decay_used(int space, float size, float decay);

/* Normalisation for the finished IR: unit mean energy per channel, times
 * the preset's loudness trim. */
float ds_irgen_gain(const ds_irgen *g);

#endif
