/*
 * Deep Space Convolution - non-uniform partitioned stereo convolver.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 * Timing of the body (M = 2048, one cycle = 16 blocks):
 *
 *   cycle c      the input chunk c is collected, 128 samples a block
 *   cycle c+1    slot 0/1   forward FFT of [chunk c-1, chunk c], left/right
 *                slot 2-13  multiply-accumulate against all body partitions
 *                slot 14/15 inverse FFT, left/right -> the next output buffer
 *   cycle c+2    that buffer is played
 *
 * Chunk c starts at time T - M (T = start of cycle c+1) and its partition-0
 * contribution must start at T - M + 4096 = T + M: the start of cycle c+2.
 * So the body is exactly in time, and the head covers everything before it.
 */
#include "ds_conv.h"
#include "ds_dsp.h"

#include <math.h>
#include <string.h>

void ds_conv_init_small(ds_conv *c) {
    for (int s = 0; s < 2; ++s) {
        c->set[s].ka = 0;
        c->set[s].kb = 0;
        c->set[s].gain = 0.0f;
    }
    c->cur = 0;
    c->pending = 0;
    c->fade_state = 0;
    c->fade_pos = 0;
    c->body_on[0] = 1;
    c->body_on[1] = 0;
    c->body_valid[0] = c->body_valid[1] = 0;
    c->xa_head = 0;
    c->xb_head = 0;
    c->phase = 0;
    c->slot = 0;
}

int ds_conv_regions(ds_conv *c, void **ptr, unsigned *bytes, int max) {
    void *p[] = {c->ta, c->xa_re, c->xa_im, c->chunk, c->tb, c->xb_re, c->xb_im,
                 c->accb_re, c->accb_im, c->bodyout, c->headout};
    unsigned b[] = {sizeof c->ta, sizeof c->xa_re, sizeof c->xa_im, sizeof c->chunk, sizeof c->tb,
                    sizeof c->xb_re, sizeof c->xb_im, sizeof c->accb_re, sizeof c->accb_im,
                    sizeof c->bodyout, sizeof c->headout};
    int n = (int)(sizeof p / sizeof p[0]);
    if (n > max) n = max;
    for (int i = 0; i < n; ++i) {
        ptr[i] = p[i];
        bytes[i] = b[i];
    }
    return n;
}

/* acc += x * h over a packed spectrum of n slots. */
static void mac(float *restrict ar, float *restrict ai, const float *restrict xr,
                const float *restrict xi, const float *restrict hr, const float *restrict hi, int n) {
    ar[0] += xr[0] * hr[0];
    ai[0] += xi[0] * hi[0];
    for (int k = 1; k < n; ++k) {
        const float a = xr[k], b = xi[k], p = hr[k], q = hi[k];
        ar[k] += a * p - b * q;
        ai[k] += a * q + b * p;
    }
}

int ds_conv_body_parts(int len) {
    if (len <= DS_HEAD_LEN) return 0;
    int k = (len - DS_HEAD_LEN + DS_MB - 1) / DS_MB;
    return k > DS_KB ? DS_KB : k;
}

static int head_parts(int len) {
    int k = (len + DS_NA - 1) / DS_NA;
    return k > DS_KA ? DS_KA : (k < 0 ? 0 : k);
}

int ds_conv_idle_set(const ds_conv *c) {
    if (c->pending || c->fade_state) return -1;
    return 1 - c->cur;
}

/* One filter partition: h[off .. off+n) zero-padded to 2n, transformed and
 * scaled by 1/n (the inverse transform is unscaled). */
static void build_part(ds_conv *c, int n, const float *ir, int len, int off, float *re, float *im) {
    float *t = c->scratch;
    const float scale = 1.0f / (float)n;
    for (int i = 0; i < n; ++i) {
        int j = off + i;
        t[i] = j < len ? ir[j] * scale : 0.0f;
    }
    memset(t + n, 0, sizeof(float) * n);
    ds_rfft(ds_fft_get(n), t, re, im, t);
}

