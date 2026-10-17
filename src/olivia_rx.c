/*
 * fldigi-olivia-core receiver: MFSK_InputProcessor, MFSK_Demodulator, MFSK_SoftDecoder
 * and MFSK_Receiver of fldigi 4.2.13 (src/include/jalocha/pj_mfsk.h), driven like
 * olivia::rx_process (src/olivia/olivia.cxx:232-313) and contestia::rx_process
 * (src/contestia/contestia.cxx:205-264).
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * Derived from fldigi 4.2.13, (c) 2004 Pawel Jalocha SP9VRC, (c) 2006-2019 Dave Freese
 * W1HKJ and the fldigi developers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The C++ templates are instantiated with Type = double as in fldigi
 * (MFSK_Receiver<double>, olivia.cxx:518); the soft decoder keeps fldigi's float
 * Signal/NoiseEnergy accumulators and the float FirstCarrierMultiplier.
 */
#include "olivia_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* pj_mfsk.h:36-37: the symbol shape in the frequency domain, {1.0, 1.0} */
static const double mfsk_symbol_freq_shape[] = {1.0, 1.0};
#define MFSK_SYMBOL_FREQ_SHAPE_LEN 2

/* Largest input batch per RateConverter call: MFSK_Receiver::Preset reserves
 * WindowLen + 2048 input samples (pj_mfsk.h:2131) and RateConverter::Process asks for
 * ceil(n * OutputRate + 2) more (pj_mfsk.h:1602), so batches up to 2046 samples never
 * reallocate (fldigi feeds 512). Longer calls are split; the result does not change. */
#define RX_MAX_BATCH 2046

/* ---------------------------------------------------------------------------------- */
/* MFSK_InputProcessor, pj_mfsk.h:292-598                                             */
/* ---------------------------------------------------------------------------------- */

typedef struct mfsk_inproc {
    size_t window_len;
    size_t wrap_mask;
    double *inp_tap;
    size_t inp_tap_ptr;
    double *out_tap;
    size_t out_tap_ptr;
    double *window_shape;
    size_t slice_separ;
    fdsp_r2fft fft;
    fdsp_cmpx *fft_buff;
    size_t spectra_len;
    fdsp_cmpx *spectra[2];
    double *output;
    double *energy;
    /* BoxFilter<double>, pj_mfsk.h:242-288; taps sized for the largest Len used */
    size_t box_len;
    double *box_tap;
    size_t box_ptr;
    double box_output;
} mfsk_inproc;

static void inproc_free(mfsk_inproc *ip)
{
    free(ip->inp_tap);
    free(ip->out_tap);
    free(ip->window_shape);
    free(ip->fft_buff);
    free(ip->spectra[0]);
    free(ip->spectra[1]);
    free(ip->output);
    free(ip->energy);
    free(ip->box_tap);
    fdsp_r2fft_free(&ip->fft);
    memset(ip, 0, sizeof *ip);
}

static void box_preset(mfsk_inproc *ip, size_t len)
{
    /* BoxFilter::Preset + Clear, pj_mfsk.h:264-277 (fldigi reallocates the taps on every
     * call; here they are allocated once with the largest length) */
    size_t idx;
    ip->box_len = len;
    for (idx = 0; idx < len; idx++)
        ip->box_tap[idx] = 0;
    ip->box_ptr = 0;
    ip->box_output = 0;
}

static void box_process(mfsk_inproc *ip, double input)
{
    /* BoxFilter::Process, pj_mfsk.h:279-286 */
    ip->box_output -= ip->box_tap[ip->box_ptr];
    ip->box_output += input;
    ip->box_tap[ip->box_ptr] = input;
    ip->box_ptr += 1;
    if (ip->box_ptr >= ip->box_len)
        ip->box_ptr -= ip->box_len;
}

static int inproc_init(mfsk_inproc *ip, size_t window_len)
{
    /* MFSK_InputProcessor::Preset, pj_mfsk.h:354-391 */
    size_t idx, w = window_len;
    double shape_scale = 2.0 / w;

    memset(ip, 0, sizeof *ip);
    ip->window_len = w;
    ip->wrap_mask = w - 1;
    ip->inp_tap = (double *)calloc(w, sizeof(double));
    ip->out_tap = (double *)calloc(w, sizeof(double));
    ip->fft_buff = (fdsp_cmpx *)calloc(w, sizeof(fdsp_cmpx));
    ip->window_shape = (double *)calloc(w, sizeof(double));
    ip->spectra_len = w / 2;
    ip->spectra[0] = (fdsp_cmpx *)calloc(w / 2, sizeof(fdsp_cmpx));
    ip->spectra[1] = (fdsp_cmpx *)calloc(w / 2, sizeof(fdsp_cmpx));
    ip->output = (double *)calloc(w, sizeof(double));
    ip->energy = (double *)calloc(w / 2, sizeof(double));
    ip->box_tap = (double *)calloc(w / 16, sizeof(double));
    if (!ip->inp_tap || !ip->out_tap || !ip->fft_buff || !ip->window_shape ||
        !ip->spectra[0] || !ip->spectra[1] || !ip->output || !ip->energy || !ip->box_tap ||
        fdsp_r2fft_init(&ip->fft, w) < 0) {
        inproc_free(ip);
        return -1;
    }
    ip->slice_separ = w / 2;
    for (idx = 0; idx < w; idx++)
        ip->window_shape[idx] = shape_scale * sqrt(1.0 - ip->fft.twiddle[idx].re);
    box_preset(ip, w / 16);
    return 0;
}

static void inproc_reset(mfsk_inproc *ip)
{
    /* MFSK_InputProcessor::Reset, pj_mfsk.h:393-398 */
    memset(ip->inp_tap, 0, ip->window_len * sizeof(double));
    ip->inp_tap_ptr = 0;
    memset(ip->out_tap, 0, ip->window_len * sizeof(double));
    ip->out_tap_ptr = 0;
}

