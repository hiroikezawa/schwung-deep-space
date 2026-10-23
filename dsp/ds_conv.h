/*
 * Deep Space Convolution - non-uniform partitioned stereo convolver.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 * Two stages, both overlap-save:
 *
 *   HEAD  h[0 .. 4096)          32 partitions of 128, FFT 256, every block.
 *                               Zero added latency.
 *   BODY  h[4096 .. 4096+45*2048) 45 partitions of 2048, FFT 4096. One body
 *                               cycle is 16 blocks of 128: the forward FFTs,
 *                               the multiply-accumulate and the inverse FFTs
 *                               are spread over those 16 blocks, so no single
 *                               block pays for a 4096-point transform AND the
 *                               whole frequency-domain delay line. The body
 *                               starts 2*2048 samples into the IR, which is
 *                               exactly the headroom that spreading needs.
 *
 * Two filter SETS share one input delay line, so switching IR costs nothing
 * in history: a newly published set is computed beside the playing one for
 * one body cycle (warm-up) and then crossfaded in over DS_FADE_CYCLES cycles.
 *
 * Channel c's input is convolved with channel c's IR (a stereo IR, the two
 * channels decorrelated by the generator), so a mono source comes out wide
 * and a stereo source keeps its sides.
 *
 * Nothing here allocates, locks or touches a file.
 */
#ifndef DS_CONV_H
#define DS_CONV_H

#include "ds_fft.h"

#define DS_NA 128                       /* head block = engine block */
#define DS_KA 32                        /* head partitions */
#define DS_MB 2048                      /* body block */
#define DS_KB 45                        /* body partitions (max) */
#define DS_HEAD_LEN (DS_NA * DS_KA)     /* 4096 */
#define DS_IR_MAX (DS_HEAD_LEN + DS_KB * DS_MB)   /* 96256 = 2.0 s at 48 kHz */
#define DS_CYCLE (DS_MB / DS_NA)        /* 16 blocks per body cycle */
#define DS_FADE_CYCLES 6                /* 6 * 2048 samples = 0.28 s at 44.1 kHz */

typedef struct {
    float a_re[2][DS_KA][DS_NA], a_im[2][DS_KA][DS_NA];
    float b_re[2][DS_KB][DS_MB], b_im[2][DS_KB][DS_MB];
    int ka, kb;       /* partitions in use */
    float gain;       /* output normalisation */
} ds_irset;

typedef struct {
    ds_irset set[2];
    int cur;          /* the set that is playing */
    int pending;      /* the other set is built and waits for a cycle boundary */
    int fade_state;   /* 0 steady, 1 warming the new set, 2 crossfading */
    int fade_pos;     /* samples into the crossfade */
    int body_on[2];   /* set s's body is computed this cycle */
    int body_valid[2];/* set s's outb holds a real body output this cycle */

    /* head: input history, frequency-domain delay line */
    float ta[2][2 * DS_NA];
    float xa_re[2][DS_KA][DS_NA], xa_im[2][DS_KA][DS_NA];
    int xa_head;

    /* body */
    float chunk[2][DS_MB];
    float tb[2][2 * DS_MB];
    float xb_re[2][DS_KB][DS_MB], xb_im[2][DS_KB][DS_MB];
    int xb_head;
    float accb_re[2][2][DS_MB], accb_im[2][2][DS_MB];   /* [set][ch] */
    float bodyout[2][2][2][DS_MB];   /* [phase][set][ch]: one plays, one fills */
    int phase;
    int slot;

    float scratch[2 * DS_MB];
    float acca_re[DS_NA], acca_im[DS_NA];
    float headout[2][2][DS_NA];      /* [set][ch] */
} ds_conv;

/* Make an instance silent: no IR, cleared history. Touches only the small
 * fields; the big history arrays must be zero already (static memory, or
 * cleared by the caller - see ds_conv_regions). */
void ds_conv_init_small(ds_conv *c);

/* The big arrays that hold signal history (for incremental clearing). */
int ds_conv_regions(ds_conv *c, void **ptr, unsigned *bytes, int max);

/* One 128-sample block, in place safe (out may alias in). */
void ds_conv_process(ds_conv *c, const float *in_l, const float *in_r, float *out_l, float *out_r);

/* --- building a new IR into the idle set, a piece at a time --- */

/* The set a builder may write, or -1 while a switch is in progress. */
int ds_conv_idle_set(const ds_conv *c);

/* Transform the head (all 32 partitions) of one channel. ir has len samples. */
void ds_conv_build_head(ds_conv *c, int set, int ch, const float *ir, int len);

/* Transform body partition k of one channel. */
void ds_conv_build_body(ds_conv *c, int set, int ch, int k, const float *ir, int len);

/* Body partitions needed for an IR of len samples. */
int ds_conv_body_parts(int len);

/* Hand the built set over; it is faded in at the next cycle boundaries. */
void ds_conv_publish(ds_conv *c, int set, int len, float gain);

/* Testing: build and switch to an IR immediately, without the crossfade. */
void ds_conv_load_now(ds_conv *c, const float *ir_l, const float *ir_r, int len, float gain);

#endif
