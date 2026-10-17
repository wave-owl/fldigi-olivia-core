/*
 * fldigi-olivia-core internals: C structures of fldigi 4.2.13's Jalocha MFSK classes.
 * Copyright (C) 2026 Gerolf Ziegenhain. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef OLIVIA_INTERNAL_H
#define OLIVIA_INTERNAL_H

#include "fldigi_olivia.h"
#include "fldigi_dsp.h"

#include <stddef.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* pj_mfsk.h:68, 717-718 */
#define MFSK_CARRIER_SEPAR 2
#define MFSK_SLICES_PER_SYMBOL 2
/* olivia.h:35-36, sound.h:40: start/stop tone length and block size */
#define OLIVIA_SCBLOCKSIZE 512
#define OLIVIA_TONE_DURATION (OLIVIA_SCBLOCKSIZE * 16)
#define OLIVIA_SR4 (OLIVIA_TONE_DURATION / 4)
/* pj_mfsk.h:1076-1077 */
#define OLIVIA_SCRAMBLING_CODE 0xE257E6D0291574ECULL
#define CONTESTIA_SCRAMBLING_CODE 0xEDB88320ULL

/* pj_mfsk.h:22-31 */
size_t mfsk_exp2(uint32_t x);
size_t mfsk_log2(uint32_t x);

/* ---------------------------------------------------------------- RateConverter
 * pj_mfsk.h:1435-1614 (Type = double) */
typedef struct mfsk_rateconv {
    size_t tap_len;
    size_t oversampling;
    double upper_freq;
    double output_rate;
    size_t filter_len;
    double *filter_shape;
    double *input_tap;
    size_t input_tap_ptr;
    size_t input_wrap;
    double output_time;
    double output_period;
    double output_before;
    double output_after;
    size_t output_ptr;
} mfsk_rateconv;

int mfsk_rateconv_init(mfsk_rateconv *c, double output_rate);
void mfsk_rateconv_free(mfsk_rateconv *c);
void mfsk_rateconv_reset(mfsk_rateconv *c);
size_t mfsk_rateconv_process(mfsk_rateconv *c, const double *input, size_t input_len,
                             double *output);

/* ---------------------------------------------------------------- FIFO (pj_fifo.h) */
typedef struct mfsk_fifo {
    size_t len;
    size_t read_ptr;
    size_t write_ptr;
    uint8_t *data;
} mfsk_fifo;

int mfsk_fifo_init(mfsk_fifo *f, size_t len);
void mfsk_fifo_free(mfsk_fifo *f);
void mfsk_fifo_reset(mfsk_fifo *f);
int mfsk_fifo_write(mfsk_fifo *f, uint8_t c);
int mfsk_fifo_read(mfsk_fifo *f, uint8_t *c);
size_t mfsk_fifo_read_ready(const mfsk_fifo *f);
int mfsk_fifo_empty(const mfsk_fifo *f);

/* ---------------------------------------------------------------- shared setup */

/* Validated primary parameters common to RX and TX. */
typedef struct olivia_params {
    size_t tones;
    size_t bandwidth;           /* Hz, integer as in fldigi */
    double center;              /* "frequency" / txbasefreq */
    int contestia;
    int eight_bit;
    int reverse;
    double sync_threshold;
    size_t sync_margin;
    size_t sync_integ_len;
    int start_tones;
    uint32_t seed;
} olivia_params;

int olivia_params_from_config(const folivia_config *cfg, olivia_params *p);
/* Bandwidth*(1-0.5/Tones)/2 as fldigi computes it (olivia.cxx:78, 256) */
double olivia_fc_offset(size_t bandwidth, size_t tones);
/* fldigi's character unescape (olivia.cxx:142-158), -1 = escape consumed */
int olivia_unescape(int *escape, int eight_bit, int c);

#endif