static void inproc_limit_spectra_peaks(mfsk_inproc *ip, fdsp_cmpx *spectra, size_t len,
                                       double threshold)
{
    /* MFSK_InputProcessor::LimitSpectraPeaks, pj_mfsk.h:400-421 */
    size_t max_freq = 3 * (ip->spectra_len / 4);
    size_t freq, idx;
    box_preset(ip, len);
    for (freq = 0; freq < len; freq++)
        box_process(ip, ip->energy[freq]);
    for (idx = len / 2; freq < max_freq; freq++, idx++) {
        double signal, limit;
        box_process(ip, ip->energy[freq]);
        signal = ip->energy[idx];
        limit = (ip->box_output / len) * threshold;
        if (signal > limit) {
            double corr = sqrt(limit / signal);
            spectra[idx].re *= corr;
            spectra[idx].im *= corr;
            ip->energy[idx] = limit;
        }
    }
}

static void inproc_limit_output_peaks(mfsk_inproc *ip, double threshold)
{
    /* MFSK_InputProcessor::LimitOutputPeaks, pj_mfsk.h:423-440 */
    size_t idx;
    double rms = 0, limit;
    for (idx = 0; idx < ip->window_len; idx++) {
        double signal = ip->output[idx];
        rms += signal * signal;
    }
    rms = sqrt(rms / ip->window_len);
    limit = rms * threshold;
    for (idx = 0; idx < ip->window_len; idx++) {
        double signal = ip->output[idx];
        if (signal > limit)
            ip->output[idx] = limit;
        else if (signal < (-limit))
            ip->output[idx] = (-limit);
    }
}

static void inproc_average_energy(mfsk_inproc *ip, size_t len)
{
    /* MFSK_InputProcessor::AverageEnergy, pj_mfsk.h:442-464 */
    size_t max_freq = 3 * (ip->spectra_len / 4);
    double scale = 1.0 / len;
    size_t len2 = len / 2;
    size_t idx, freq;
    box_preset(ip, len);
    for (freq = 0; freq < len; freq++)
        box_process(ip, ip->energy[freq]);
    for (idx = 0; idx < len2; idx++)
        ip->energy[idx] = ip->box_output * scale;
    for (; freq < max_freq; freq++, idx++) {
        box_process(ip, ip->energy[freq]);
        ip->energy[idx] = ip->box_output * scale;
    }
    for (; idx < ip->spectra_len; idx++)
        ip->energy[idx] = ip->box_output * scale;
}

static void inproc_process_spectra(mfsk_inproc *ip, fdsp_cmpx *spectra)
{
    /* MFSK_InputProcessor::ProcessSpectra, pj_mfsk.h:467-485 */
    size_t freq;
    for (freq = 0; freq < ip->spectra_len; freq++)
        ip->energy[freq] = spectra[freq].re * spectra[freq].re +
                           spectra[freq].im * spectra[freq].im;
    inproc_limit_spectra_peaks(ip, spectra, ip->window_len / 64, 4.0);
    inproc_limit_spectra_peaks(ip, spectra, ip->window_len / 64, 4.0);
    inproc_limit_spectra_peaks(ip, spectra, ip->window_len / 64, 4.0);
    inproc_average_energy(ip, ip->window_len / 96);
    inproc_average_energy(ip, ip->window_len / 64);
    for (freq = 0; freq < ip->spectra_len; freq++) {
        double corr = ip->energy[freq];
        if (corr <= 0)
            continue;
        corr = 1.0 / sqrt(corr);
        spectra[freq].re *= corr;
        spectra[freq].im *= corr;
    }
}

static void inproc_process(mfsk_inproc *ip, const double *input)
{
    /* MFSK_InputProcessor::Process, pj_mfsk.h:550-579 */
    size_t i;
    /* ProcessInpTap + ProcessInpWindow_Re, pj_mfsk.h:488-513 */
    for (i = 0; i < ip->slice_separ; i++) {
        ip->inp_tap[ip->inp_tap_ptr] = input[i];
        ip->inp_tap_ptr = (ip->inp_tap_ptr + 1) & ip->wrap_mask;
    }
    for (i = 0; i < ip->window_len; i++) {
        ip->fft_buff[i].re = ip->inp_tap[ip->inp_tap_ptr] * ip->window_shape[i];
        ip->inp_tap_ptr = (ip->inp_tap_ptr + 1) & ip->wrap_mask;
    }
    /* ProcessInpTap(Input + SliceSepar) + ProcessInpWindow_Im, pj_mfsk.h:515-522 */
    for (i = 0; i < ip->slice_separ; i++) {
        ip->inp_tap[ip->inp_tap_ptr] = input[ip->slice_separ + i];
        ip->inp_tap_ptr = (ip->inp_tap_ptr + 1) & ip->wrap_mask;
    }
    for (i = 0; i < ip->window_len; i++) {
        ip->fft_buff[i].im = ip->inp_tap[ip->inp_tap_ptr] * ip->window_shape[i];
        ip->inp_tap_ptr = (ip->inp_tap_ptr + 1) & ip->wrap_mask;
    }
    fdsp_r2fft_process(&ip->fft, ip->fft_buff);
    fdsp_r2fft_separ_two_reals(&ip->fft, ip->fft_buff, ip->spectra[0], ip->spectra[1]);
    inproc_process_spectra(ip, ip->spectra[0]);
    inproc_process_spectra(ip, ip->spectra[1]);
    fdsp_r2fft_join_two_reals(&ip->fft, ip->spectra[0], ip->spectra[1], ip->fft_buff);
    fdsp_r2fft_process(&ip->fft, ip->fft_buff);
    /* ProcessOutWindow_Re / ProcessOutTap / ProcessOutWindow_Im / ProcessOutTap,
     * pj_mfsk.h:524-548 */
    for (i = 0; i < ip->window_len; i++) {
        ip->out_tap[ip->out_tap_ptr] += ip->fft_buff[i].re * ip->window_shape[i];
        ip->out_tap_ptr = (ip->out_tap_ptr + 1) & ip->wrap_mask;
    }
    for (i = 0; i < ip->slice_separ; i++) {
        ip->output[i] = ip->out_tap[ip->out_tap_ptr];
        ip->out_tap[ip->out_tap_ptr] = 0;
        ip->out_tap_ptr = (ip->out_tap_ptr + 1) & ip->wrap_mask;
    }
    for (i = 0; i < ip->window_len; i++) {
        ip->out_tap[ip->out_tap_ptr] += ip->fft_buff[i].im * ip->window_shape[i];
        ip->out_tap_ptr = (ip->out_tap_ptr + 1) & ip->wrap_mask;
    }
    for (i = 0; i < ip->slice_separ; i++) {
        ip->output[ip->slice_separ + i] = ip->out_tap[ip->out_tap_ptr];
        ip->out_tap[ip->out_tap_ptr] = 0;
        ip->out_tap_ptr = (ip->out_tap_ptr + 1) & ip->wrap_mask;
    }
    inproc_limit_output_peaks(ip, 2.5);
    inproc_limit_output_peaks(ip, 2.5);
}

