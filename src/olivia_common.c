/*
 * fldigi-olivia-core: version, parameter checks and the helpers shared by receiver and
 * transmitter (RateConverter, FIFO) after fldigi 4.2.13.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * Derived from fldigi 4.2.13 src/include/jalocha/pj_mfsk.h and pj_fifo.h,
 * (c) 2004 Pawel Jalocha SP9VRC, (c) 2019 David Freese W1HKJ.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "olivia_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FOLIVIA_VERSION_STRING "0.1.0"

const char *folivia_version(void)
{
    return "fldigi-olivia-core " FOLIVIA_VERSION_STRING " contract=" FOLIVIA_CONTRACT
           " abi=1 tx=1 dsp=" FDSP_VERSION_STRING;
}

/* Exp2 and Log2, pj_mfsk.h:22-31 */
size_t mfsk_exp2(uint32_t x)
{
    return (size_t)((uint32_t)1 << x);
}

size_t mfsk_log2(uint32_t x)
{
    uint32_t y;
    for (y = 0; x > 1; x >>= 1)
        y += 1;
    return y;
}

/* ---------------------------------------------------------------------------------- */
/* Parameters                                                                         */
/* ---------------------------------------------------------------------------------- */

int olivia_params_from_config(const folivia_config *cfg, olivia_params *p)
{
    size_t t;
    double bw;

    if (!cfg || !p)
        return -1;
    memset(p, 0, sizeof *p);
    /* olivia.cxx:324, contestia.cxx:273: the modem runs at 8000 Hz */
    if (!(cfg->sample_rate_hz == FOLIVIA_SAMPLE_RATE))
        return -1;
    /* fldigi's tone menu 2 .. 256 (configuration.h OLIVIATONES, Tx->Tones = 2 * (1 << n),
     * olivia.cxx:327); MFSK_Receiver::Preset rounds to a power of two (pj_mfsk.h:2041-2054) */
    if (cfg->tones < 2 || cfg->tones > 256)
        return -1;
    t = (size_t)cfg->tones;
    if ((t & (t - 1)) != 0)
        return -1;
    /* bandwidth = 125 * (1 << bw), bw 0..4 (olivia.cxx:325) */
    bw = cfg->bandwidth_hz;
    if (!(bw == 125.0 || bw == 250.0 || bw == 500.0 || bw == 1000.0 || bw == 2000.0))
        return -1;
    if (!(cfg->center_freq_hz > 0.0 && cfg->center_freq_hz < FOLIVIA_SAMPLE_RATE / 2))
        return -1;
    if (!(cfg->sync_threshold >= 0.0 && cfg->sync_threshold <= 1e6))
        return -1;
    if (cfg->sync_margin < 0 || cfg->sync_margin > 64)
        return -1;
    if (cfg->sync_integ_len < 0 || cfg->sync_integ_len > 64)
        return -1;
    p->tones = t;
    p->bandwidth = (size_t)bw;
    p->center = cfg->center_freq_hz;
    p->contestia = cfg->contestia ? 1 : 0;
    p->eight_bit = cfg->olivia8bit ? 1 : 0;
    p->reverse = cfg->reverse ? 1 : 0;
    p->sync_threshold = cfg->sync_threshold;
    /* configuration.h OLIVIASMARGIN 8, OLIVIASINTEG 4 */
    p->sync_margin = cfg->sync_margin ? (size_t)cfg->sync_margin : 8;
    p->sync_integ_len = cfg->sync_integ_len ? (size_t)cfg->sync_integ_len : 4;
    p->start_tones = cfg->start_tones ? 1 : 0;
    p->seed = cfg->seed;
    return 0;
}

double olivia_fc_offset(size_t bandwidth, size_t tones)
{
    /* olivia.cxx:78 (double) and :256, :336, :365 (assigned to int by the caller) */
    return bandwidth * (1.0 - 0.5 / tones) / 2.0;
}

int olivia_unescape(int *escape, int eight_bit, int c)
{
    /* olivia::unescape, olivia.cxx:142-158 (contestia.cxx:125-141 is identical) */
    if (eight_bit == 0)
        return c;
    if (*escape) {
        *escape = 0;
        return c + 128;
    }
    if (c == 127) {
        *escape = 1;
        return -1;
    }
    return c;
}

/* ---------------------------------------------------------------------------------- */
/* RateConverter, pj_mfsk.h:1435-1614                                                 */
/* ---------------------------------------------------------------------------------- */

void mfsk_rateconv_free(mfsk_rateconv *c)
{
    free(c->filter_shape);
    free(c->input_tap);
    c->filter_shape = NULL;
    c->input_tap = NULL;
}

int mfsk_rateconv_init(mfsk_rateconv *c, double output_rate)
{
    size_t idx;

    memset(c, 0, sizeof *c);
    /* RateConverter::Default, pj_mfsk.h:1481-1486 */
    c->tap_len = 16;
    c->oversampling = 16;
    c->upper_freq = 3.0 / 8;
    c->output_rate = output_rate;
    /* RateConverter::Preset, pj_mfsk.h:1488-1519 */
    c->tap_len = mfsk_exp2((uint32_t)mfsk_log2((uint32_t)c->tap_len));
    c->filter_len = c->tap_len * c->oversampling;
    c->filter_shape = (double *)malloc(c->filter_len * sizeof(double));
    c->input_tap = (double *)malloc(c->tap_len * sizeof(double));
    if (!c->filter_shape || !c->input_tap) {
        mfsk_rateconv_free(c);
        return -1;
    }
    for (idx = 0; idx < c->filter_len; idx++) {
        double phase = (M_PI * (2 * (int)idx - (int)c->filter_len)) / c->filter_len;
        /* Blackman-Harris window, pj_mfsk.h:1503-1505 */
        double window = 0.35875 + 0.48829 * cos(phase) + 0.14128 * cos(2 * phase) +
                        0.01168 * cos(3 * phase);
        double filter = 1.0;
        if (phase != 0) {
            phase *= (c->upper_freq * c->tap_len);
            filter = sin(phase) / phase;
        }
        c->filter_shape[idx] = window * filter;
    }
    mfsk_rateconv_reset(c);
    return 0;
}

