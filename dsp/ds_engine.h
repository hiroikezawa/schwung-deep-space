/*
 * Deep Space Convolution - the whole effect.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 *   in ─┬──────────────────────────────── direct (distance-shaped by DEPTH) ──┐
 *       └ freeze gate → predelay → convolution (IR) → density diffusion ─┬ ER │
 *                                    └ input diffusion → FDN (late) ─────┤    │
 *                                     ER/late balance, air absorption,   │    │
 *                                     DC block, wet peak limiter ────────┴ MIX┴→ out
 *
 * The wet path runs in fixed 128-sample blocks behind a 128-sample FIFO
 * whatever block size the host uses (the FIFO delay is taken out of the
 * predelay, so the wet timing is still exact and the dry path has zero
 * latency). Output is therefore identical for any host block size.
 *
 * At MIX 0 (fully faded out, FREEZE off) the wet path is not computed at
 * all; raising MIX clears its history and starts from an empty space.
 *
 * Background work (clearing a reused instance's memory, generating and
 * transforming a new IR) runs a slice per block on the audio thread: no
 * thread, no allocation, no file.
 */
#ifndef DS_ENGINE_H
#define DS_ENGINE_H

#include <stdint.h>

#include "ds_conv.h"
#include "ds_fdn.h"
#include "ds_irgen.h"
#include "ds_params.h"

#define DS_PD_SIZE 32768
#define DS_AP_SIZE 1024
#define DS_AP_IN 4      /* FDN input diffusers per channel */
#define DS_AP_ER 2      /* early-reflection density diffusers per channel */
#define DS_REGIONS 16

typedef struct {
    float buf[DS_AP_SIZE];
    int w, len;
} ds_allpass;

enum { DS_JOB_CLEAR, DS_JOB_IDLE, DS_JOB_GEN, DS_JOB_HEAD, DS_JOB_BODY };

typedef struct {
    float p[DS_P_COUNT];
    float fs;

    /* host-block-independent framing */
    float in_fifo[2][DS_NA], wet_fifo[2][DS_NA];
    int fifo_pos;

    /* dry / mix (per sample) */
    float g_dry, g_wet, g_dry_t, g_wet_t, mix_k;
    float dir_w, dir_w_t, dir_lp[2], dir_lp_k;

    /* smoothed block-rate controls */
    float depth_s, diff_s, dark_s, frz_s;
    float ctl_k;
    float g_er, g_late, air_k_s, wet_comp;
    float in_gain;      /* freeze gate before the predelay */
    float fdn_gain;     /* freeze gate at the FDN input */

    /* predelay */
    float pd[2][DS_PD_SIZE];
    int pd_w;
    float pd_d, pd_k;

    ds_allpass ap_er[2][DS_AP_ER], ap_in[2][DS_AP_IN];
    float air1[2], air2[2];
    float wdc_x[2], wdc_y[2], wdc_r;
    float lim_env, lim_rel;

    ds_conv conv;
    ds_fdn fdn;

    /* background jobs */
    int job;
    int mem_ready;
    int asleep;         /* MIX 0: the wet path is not computed at all */
    int wake_clear;     /* the running CLEAR keeps the IR (waking up) */
    void *reg_ptr[DS_REGIONS];
    unsigned reg_bytes[DS_REGIONS];
    int reg_count, reg_idx;
    unsigned reg_off;
    int build_set, build_k, build_kb;
    int built_space, want_space;      /* -1 = nothing built */
    float built_size, built_decay;
    ds_irgen gen;
    float irbuf[2][DS_IR_MAX];

    float blk[6][DS_NA];
    int nan_resets;
    int ir_builds;
} ds_engine;

/* fresh: the memory is known to be zero (never used static storage). */
void ds_engine_init(ds_engine *e, float fs, int fresh);
void ds_engine_set(ds_engine *e, int idx, float value);
/* Jump every smoothed control to its target (state restore, tests). */
void ds_engine_snap(ds_engine *e);
void ds_engine_process(ds_engine *e, int16_t *audio, int frames);
void ds_engine_process_float(ds_engine *e, float *l, float *r, int frames);
/* 1 once memory is clear and the requested IR is playing (no build, no fade). */
int ds_engine_settled(const ds_engine *e);

#endif