/* ---------------------------------------------------------------------------------- */
/* MFSK_Demodulator, pj_mfsk.h:704-1046 (EqualizerDepth = 0)                          */
/* ---------------------------------------------------------------------------------- */

typedef struct mfsk_demod {
    size_t symbol_len;
    size_t first_carrier;
    size_t bits_per_symbol;
    int use_gray_code;
    size_t decode_margin;
    int reverse;
    size_t carriers;
    size_t symbol_separ;
    size_t decode_width;
    size_t symbol_separ2;
    size_t wrap_mask;
    double *inp_tap;
    size_t inp_tap_ptr;
    double *symbol_shape;
    fdsp_r2fft fft;
    fdsp_cmpx *fft_buff;
    size_t spectra_len;
    fdsp_cmpx *spectra[2];
    double *energy[2];
} mfsk_demod;

static void demod_free(mfsk_demod *d)
{
    /* MFSK_Demodulator::Free, pj_mfsk.h:768-778 */
    free(d->inp_tap);
    free(d->symbol_shape);
    free(d->fft_buff);
    free(d->spectra[0]);
    free(d->spectra[1]);
    free(d->energy[0]);
    free(d->energy[1]);
    fdsp_r2fft_free(&d->fft);
    d->inp_tap = d->symbol_shape = NULL;
    d->fft_buff = d->spectra[0] = d->spectra[1] = NULL;
    d->energy[0] = d->energy[1] = NULL;
}

/* Parameters must be set (SymbolLen, FirstCarrier, BitsPerSymbol, DecodeMargin, Reverse,
 * UseGrayCode). MFSK_Demodulator::Preset, pj_mfsk.h:789-858. */
static int demod_init(mfsk_demod *d)
{
    size_t time, freq;
    double shape_scale;

    d->carriers = mfsk_exp2((uint32_t)d->bits_per_symbol);
    d->wrap_mask = d->symbol_len - 1;
    shape_scale = 1.0 / d->symbol_len;
    d->inp_tap = (double *)calloc(d->symbol_len, sizeof(double));
    d->inp_tap_ptr = 0;
    d->fft_buff = (fdsp_cmpx *)calloc(d->symbol_len, sizeof(fdsp_cmpx));
    d->symbol_shape = (double *)calloc(d->symbol_len, sizeof(double));
    d->spectra_len = d->symbol_len / 2;
    d->spectra[0] = (fdsp_cmpx *)calloc(d->spectra_len, sizeof(fdsp_cmpx));
    d->spectra[1] = (fdsp_cmpx *)calloc(d->spectra_len, sizeof(fdsp_cmpx));
    if (d->decode_margin > d->first_carrier)
        d->decode_margin = d->first_carrier;
    d->decode_width = (d->carriers * MFSK_CARRIER_SEPAR - 1) + 2 * d->decode_margin;
    d->energy[0] = (double *)calloc(d->decode_width, sizeof(double));
    d->energy[1] = (double *)calloc(d->decode_width, sizeof(double));
    if (!d->inp_tap || !d->fft_buff || !d->symbol_shape || !d->spectra[0] ||
        !d->spectra[1] || !d->energy[0] || !d->energy[1] ||
        fdsp_r2fft_init(&d->fft, d->symbol_len) < 0) {
        demod_free(d);
        return -1;
    }
    d->symbol_separ = d->symbol_len / 2;
    d->symbol_separ2 = d->symbol_separ / 2;
    /* pj_mfsk.h:809-827: shape 1 - cos(2 pi t / L), scaled by 1/L */
    for (time = 0; time < d->symbol_len; time++)
        d->symbol_shape[time] = mfsk_symbol_freq_shape[0];
    for (freq = 1; freq < MFSK_SYMBOL_FREQ_SHAPE_LEN; freq++) {
        double ampl = mfsk_symbol_freq_shape[freq];
        size_t phase = 0;
        if (freq & 1)
            ampl = (-ampl);
        for (time = 0; time < d->symbol_len; time++) {
            d->symbol_shape[time] += ampl * d->fft.twiddle[phase].re;
            phase += freq;
            if (phase >= d->symbol_len)
                phase -= d->symbol_len;
        }
    }
    for (time = 0; time < d->symbol_len; time++)
        d->symbol_shape[time] *= shape_scale;
    return 0;
}