void mfsk_rateconv_reset(mfsk_rateconv *c)
{
    /* RateConverter::Reset, pj_mfsk.h:1521-1534 */
    size_t idx;
    c->input_wrap = c->tap_len - 1;
    for (idx = 0; idx < c->tap_len; idx++)
        c->input_tap[idx] = 0;
    c->input_tap_ptr = 0;
    c->output_time = 0;
    c->output_period = c->oversampling / c->output_rate;
    c->output_before = 0;
    c->output_after = 0;
    c->output_ptr = 0;
}

static double rateconv_convolute(const mfsk_rateconv *c, size_t shift)
{
    /* RateConverter::Convolute, pj_mfsk.h:1538-1548 */
    double sum = 0;
    size_t idx = c->input_tap_ptr;
    shift = (c->oversampling - 1) - shift;
    for (; shift < c->filter_len; shift += c->oversampling) {
        sum += c->input_tap[idx] * c->filter_shape[shift];
        idx += 1;
        idx &= c->input_wrap;
    }
    return sum;
}

static void rateconv_new_input(mfsk_rateconv *c, double input)
{
    /* RateConverter::NewInput, pj_mfsk.h:1550-1553 */
    c->input_tap[c->input_tap_ptr] = input;
    c->input_tap_ptr += 1;
    c->input_tap_ptr &= c->input_wrap;
}

size_t mfsk_rateconv_process(mfsk_rateconv *c, const double *input, size_t input_len,
                             double *output)
{
    /* RateConverter::Process, pj_mfsk.h:1557-1597 */
    size_t output_len = 0;
    for (;;) {
        if (c->output_ptr) {
            size_t idx = (size_t)floor(c->output_time) + 1;
            double weight;
            if (idx >= c->oversampling) {
                if (input_len == 0)
                    break;
                rateconv_new_input(c, *input);
                input++;
                input_len -= 1;
                idx -= c->oversampling;
                c->output_time -= (double)c->oversampling;
            }
            c->output_after = rateconv_convolute(c, idx);
            weight = idx - c->output_time;
            *output = weight * c->output_before + (1.0 - weight) * c->output_after;
            output++;
            output_len += 1;
            c->output_ptr = 0;
        } else {
            size_t idx = (size_t)floor(c->output_time + c->output_period);
            if (idx >= c->oversampling) {
                if (input_len == 0)
                    break;
                rateconv_new_input(c, *input);
                input++;
                input_len -= 1;
                idx -= c->oversampling;
                c->output_time -= (double)c->oversampling;
            }
            c->output_before = rateconv_convolute(c, idx);
            c->output_time += c->output_period;
            c->output_ptr = 1;
        }
    }
    return output_len;
}

/* ---------------------------------------------------------------------------------- */
/* FIFO, pj_fifo.h:13-104                                                             */
/* ---------------------------------------------------------------------------------- */

int mfsk_fifo_init(mfsk_fifo *f, size_t len)
{
    f->len = len;
    f->data = (uint8_t *)calloc(len, 1);
    f->read_ptr = f->write_ptr = 0;
    return f->data ? 0 : -1;
}

void mfsk_fifo_free(mfsk_fifo *f)
{
    free(f->data);
    f->data = NULL;
    f->len = 0;
}

void mfsk_fifo_reset(mfsk_fifo *f)
{
    f->read_ptr = f->write_ptr = 0;
}

static void fifo_incr(const mfsk_fifo *f, size_t *ptr)
{
    *ptr += 1;
    if (*ptr >= f->len)
        *ptr -= f->len;
}

int mfsk_fifo_write(mfsk_fifo *f, uint8_t c)
{
    /* FIFO::Write, pj_fifo.h:78-86: one slot always stays free */
    size_t ptr = f->write_ptr;
    fifo_incr(f, &ptr);
    if (ptr == f->read_ptr)
        return 0;
    f->data[f->write_ptr] = c;
    f->write_ptr = ptr;
    return 1;
}

int mfsk_fifo_read(mfsk_fifo *f, uint8_t *c)
{
    /* FIFO::Read, pj_fifo.h:88-94 */
    if (f->read_ptr == f->write_ptr)
        return 0;
    *c = f->data[f->read_ptr];
    fifo_incr(f, &f->read_ptr);
    return 1;
}

size_t mfsk_fifo_read_ready(const mfsk_fifo *f)
{
    /* FIFO::ReadReady, pj_fifo.h:70-75 */
    long ready = (long)f->write_ptr - (long)f->read_ptr;
    if (ready < 0)
        ready += (long)f->len;
    return (size_t)ready;
}

int mfsk_fifo_empty(const mfsk_fifo *f)
{
    return f->read_ptr == f->write_ptr;
}