void ds_conv_build_head(ds_conv *c, int set, int ch, const float *ir, int len) {
    ds_irset *s = &c->set[set];
    int ka = head_parts(len);
    for (int k = 0; k < ka; ++k)
        build_part(c, DS_NA, ir, len, k * DS_NA, s->a_re[ch][k], s->a_im[ch][k]);
}

void ds_conv_build_body(ds_conv *c, int set, int ch, int k, const float *ir, int len) {
    ds_irset *s = &c->set[set];
    build_part(c, DS_MB, ir, len, DS_HEAD_LEN + k * DS_MB, s->b_re[ch][k], s->b_im[ch][k]);
}

void ds_conv_publish(ds_conv *c, int set, int len, float gain) {
    ds_irset *s = &c->set[set];
    s->ka = head_parts(len);
    s->kb = ds_conv_body_parts(len);
    s->gain = gain;
    __atomic_store_n(&c->pending, 1, __ATOMIC_RELEASE);
}

void ds_conv_load_now(ds_conv *c, const float *ir_l, const float *ir_r, int len, float gain) {
    int set = 1 - c->cur;
    const float *ir[2] = {ir_l, ir_r};
    for (int ch = 0; ch < 2; ++ch) {
        ds_conv_build_head(c, set, ch, ir[ch], len);
        for (int k = 0; k < ds_conv_body_parts(len); ++k) ds_conv_build_body(c, set, ch, k, ir[ch], len);
    }
    c->set[set].ka = head_parts(len);
    c->set[set].kb = ds_conv_body_parts(len);
    c->set[set].gain = gain;
    c->cur = set;
    c->body_on[set] = 1;
    c->body_on[1 - set] = 0;
    c->pending = 0;
    c->fade_state = 0;
}

static void cycle_boundary(ds_conv *c) {
    /* the buffers filled last cycle become the ones played now */
    c->phase ^= 1;
    for (int s = 0; s < 2; ++s) c->body_valid[s] = c->body_on[s];
    for (int ch = 0; ch < 2; ++ch) {
        memcpy(c->tb[ch], c->tb[ch] + DS_MB, sizeof(float) * DS_MB);
        memcpy(c->tb[ch] + DS_MB, c->chunk[ch], sizeof(float) * DS_MB);
    }
    c->xb_head = (c->xb_head + 1) % DS_KB;

    const int nxt = 1 - c->cur;
    if (c->fade_state == 2) {
        if (c->fade_pos >= DS_FADE_CYCLES * DS_MB) {
            c->cur = nxt;
            c->fade_state = 0;
            c->fade_pos = 0;
            c->body_valid[1 - c->cur] = 0;
        }
    } else if (c->fade_state == 1) {
        c->fade_state = 2;   /* the new set's body was computed last cycle */
        c->fade_pos = 0;
    } else if (__atomic_load_n(&c->pending, __ATOMIC_ACQUIRE)) {
        c->pending = 0;
        c->fade_state = 1;
    }
    c->body_on[c->cur] = 1;
    c->body_on[1 - c->cur] = c->fade_state != 0;
}

static void body_work(ds_conv *c, int slot) {
    if (slot < 2) {
        const int ch = slot;
        ds_rfft(ds_fft_get(DS_MB), c->tb[ch], c->xb_re[ch][c->xb_head], c->xb_im[ch][c->xb_head], c->scratch);
        return;
    }
    if (slot < 14) {
        const int j = slot - 2;
        for (int s = 0; s < 2; ++s) {
            if (!c->body_on[s]) continue;
            const ds_irset *f = &c->set[s];
            const int lo = f->kb * j / 12, hi = f->kb * (j + 1) / 12;
            for (int ch = 0; ch < 2; ++ch) {
                float *ar = c->accb_re[s][ch], *ai = c->accb_im[s][ch];
                if (j == 0) {
                    memset(ar, 0, sizeof(float) * DS_MB);
                    memset(ai, 0, sizeof(float) * DS_MB);
                }
                for (int k = lo; k < hi; ++k) {
                    const int x = (c->xb_head - k + DS_KB) % DS_KB;
                    mac(ar, ai, c->xb_re[ch][x], c->xb_im[ch][x], f->b_re[ch][k], f->b_im[ch][k], DS_MB);
                }
            }
        }
        return;
    }
    const int ch = slot - 14;
    for (int s = 0; s < 2; ++s) {
        if (!c->body_on[s]) continue;
        float *dst = c->bodyout[c->phase ^ 1][s][ch];
        if (c->set[s].kb == 0) {
            memset(dst, 0, sizeof(float) * DS_MB);
            continue;
        }
        ds_irfft(ds_fft_get(DS_MB), c->accb_re[s][ch], c->accb_im[s][ch], c->scratch, c->scratch);
        memcpy(dst, c->scratch + DS_MB, sizeof(float) * DS_MB);
    }
}