static void demod_process(mfsk_demod *d, const double *input)
{
    /* MFSK_Demodulator::Process, pj_mfsk.h:860-938 */
    size_t inp_idx, time, idx, freq;
    for (inp_idx = 0; inp_idx < d->symbol_separ2; inp_idx++) {
        d->inp_tap[d->inp_tap_ptr] = input[inp_idx];
        d->inp_tap_ptr = (d->inp_tap_ptr + 1) & d->wrap_mask;
    }
    for (time = 0; time < d->symbol_len; time++) {
        d->fft_buff[time].re = d->inp_tap[d->inp_tap_ptr] * d->symbol_shape[time];
        d->inp_tap_ptr = (d->inp_tap_ptr + 1) & d->wrap_mask;
    }
    for (; inp_idx < d->symbol_separ; inp_idx++) {
        d->inp_tap[d->inp_tap_ptr] = input[inp_idx];
        d->inp_tap_ptr = (d->inp_tap_ptr + 1) & d->wrap_mask;
    }
    for (time = 0; time < d->symbol_len; time++) {
        d->fft_buff[time].im = d->inp_tap[d->inp_tap_ptr] * d->symbol_shape[time];
        d->inp_tap_ptr = (d->inp_tap_ptr + 1) & d->wrap_mask;
    }
    fdsp_r2fft_process(&d->fft, d->fft_buff);
    fdsp_r2fft_separ_two_reals(&d->fft, d->fft_buff, d->spectra[0], d->spectra[1]);
    /* pj_mfsk.h:924-937 (no equaliser) */
    if (d->reverse == 1) {
        freq = d->first_carrier;
        for (idx = 0; idx < d->decode_width; idx++, freq--) {
            d->energy[0][idx] = d->spectra[0][freq].re * d->spectra[0][freq].re +
                                d->spectra[0][freq].im * d->spectra[0][freq].im;
            d->energy[1][idx] = d->spectra[1][freq].re * d->spectra[1][freq].re +
                                d->spectra[1][freq].im * d->spectra[1][freq].im;
        }
    } else {
        freq = d->first_carrier - d->decode_margin;
        for (idx = 0; idx < d->decode_width; idx++, freq++) {
            d->energy[0][idx] = d->spectra[0][freq].re * d->spectra[0][freq].re +
                                d->spectra[0][freq].im * d->spectra[0][freq].im;
            d->energy[1][idx] = d->spectra[1][freq].re * d->spectra[1][freq].re +
                                d->spectra[1][freq].im * d->spectra[1][freq].im;
        }
    }
}

static void demod_soft_decode(const mfsk_demod *d, double *symbol, size_t slice,
                              int freq_offset)
{
    /* MFSK_Demodulator::SoftDecode, pj_mfsk.h:955-982 */
    size_t bit, idx, freq = 0;
    const double *energy_ptr = d->energy[slice] + (d->decode_margin + freq_offset);
    double total_energy = 0;
    for (bit = 0; bit < d->bits_per_symbol; bit++)
        symbol[bit] = 0;
    for (idx = 0; idx < d->carriers; idx++) {
        uint8_t symb_idx = (uint8_t)idx;
        double energy;
        uint8_t mask = 1;
        if (d->use_gray_code)
            symb_idx = fdsp_binary_code(symb_idx);
        energy = energy_ptr[freq];
        energy *= energy;
        total_energy += energy;
        for (bit = 0; bit < d->bits_per_symbol; bit++) {
            if (symb_idx & mask)
                symbol[bit] -= energy;
            else
                symbol[bit] += energy;
            mask = (uint8_t)(mask << 1);
        }
        freq += MFSK_CARRIER_SEPAR;
    }
    if (total_energy > 0) {
        for (bit = 0; bit < d->bits_per_symbol; bit++)
            symbol[bit] /= total_energy;
    }
}

/* ---------------------------------------------------------------------------------- */
/* MFSK_SoftDecoder<double,double>, pj_mfsk.h:1219-1430                               */
/* ---------------------------------------------------------------------------------- */

typedef struct mfsk_softdec {
    size_t bits_per_symbol;
    size_t symbols_per_block;
    float signal;
    float noise_energy;
    uint8_t output_block[8];
    int contestia;
    uint64_t scrambling_code;
    size_t input_buffer_len;
    double *input_buffer;   /* points into the receiver's pool */
    size_t input_ptr;
    double *fht_buffer;     /* points into the receiver's pool */
} mfsk_softdec;

static void softdec_reset(mfsk_softdec *s)
{
    /* MFSK_SoftDecoder::Reset, pj_mfsk.h:1280-1285 */
    size_t idx;
    for (idx = 0; idx < s->input_buffer_len; idx++)
        s->input_buffer[idx] = 0;
    s->input_ptr = 0;
}

static void softdec_input(mfsk_softdec *s, const double *symbol)
{
    /* MFSK_SoftDecoder::Input, pj_mfsk.h:1319-1326 */
    size_t freq_bit;
    for (freq_bit = 0; freq_bit < s->bits_per_symbol; freq_bit++) {
        s->input_buffer[s->input_ptr] = symbol[freq_bit];
        s->input_ptr += 1;
    }
    if (s->input_ptr >= s->input_buffer_len)
        s->input_ptr -= s->input_buffer_len;
}

static void softdec_decode_character(mfsk_softdec *s, size_t freq_bit)
{
    /* MFSK_SoftDecoder::DecodeCharacter, pj_mfsk.h:1328-1383 */
    size_t time_bit, peak_pos = 0;
    size_t ptr = s->input_ptr;
    size_t rotate = freq_bit;
    size_t code_wrap = s->symbols_per_block - 1;
    size_t n_shift = s->contestia ? 5 : 13;   /* Olivia 13, Contestia 5 */
    size_t code_bit = freq_bit * n_shift;
    double peak = 0, sqr_sum = 0;
    uint8_t ch;

    code_bit &= code_wrap;
    for (time_bit = 0; time_bit < s->symbols_per_block; time_bit++) {
        double bit = s->input_buffer[ptr + rotate];
        uint64_t code_mask = 1;
        code_mask <<= code_bit;
        if (s->scrambling_code & code_mask)
            bit = (-bit);
        s->fht_buffer[time_bit] = bit;
        code_bit += 1;
        code_bit &= code_wrap;
        rotate += 1;
        if (rotate >= s->bits_per_symbol)
            rotate -= s->bits_per_symbol;
        ptr += s->bits_per_symbol;
        if (ptr >= s->input_buffer_len)
            ptr -= s->input_buffer_len;
    }
    fdsp_fht(s->fht_buffer, s->symbols_per_block);
    for (time_bit = 0; time_bit < s->symbols_per_block; time_bit++) {
        double signal = s->fht_buffer[time_bit];
        sqr_sum += signal * signal;
        if (fabs(signal) > fabs(peak)) {
            peak = signal;
            peak_pos = time_bit;
        }
    }
    ch = (uint8_t)peak_pos;
    if (peak < 0)
        ch = (uint8_t)(ch + s->symbols_per_block);
    sqr_sum -= peak * peak;
    /* Contestia character map, pj_mfsk.h:1369-1378 */
    if (s->contestia && ch > 0) {
        if (ch == 59)
            ch = ' ';
        else if (ch == 60)
            ch = '\r';
        else if (ch == 61)
            ch = 8; /* backspace */
        else
            ch = (uint8_t)(ch + 32);
    }
    s->output_block[freq_bit] = ch;
    s->noise_energy += (float)sqr_sum / (s->symbols_per_block - 1);
    s->signal += fabs(peak);
}

