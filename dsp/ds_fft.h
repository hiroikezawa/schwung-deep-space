/*
 * Deep Space Convolution - real FFT for the partitioned convolver.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 * Two fixed sizes only: a 2*128-point real transform (the zero-latency head)
 * and a 2*2048-point one (the long body). Tables are built once, before any
 * instance exists; the transforms themselves touch no memory but the arrays
 * handed to them.
 *
 * Spectrum format ("packed"): re[0] = DC, im[0] = Nyquist (both real),
 * re[k], im[k] = bin k for 1 <= k < m. A 2m-point real signal therefore
 * fits in m + m floats.
 */
#ifndef DS_FFT_H
#define DS_FFT_H

typedef struct ds_fft ds_fft;

/* Build the tables (idempotent). Called from the plugin's init entry. */
void ds_fft_global_init(void);

/* m = 128 or 2048 (the complex size; the real size is 2m). */
const ds_fft *ds_fft_get(int m);

/* x: 2m real samples. scratch: 2m floats. Unscaled. */
void ds_rfft(const ds_fft *f, const float *x, float *re, float *im, float *scratch);

/* Inverse of ds_rfft WITHOUT the 1/m factor (the convolver folds it into the
 * filter spectra). x and scratch: 2m floats; x may alias scratch. */
void ds_irfft(const ds_fft *f, const float *re, const float *im, float *x, float *scratch);

#endif