void ds_conv_process(ds_conv *c, const float *in_l, const float *in_r, float *out_l, float *out_r) {
    const int slot = c->slot;
    if (slot == 0) cycle_boundary(c);

    const float *in[2] = {in_l, in_r};
    for (int ch = 0; ch < 2; ++ch) memcpy(c->chunk[ch] + slot * DS_NA, in[ch], sizeof(float) * DS_NA);

    /* head */
    c->xa_head = (c->xa_head + 1) % DS_KA;
    for (int ch = 0; ch < 2; ++ch) {
        float *t = c->ta[ch];
        memcpy(t, t + DS_NA, sizeof(float) * DS_NA);
        memcpy(t + DS_NA, in[ch], sizeof(float) * DS_NA);
        ds_rfft(ds_fft_get(DS_NA), t, c->xa_re[ch][c->xa_head], c->xa_im[ch][c->xa_head], c->scratch);
        for (int s = 0; s < 2; ++s) {
            if (!c->body_on[s]) continue;
            const ds_irset *f = &c->set[s];
            if (f->ka == 0) {
                memset(c->headout[s][ch], 0, sizeof(float) * DS_NA);
                continue;
            }
            memset(c->acca_re, 0, sizeof c->acca_re);
            memset(c->acca_im, 0, sizeof c->acca_im);
            for (int k = 0; k < f->ka; ++k) {
                const int x = (c->xa_head - k + DS_KA) % DS_KA;
                mac(c->acca_re, c->acca_im, c->xa_re[ch][x], c->xa_im[ch][x], f->a_re[ch][k], f->a_im[ch][k], DS_NA);
            }
            ds_irfft(ds_fft_get(DS_NA), c->acca_re, c->acca_im, c->scratch, c->scratch);
            memcpy(c->headout[s][ch], c->scratch + DS_NA, sizeof(float) * DS_NA);
        }
    }

    body_work(c, slot);

    /* mix: steady = the current set; crossfade = equal power old -> new */
    const int cur = c->cur, nxt = 1 - cur;
    float *out[2] = {out_l, out_r};
    const int off = slot * DS_NA;
    for (int ch = 0; ch < 2; ++ch) {
        const float *hc = c->headout[cur][ch];
        const float *bc = c->bodyout[c->phase][cur][ch] + off;
        const float gc = c->set[cur].gain;
        const int bvc = c->body_valid[cur];
        if (c->fade_state != 2) {
            for (int n = 0; n < DS_NA; ++n) out[ch][n] = gc * (hc[n] + (bvc ? bc[n] : 0.0f));
        } else {
            const float *hn = c->headout[nxt][ch];
            const float *bn = c->bodyout[c->phase][nxt][ch] + off;
            const float gn = c->set[nxt].gain;
            const float span = (float)(DS_FADE_CYCLES * DS_MB);
            for (int n = 0; n < DS_NA; ++n) {
                const float t = (float)(c->fade_pos + n) / span;
                const float wo = cosf(0.5f * DS_PI * t), wn = sinf(0.5f * DS_PI * t);
                out[ch][n] = wo * gc * (hc[n] + (bvc ? bc[n] : 0.0f)) + wn * gn * (hn[n] + bn[n]);
            }
        }
    }
    if (c->fade_state == 2) c->fade_pos += DS_NA;
    c->slot = (slot + 1) % DS_CYCLE;
}