static void softdec_process(mfsk_softdec *s)
{
    /* MFSK_SoftDecoder::Process, pj_mfsk.h:1385-1393 */
    size_t freq_bit;
    s->signal = 0;
    s->noise_energy = 0;
    for (freq_bit = 0; freq_bit < s->bits_per_symbol; freq_bit++)
        softdec_decode_character(s, freq_bit);
    s->signal /= s->bits_per_symbol;
    s->noise_energy /= s->bits_per_symbol;
}

static uint64_t softdec_output_packed(const mfsk_softdec *s)
{
    /* MFSK_SoftDecoder::Output(uint64_t &), pj_mfsk.h:1402-1411 */
    size_t freq_bit;
    uint64_t packed = 0;
    for (freq_bit = s->bits_per_symbol; freq_bit > 0;) {
        packed <<= 8;
        freq_bit--;
        packed |= s->output_block[freq_bit];
    }
    return packed;
}

/* ---------------------------------------------------------------------------------- */
/* MFSK_Receiver<double>, pj_mfsk.h:1929-2365                                         */
/* ---------------------------------------------------------------------------------- */

struct folivia_rx {
    olivia_params p;
    /* primary parameters after Preset */
    size_t tones;
    size_t bandwidth;
    size_t sync_margin;
    size_t sync_integ_len;
    double sync_threshold;
    double sample_rate;
    float first_carrier_multiplier;
    int reverse;
    size_t bits_per_symbol;
    size_t symbols_per_block;

    mfsk_rateconv converter;
    double *input_buffer;     /* Seq<double> InputBuffer */
    size_t input_len;
    size_t input_space;
    mfsk_inproc inproc;
    mfsk_demod demod;
    size_t freq_offsets;
    size_t block_phases;
    mfsk_softdec *decoder;    /* SlicesPerSymbol * FreqOffsets */
    double *decoder_pool;
    size_t block_phase;
    fdsp_lowpass3 *sync_noise_energy;  /* CircularBuffer: Len BlockPhases x Width FreqOffsets */
    fdsp_lowpass3 *sync_signal;
    double sync_filter_weight;
    double sync_best_signal;
    size_t sync_best_block_phase;
    size_t sync_best_freq_offset;
    double sync_snr;
    uint64_t *decode_pipe;    /* BlockPhases x (SyncIntegLen x FreqOffsets) */
    size_t *decode_pipe_ptr;  /* CircularBuffer::Ptr per block phase */
    mfsk_fifo output;
    int escape;               /* olivia::escape */
    size_t wrapped_reads;     /* times the pj_mfsk.h:2337 deviation path was taken */
};

static void rx_free_parts(folivia_rx *rx)
{
    mfsk_rateconv_free(&rx->converter);
    free(rx->input_buffer);
    inproc_free(&rx->inproc);
    demod_free(&rx->demod);
    free(rx->decoder);
    free(rx->decoder_pool);
    free(rx->sync_noise_energy);
    free(rx->sync_signal);
    free(rx->decode_pipe);
    free(rx->decode_pipe_ptr);
    mfsk_fifo_free(&rx->output);
}

void folivia_rx_destroy(folivia_rx *rx)
{
    if (!rx)
        return;
    rx_free_parts(rx);
    free(rx);
}

static void rx_clear_sync(folivia_rx *rx)
{
    /* CircularBuffer::Clear of DecodePipe, SyncSignal, SyncNoiseEnergy, pj_mfsk.h:643-647 */
    size_t idx, n = rx->block_phases * rx->freq_offsets;
    for (idx = 0; idx < n; idx++) {
        fdsp_lowpass3_set(&rx->sync_signal[idx], 0.0);
        fdsp_lowpass3_set(&rx->sync_noise_energy[idx], 0.0);
    }
    memset(rx->decode_pipe, 0,
           rx->block_phases * rx->sync_integ_len * rx->freq_offsets * sizeof(uint64_t));
    for (idx = 0; idx < rx->block_phases; idx++)
        rx->decode_pipe_ptr[idx] = 0;
}

/* Sync threshold after olivia::rx_process / contestia::rx_process (olivia.cxx:271-272,
 * contestia.cxx:232-233): squelch off gives 0 (Olivia) or 3 (Contestia). */
static double effective_sync_threshold(const olivia_params *p)
{
    double lo = p->contestia ? 3.0 : 0.0, v = p->sync_threshold;
    if (v == 0.0)
        return lo;
    if (v < lo)
        v = lo;
    if (v > 90.0)
        v = 90.0;
    return v;
}

folivia_rx *folivia_rx_create(const folivia_config *cfg)
{
    folivia_rx *rx;
    olivia_params p;
    size_t idx, n_dec, pool_per_dec;
    mfsk_demod *d;

    if (olivia_params_from_config(cfg, &p) < 0)
        return NULL;
    rx = (folivia_rx *)calloc(1, sizeof *rx);
    if (!rx)
        return NULL;
    rx->p = p;

    /* olivia::restart, olivia.cxx:355-372 and the retune in rx_process,
     * olivia.cxx:256-269 (fc_offset is an int there); contestia.cxx:301-318 */
    rx->tones = p.tones;
    rx->bandwidth = p.bandwidth;
    rx->sync_margin = p.sync_margin;
    rx->sync_integ_len = p.sync_integ_len;
    rx->sample_rate = FOLIVIA_SAMPLE_RATE;
    rx->reverse = p.reverse;
    if (p.contestia) {
        if (p.reverse)
            rx->first_carrier_multiplier = (float)((p.center + (double)(p.bandwidth / 2)) / 500);
        else
            rx->first_carrier_multiplier = (float)((p.center - (double)(p.bandwidth / 2)) / 500);
    } else {
        int fc_offset = (int)olivia_fc_offset(p.bandwidth, p.tones);
        if (p.reverse)
            rx->first_carrier_multiplier = (float)((p.center + fc_offset) / 500.0);
        else
            rx->first_carrier_multiplier = (float)((p.center - fc_offset) / 500.0);
    }

    if (!(rx->first_carrier_multiplier > 0))
        goto error;  /* the band would start below 0 Hz */
    /* MFSK_Receiver::Preset, pj_mfsk.h:2039-2142 */
    if (rx->sync_margin < 2)
        rx->sync_margin = 2;
    if (rx->sync_integ_len < 2)
        rx->sync_integ_len = 2;
    rx->bits_per_symbol = mfsk_log2((uint32_t)rx->tones);
    rx->tones = mfsk_exp2((uint32_t)rx->bits_per_symbol);
    if (mfsk_rateconv_init(&rx->converter, rx->sample_rate / FOLIVIA_SAMPLE_RATE) < 0)
        goto error;
    d = &rx->demod;
    d->bits_per_symbol = rx->bits_per_symbol;
    d->symbol_len = mfsk_exp2((uint32_t)(rx->bits_per_symbol + 7 -
                                         mfsk_log2((uint32_t)(rx->bandwidth / 125))));
    rx->bandwidth = mfsk_exp2((uint32_t)mfsk_log2((uint32_t)(rx->bandwidth / 125))) * 125;
    d->first_carrier = (size_t)((d->symbol_len / 16) * rx->first_carrier_multiplier) + 1;
    d->reverse = rx->reverse;
    d->decode_margin = rx->sync_margin;
    d->use_gray_code = 1;
    if (d->symbol_len < 4)
        goto error;
    if (demod_init(d) < 0)
        goto error;
    rx->sync_margin = d->decode_margin;
    /* Every FFT bin the demodulator reads must exist (fldigi does not check). */
    if (d->reverse == 1) {
        if (d->first_carrier >= d->spectra_len || d->first_carrier + 1 < d->decode_width)
            goto error;
    } else {
        if (d->first_carrier - d->decode_margin + d->decode_width > d->spectra_len)
            goto error;
    }
    if (inproc_init(&rx->inproc, 16 * d->symbol_len) < 0)
        goto error;

    /* RefDecoder, pj_mfsk.h:2072-2076, 1287-1311 */
    rx->symbols_per_block = mfsk_exp2((uint32_t)((p.contestia ? 6 : 7) - 1));
    rx->freq_offsets = 2 * rx->sync_margin + 1;
    rx->block_phases = MFSK_SLICES_PER_SYMBOL * rx->symbols_per_block;

    n_dec = MFSK_SLICES_PER_SYMBOL * rx->freq_offsets;
    pool_per_dec = rx->symbols_per_block * rx->bits_per_symbol + rx->symbols_per_block;
    rx->decoder = (mfsk_softdec *)calloc(n_dec, sizeof(mfsk_softdec));
    rx->decoder_pool = (double *)calloc(n_dec * pool_per_dec, sizeof(double));
    if (!rx->decoder || !rx->decoder_pool)
        goto error;
    for (idx = 0; idx < n_dec; idx++) {
        mfsk_softdec *s = &rx->decoder[idx];
        s->bits_per_symbol = rx->bits_per_symbol;
        s->symbols_per_block = rx->symbols_per_block;
        s->contestia = p.contestia;
        s->scrambling_code = p.contestia ? CONTESTIA_SCRAMBLING_CODE : OLIVIA_SCRAMBLING_CODE;
        s->input_buffer_len = rx->symbols_per_block * rx->bits_per_symbol;
        s->input_buffer = rx->decoder_pool + idx * pool_per_dec;
        s->fht_buffer = s->input_buffer + s->input_buffer_len;
        softdec_reset(s);
    }
    rx->decode_pipe = (uint64_t *)calloc(rx->block_phases * rx->sync_integ_len *
                                         rx->freq_offsets, sizeof(uint64_t));
    rx->decode_pipe_ptr = (size_t *)calloc(rx->block_phases, sizeof(size_t));
    rx->sync_signal = (fdsp_lowpass3 *)calloc(rx->block_phases * rx->freq_offsets,
                                              sizeof(fdsp_lowpass3));
    rx->sync_noise_energy = (fdsp_lowpass3 *)calloc(rx->block_phases * rx->freq_offsets,
                                                    sizeof(fdsp_lowpass3));
    if (!rx->decode_pipe || !rx->decode_pipe_ptr || !rx->sync_signal ||
        !rx->sync_noise_energy)
        goto error;
    rx->sync_filter_weight = 1.0 / rx->sync_integ_len;
    rx->input_space = rx->inproc.window_len + 2048;
    rx->input_buffer = (double *)calloc(rx->input_space, sizeof(double));
    if (!rx->input_buffer)
        goto error;
    if (mfsk_fifo_init(&rx->output, 1024) < 0)
        goto error;
    rx->sync_threshold = effective_sync_threshold(&p);
    folivia_rx_reset(rx);
    return rx;

error:
    folivia_rx_destroy(rx);
    return NULL;
}

void folivia_rx_reset(folivia_rx *rx)
{
    /* MFSK_Receiver::Reset, pj_mfsk.h:2144-2166, and olivia::rx_init (escape = 0) */
    size_t idx;
    if (!rx)
        return;
    mfsk_rateconv_reset(&rx->converter);
    rx->input_len = 0;
    inproc_reset(&rx->inproc);
    for (idx = 0; idx < MFSK_SLICES_PER_SYMBOL * rx->freq_offsets; idx++)
        softdec_reset(&rx->decoder[idx]);
    rx_clear_sync(rx);
    rx->block_phase = 0;
    rx->sync_best_signal = 0;
    rx->sync_best_block_phase = 0;
    rx->sync_best_freq_offset = 0;
    rx->sync_snr = 0;
    mfsk_fifo_reset(&rx->output);
    rx->escape = 0;
    rx->wrapped_reads = 0;
    /* Preset() state that Reset() does not touch in fldigi but a fresh Preset() has */
    memset(rx->demod.inp_tap, 0, rx->demod.symbol_len * sizeof(double));
    rx->demod.inp_tap_ptr = 0;
    memset(rx->inproc.output, 0, rx->inproc.window_len * sizeof(double));
}

static void rx_process_symbol(folivia_rx *rx, const double *input)
{
    /* MFSK_Receiver::ProcessSymbol, pj_mfsk.h:2275-2363 */
    mfsk_softdec *decoder_ptr = rx->decoder;
    size_t offset, slice, f = rx->freq_offsets;

    demod_process(&rx->demod, input);
    for (slice = 0; slice < MFSK_SLICES_PER_SYMBOL; slice++) {
        fdsp_lowpass3 *noise_energy_ptr = rx->sync_noise_energy + rx->block_phase * f;
        fdsp_lowpass3 *signal_ptr = rx->sync_signal + rx->block_phase * f;
        uint64_t *decode_block_ptr = rx->decode_pipe +
            (rx->block_phase * rx->sync_integ_len + rx->decode_pipe_ptr[rx->block_phase]) * f;
        size_t best_slice_offset = 0;
        double best_slice_signal = 0;
        double noise_energy, signal;
        double symbol[8];
        int dist;

        for (offset = 0; offset < f; offset++) {
            demod_soft_decode(&rx->demod, symbol, slice, (int)offset - (int)(f / 2));
            softdec_input(decoder_ptr, symbol);
            softdec_process(decoder_ptr);
            *decode_block_ptr = softdec_output_packed(decoder_ptr);

            noise_energy = decoder_ptr->noise_energy;
            fdsp_lowpass3_process(noise_energy_ptr, noise_energy, rx->sync_filter_weight, 0.1);

            signal = decoder_ptr->signal;
            fdsp_lowpass3_process(signal_ptr, signal, rx->sync_filter_weight, 0.1);
            signal = signal_ptr->output;

            if (signal > best_slice_signal) {
                best_slice_signal = signal;
                best_slice_offset = offset;
            }
            decoder_ptr++;
            decode_block_ptr++;
            noise_energy_ptr++;
            signal_ptr++;
        }
        /* DecodePipe[BlockPhase] += 1 */
        rx->decode_pipe_ptr[rx->block_phase] += 1;
        if (rx->decode_pipe_ptr[rx->block_phase] >= rx->sync_integ_len)
            rx->decode_pipe_ptr[rx->block_phase] -= rx->sync_integ_len;

        if (rx->block_phase == rx->sync_best_block_phase) {
            rx->sync_best_signal = best_slice_signal;
            rx->sync_best_freq_offset = best_slice_offset;
        } else if (best_slice_signal > rx->sync_best_signal) {
            rx->sync_best_signal = best_slice_signal;
            rx->sync_best_block_phase = rx->block_phase;
            rx->sync_best_freq_offset = best_slice_offset;
        }

        dist = (int)rx->block_phase - (int)rx->sync_best_block_phase;
        if (dist < 0)
            dist += (int)rx->block_phases;
        if (dist == (int)(rx->block_phases / 2)) {
            /* pj_mfsk.h:2337 reads NoiseEnergyPtr after the offset loop, one element past
             * the row of this block phase, i.e. offset 0 of the next block phase. For the
             * last block phase that is past the end of the array (undefined in fldigi);
             * this port wraps to the first element instead (documented deviation). */
            size_t past = (size_t)(noise_energy_ptr - rx->sync_noise_energy);
            const fdsp_lowpass3 *np;
            double best_noise;
            if (past >= rx->block_phases * f) {
                past -= rx->block_phases * f;
                rx->wrapped_reads++;
            }
            np = rx->sync_noise_energy + past;
            best_noise = sqrt(np->output);
            if (best_noise == 0)
                rx->sync_snr = 0;
            else
                rx->sync_snr = rx->sync_best_signal / best_noise;
            if (rx->sync_snr >= rx->sync_threshold) {
                size_t bbp = rx->sync_best_block_phase;
                const uint64_t *best_block_ptr = rx->decode_pipe +
                    (bbp * rx->sync_integ_len + rx->decode_pipe_ptr[bbp]) * f;
                uint64_t block = best_block_ptr[rx->sync_best_freq_offset];
                size_t byte;
                for (byte = 0; byte < rx->bits_per_symbol; byte++) {
                    mfsk_fifo_write(&rx->output, (uint8_t)(block & 0xFF));
                    block >>= 8;
                }
            }
            if (rx->sync_snr > 100)
                rx->sync_snr = 0.0;
        }
        rx->block_phase++;
    }
    if (rx->block_phase >= rx->block_phases)
        rx->block_phase -= rx->block_phases;
}

static void rx_process_input_buffer(folivia_rx *rx)
{
    /* MFSK_Receiver::ProcessInputBuffer, pj_mfsk.h:2261-2271 */
    size_t w = rx->inproc.window_len;
    while (rx->input_len >= w) {
        size_t idx;
        inproc_process(&rx->inproc, rx->input_buffer);
        /* InputBuffer.Delete(0, WindowLen), pj_struc.h:247-252 */
        if (rx->input_len - w > 0)
            memmove(rx->input_buffer, rx->input_buffer + w,
                    (rx->input_len - w) * sizeof(double));
        rx->input_len -= w;
        for (idx = 0; idx < w; idx += rx->demod.symbol_separ)
            rx_process_symbol(rx, rx->inproc.output + idx);
    }
}

/* Drain the receiver FIFO like olivia::rx_process (olivia.cxx:276-281): unescape, then
 * only codes above 7 reach the screen. raw = 1 is olivia::rx_flush (olivia.cxx:95-103),
 * which passes every character on unchanged. */
static size_t rx_drain(folivia_rx *rx, uint8_t *chars, size_t max_chars, size_t got,
                       int raw)
{
    uint8_t ch;
    while (mfsk_fifo_read(&rx->output, &ch) > 0) {
        int c = raw ? ch : olivia_unescape(&rx->escape, rx->p.eight_bit, ch);
        if (raw || (c != -1 && c > 7)) {
            if (got < max_chars)
                chars[got] = (uint8_t)c;
            got++;
        }
    }
    return got;
}

size_t folivia_rx_process(folivia_rx *rx, const double *x, size_t n, uint8_t *chars,
                          size_t max_chars)
{
    size_t got = 0;
    if (!rx || !x)
        return 0;
    if (!chars)
        max_chars = 0;
    while (n > 0) {
        size_t batch = n < RX_MAX_BATCH ? n : RX_MAX_BATCH;
        /* MFSK_Receiver::Process, pj_mfsk.h:2228-2233 */
        rx->input_len += mfsk_rateconv_process(&rx->converter, x, batch,
                                               rx->input_buffer + rx->input_len);
        rx_process_input_buffer(rx);
        got = rx_drain(rx, chars, max_chars, got, 0);
        x += batch;
        n -= batch;
    }
    return got;
}

size_t folivia_rx_flush(folivia_rx *rx, uint8_t *chars, size_t max_chars)
{
    /* MFSK_Receiver::Flush, pj_mfsk.h:2235-2252 */
    size_t idx, flush_len, w, got = 0;
    if (!rx)
        return 0;
    if (!chars)
        max_chars = 0;
    w = rx->inproc.window_len;
    rx_process_input_buffer(rx);
    got = rx_drain(rx, chars, max_chars, got, 1);
    for (idx = rx->input_len; idx < w; idx++)
        rx->input_buffer[idx] = 0;
    rx->input_len = w;
    rx_process_input_buffer(rx);
    got = rx_drain(rx, chars, max_chars, got, 1);
    for (idx = 0; idx < w; idx++)
        rx->input_buffer[idx] = 0;
    flush_len = rx->demod.symbol_separ * rx->symbols_per_block * rx->sync_integ_len * 2;
    for (idx = 0; idx < flush_len; idx += w) {
        rx->input_len = w;
        rx_process_input_buffer(rx);
        got = rx_drain(rx, chars, max_chars, got, 1);
    }
    return got;
}

double folivia_rx_frequency_offset_hz(const folivia_rx *rx)
{
    /* MFSK_Receiver::FrequencyOffset, pj_mfsk.h:2216-2219 */
    if (!rx)
        return 0.0;
    return ((int)rx->sync_best_freq_offset - (int)rx->freq_offsets / 2) *
           (rx->sample_rate / rx->demod.symbol_len);
}

double folivia_rx_sync_snr(const folivia_rx *rx)
{
    return rx ? rx->sync_snr : 0.0;
}

double folivia_rx_snr_db(const folivia_rx *rx)
{
    if (!rx || !(rx->sync_snr > 0))
        return -99.0;
    return 20.0 * log10(rx->sync_snr);
}

size_t folivia_rx_deviation_count(const folivia_rx *rx)
{
    return rx ? rx->wrapped_reads : 0;
}

double folivia_rx_metric(const folivia_rx *rx)
{
    /* olivia.cxx:302 */
    double m;
    if (!rx)
        return 0.0;
    m = 5.0 * (rx->sync_snr - 3.0);
    return m < 0 ? 0 : (m > 100 ? 100 : m);
}

void folivia_rx_info(const folivia_rx *rx, folivia_info *info)
{
    /* MFSK_Receiver::PrintParameters, BaudRate, TuneMargin, BlockPeriod,
     * CharactersPerSecond, pj_mfsk.h:2168-2210 */
    double bin;
    if (!rx || !info)
        return;
    memset(info, 0, sizeof *info);
    bin = rx->sample_rate / rx->demod.symbol_len;
    info->tones = (int)rx->tones;
    info->bandwidth_hz = (double)rx->bandwidth;
    info->bits_per_symbol = (int)rx->bits_per_symbol;
    info->symbols_per_block = (int)rx->symbols_per_block;
    info->symbol_len = rx->demod.symbol_len;
    info->symbol_separ = rx->demod.symbol_separ;
    info->first_carrier = rx->demod.first_carrier;
    info->tone_spacing_hz = bin * MFSK_CARRIER_SEPAR;
    info->baud = rx->sample_rate / rx->demod.symbol_separ;
    info->block_period_s = (rx->symbols_per_block * rx->demod.symbol_separ) / rx->sample_rate;
    info->chars_per_second = rx->bits_per_symbol * rx->sample_rate /
                             (rx->symbols_per_block * rx->demod.symbol_separ);
    if (rx->reverse) {
        info->high_hz = bin * rx->demod.first_carrier;
        info->low_hz = info->high_hz - bin * MFSK_CARRIER_SEPAR * (rx->tones - 1);
    } else {
        info->low_hz = bin * rx->demod.first_carrier;
        info->high_hz = info->low_hz + bin * MFSK_CARRIER_SEPAR * (rx->tones - 1);
    }
    info->tune_margin_hz = rx->sync_margin * bin;
    info->sync_threshold = rx->sync_threshold;
}
