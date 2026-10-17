/*
 * fldigi-olivia-core transmitter: MFSK_Encoder, MFSK_Modulator and MFSK_Transmitter of
 * fldigi 4.2.13 (src/include/jalocha/pj_mfsk.h) driven like olivia::tx_init,
 * olivia::send_tones and olivia::tx_process (src/olivia/olivia.cxx:52-229) and their
 * Contestia counterparts (src/contestia/contestia.cxx:44-203).
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * Derived from fldigi 4.2.13, (c) 2004 Pawel Jalocha SP9VRC, (c) 2006-2019 Dave Freese
 * W1HKJ and the fldigi developers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The output is what fldigi passes to modem::ModulateXmtr(): start tones, the MFSK
 * symbols normalised per batch to peak 1, stop tones (Olivia: plus one 512-sample block
 * of silence). fldigi's TX level (-12 dB by default, modem.cxx:652-658) is left to the caller.
 */
#include "olivia_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TX_STATE_RUNNING 0x0001  /* pj_mfsk.h:1636 */
#define TX_STATE_STOPREQ 0x0010  /* pj_mfsk.h:1637 */
#define TX_KEYBOARD_LEN 4096
/* get_tx_char() results (fl_digi.cxx) */
#define GET_TX_CHAR_NODATA (-1)
#define GET_TX_CHAR_ETX (-3)

struct folivia_tx {
    olivia_params p;

    /* MFSK_Transmitter primary/secondary parameters, pj_mfsk.h:1619-1634 */
    size_t tones;
    size_t bandwidth;
    float first_carrier_multiplier;
    int reverse;
    size_t bits_per_symbol;
    size_t symbols_per_block;
    size_t max_output_len;
    int state;
    mfsk_fifo input;              /* characters to be encoded (Len 1024) */
    uint8_t input_block[8];
    size_t symbol_ptr;

    /* MFSK_Encoder, pj_mfsk.h:1060-1217 */
    uint64_t scrambling_code;
    int8_t fht_buffer[64];
    uint8_t output_block[64];

    /* MFSK_Modulator<double>, pj_mfsk.h:58-238 */
    size_t symbol_len;
    size_t first_carrier;
    size_t carriers;
    size_t symbol_separ;
    int symbol_phase;
    double *cosine_table;
    double *out_tap;
    size_t tap_ptr;
    size_t wrap_mask;

    double *modulator_output;
    mfsk_rateconv converter;
    double *converter_output;

    /* olivia modem state, olivia.h:38-75 */
    int preamblesent;
    int postamblesent;
    int stopflag;
    double preamblephase;
    double ampshape[OLIVIA_SR4];
    double tonebuff[OLIVIA_TONE_DURATION];

    /* keyboard queue: the source of get_tx_char() */
    uint8_t kbd[TX_KEYBOARD_LEN];
    size_t kbd_head;
    size_t kbd_count;
    int stop_requested;
    size_t chars_taken;

    /* rendered but not yet delivered samples of one tx_process() step */
    double *stage;
    size_t stage_cap;
    size_t stage_len;
    size_t stage_pos;
    int finished;
    size_t samples;

    /* phase-jump generator (fldigi: rand(), pj_mfsk.h:185) */
    uint32_t rng;
    int (*rand_fn)(void *);
    void *rand_ctx;
};

static int tx_rand_bit(folivia_tx *tx)
{
    /* Deviation from fldigi: libc rand() is process-global and unseeded; this port uses
     * a per-instance xorshift32 seeded from the configuration, unless a verification
     * hook supplies the values. Only bit 0 is used, as in fldigi. */
    uint32_t x;
    if (tx->rand_fn)
        return tx->rand_fn(tx->rand_ctx) & 1;
    x = tx->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    tx->rng = x;
    return (int)(x >> 16) & 1;
}

/* ---------------------------------------------------------------------------------- */
/* MFSK_Encoder                                                                       */
/* ---------------------------------------------------------------------------------- */

static void encoder_encode_character(folivia_tx *tx, uint8_t ch)
{
    /* MFSK_Encoder::EncodeCharacter, pj_mfsk.h:1133-1166 */
    size_t time_bit, spb = tx->symbols_per_block;
    uint8_t mask = (uint8_t)((spb << 1) - 1);

    if (tx->p.contestia) {
        if (ch >= 'a' && ch <= 'z')
            ch = (uint8_t)(ch + ('A' - 'a'));
        if (ch == ' ')
            ch = 59;
        else if (ch == '\r')
            ch = 60;
        else if (ch == '\n')
            ch = 0;
        else if (ch >= 33 && ch <= 90)
            ch = (uint8_t)(ch - 32);
        else if (ch == 8)
            ch = 61;
        else if (ch == 0)
            ch = 0;
        else
            ch = '?' - 32;
    } else {
        ch &= mask;
    }
    for (time_bit = 0; time_bit < spb; time_bit++)
        tx->fht_buffer[time_bit] = 0;
    if (ch < spb)
        tx->fht_buffer[ch] = 1;
    else
        tx->fht_buffer[ch - spb] = (-1);
    fdsp_ifht_i8(tx->fht_buffer, spb);
}

static void encoder_scramble_fht(folivia_tx *tx, size_t code_offset)
{
    /* MFSK_Encoder::ScrambleFHT, pj_mfsk.h:1168-1177 */
    size_t time_bit, code_wrap = tx->symbols_per_block - 1;
    size_t code_bit = code_offset & code_wrap;
    for (time_bit = 0; time_bit < tx->symbols_per_block; time_bit++) {
        uint64_t code_mask = 1;
        code_mask <<= code_bit;
        if (tx->scrambling_code & code_mask)
            tx->fht_buffer[time_bit] = (int8_t)(-tx->fht_buffer[time_bit]);
        code_bit += 1;
        code_bit &= code_wrap;
    }
}

static void encoder_encode_block(folivia_tx *tx, const uint8_t *input_block)
{
    /* MFSK_Encoder::EncodeBlock, pj_mfsk.h:1179-1207 */
    size_t freq_bit, time_bit;
    size_t n_shift = tx->p.contestia ? 5 : 13;
    for (time_bit = 0; time_bit < tx->symbols_per_block; time_bit++)
        tx->output_block[time_bit] = 0;
    for (freq_bit = 0; freq_bit < tx->bits_per_symbol; freq_bit++) {
        size_t rotate = 0;
        encoder_encode_character(tx, input_block[freq_bit]);
        encoder_scramble_fht(tx, freq_bit * n_shift);
        for (time_bit = 0; time_bit < tx->symbols_per_block; time_bit++) {
            if (tx->fht_buffer[time_bit] < 0) {
                size_t bit = freq_bit + rotate;
                uint8_t mask = 1;
                if (bit >= tx->bits_per_symbol)
                    bit -= tx->bits_per_symbol;
                mask = (uint8_t)(mask << bit);
                tx->output_block[time_bit] |= mask;
            }
            rotate += 1;
            if (rotate >= tx->bits_per_symbol)
                rotate -= tx->bits_per_symbol;
        }
    }
}

/* ---------------------------------------------------------------------------------- */
/* MFSK_Modulator                                                                     */
/* ---------------------------------------------------------------------------------- */

static void modulator_preset(folivia_tx *tx)
{
    /* MFSK_Modulator::Preset, pj_mfsk.h:117-165 (SymbolShape is computed there but not
     * used by AddSymbol, pj_mfsk.h:226-229, so it is not ported) */
    size_t idx;
    tx->carriers = mfsk_exp2((uint32_t)tx->bits_per_symbol);
    tx->symbol_separ = tx->symbol_len / 2;
    for (idx = 0; idx < tx->symbol_len; idx++)
        tx->cosine_table[idx] = cos((2 * M_PI * idx) / tx->symbol_len);
    for (idx = 0; idx < tx->symbol_len; idx++)
        tx->out_tap[idx] = 0;
    tx->tap_ptr = 0;
    tx->wrap_mask = tx->symbol_len - 1;
    tx->symbol_phase = 0;
}

static int wrap_phase(int phase, size_t wrap_mask)
{
    /* int &= size_t mask, as SymbolPhase &= WrapMask (pj_mfsk.h:176) */
    return (int)((size_t)phase & wrap_mask);
}

static void modulator_add_symbol(folivia_tx *tx, int freq, int phase)
{
    /* MFSK_Modulator::AddSymbol, pj_mfsk.h:223-236 */
    size_t time;
    for (time = 0; time < tx->symbol_len; time++) {
        double shape = 1.0 - tx->cosine_table[time];
        tx->out_tap[tx->tap_ptr] += tx->cosine_table[phase] * shape;
        phase += freq;
        phase = wrap_phase(phase, tx->wrap_mask);
        tx->tap_ptr += 1;
        tx->tap_ptr &= tx->wrap_mask;
    }
}

static void modulator_send(folivia_tx *tx, uint8_t symbol)
{
    /* MFSK_Modulator::Send, pj_mfsk.h:167-188 */
    int symbol_freq, time_shift, phase_differ;
    symbol = fdsp_gray_code(symbol);
    if (tx->reverse == 1) {
        int rev_first_car = (int)tx->first_carrier - 2;
        symbol_freq = rev_first_car - MFSK_CARRIER_SEPAR * (int)symbol;
    } else {
        symbol_freq = (int)tx->first_carrier + MFSK_CARRIER_SEPAR * (int)symbol;
    }
    time_shift = (int)(tx->symbol_separ / 2) - (int)(tx->symbol_len / 2);
    tx->symbol_phase += symbol_freq * time_shift;
    tx->symbol_phase = wrap_phase(tx->symbol_phase, tx->wrap_mask);
    modulator_add_symbol(tx, symbol_freq, tx->symbol_phase);
    time_shift = (int)(tx->symbol_separ / 2) + (int)(tx->symbol_len / 2);
    tx->symbol_phase += symbol_freq * time_shift;
    tx->symbol_phase = wrap_phase(tx->symbol_phase, tx->wrap_mask);
    phase_differ = (int)(tx->symbol_len / 4);
    if (tx_rand_bit(tx))
        phase_differ = (-phase_differ);
    tx->symbol_phase += phase_differ;
    tx->symbol_phase = wrap_phase(tx->symbol_phase, tx->wrap_mask);
}

static size_t modulator_output(folivia_tx *tx, double *buffer)
{
    /* MFSK_Modulator::Output<double>, pj_mfsk.h:209-219 */
    size_t idx;
    for (idx = 0; idx < tx->symbol_separ; idx++) {
        buffer[idx] = tx->out_tap[tx->tap_ptr];
        tx->out_tap[tx->tap_ptr] = 0;
        tx->tap_ptr += 1;
        tx->tap_ptr &= tx->wrap_mask;
    }
    return tx->symbol_separ;
}

/* ---------------------------------------------------------------------------------- */
/* MFSK_Transmitter                                                                   */
/* ---------------------------------------------------------------------------------- */

static void transmitter_preset(folivia_tx *tx)
{
    /* MFSK_Transmitter::Preset, pj_mfsk.h:1693-1747 (arrays allocated at create) */
    mfsk_fifo_reset(&tx->input);
    tx->symbol_ptr = 0;
    tx->state = 0;
    modulator_preset(tx);
    mfsk_rateconv_reset(&tx->converter);
}

static size_t transmitter_output(folivia_tx *tx, double *buffer)
{
    /* MFSK_Transmitter::Output, pj_mfsk.h:1813-1847 */
    size_t mod_len, conv_len, idx;
    double maxval = 0, tempnum;
    if (tx->symbol_ptr == 0) {
        if ((tx->state & TX_STATE_STOPREQ) && mfsk_fifo_empty(&tx->input)) {
            tx->state = 0;
        } else if (tx->state & TX_STATE_RUNNING) {
            for (idx = 0; idx < tx->bits_per_symbol; idx++) {
                uint8_t ch;
                if (mfsk_fifo_read(&tx->input, &ch) <= 0)
                    break;
                tx->input_block[idx] = ch;
            }
            for (; idx < tx->bits_per_symbol; idx++)
                tx->input_block[idx] = 0;
            encoder_encode_block(tx, tx->input_block);
        }
    }
    if (tx->state & TX_STATE_RUNNING) {
        modulator_send(tx, tx->output_block[tx->symbol_ptr]);
        tx->symbol_ptr += 1;
        if (tx->symbol_ptr >= tx->symbols_per_block)
            tx->symbol_ptr = 0;
    }
    mod_len = modulator_output(tx, tx->modulator_output);
    conv_len = mfsk_rateconv_process(&tx->converter, tx->modulator_output, mod_len,
                                     tx->converter_output);
    for (idx = 0; idx < conv_len; idx++)
        if ((tempnum = fabs(tx->converter_output[idx])) > maxval)
            maxval = tempnum;
    /* Deviation: fldigi divides by maxval even when it is 0 (0/0); a silent batch stays
     * silent here. fldigi's own sequence never produces one. */
    for (idx = 0; idx < conv_len; idx++)
        buffer[idx] = maxval > 0 ? (double)tx->converter_output[idx] / maxval : 0.0;
    return conv_len;
}

/* ---------------------------------------------------------------------------------- */
/* olivia / contestia modem                                                           */
/* ---------------------------------------------------------------------------------- */

static double tx_nco(folivia_tx *tx, double freq)
{
    /* olivia::nco, olivia.cxx:52-60 (wrap at pi); contestia::nco, contestia.cxx:44-52
     * (wrap at 2 pi). samplerate is fldigi's int 8000. */
    tx->preamblephase += 2.0 * M_PI * freq / 8000;
    if (tx->p.contestia) {
        if (tx->preamblephase > (2.0 * M_PI))
            tx->preamblephase -= (2.0 * M_PI);
    } else {
        if (tx->preamblephase > M_PI)
            tx->preamblephase -= 2.0 * M_PI;
    }
    return cos(tx->preamblephase);
}

static void stage_put(folivia_tx *tx, const double *x, size_t n)
{
    memcpy(tx->stage + tx->stage_len, x, n * sizeof(double));
    tx->stage_len += n;
}

static void send_tones(folivia_tx *tx)
{
    /* olivia::send_tones, olivia.cxx:105-132; contestia::send_tones, contestia.cxx:90-117 */
    double freqa, freqb, tone_bw, tone_midfreq;
    int i;
    if (!tx->p.start_tones)
        return;
    /* tone_bw = modem::bandwidth, which olivia::restart sets to
     * Tx->Bandwidth - Tx->Bandwidth / Tx->Tones in integer arithmetic (olivia.cxx:379,
     * contestia.cxx:325, modem.cxx:361-365): the tones sit on the outer carriers. */
    tone_bw = (double)(tx->bandwidth - tx->bandwidth / tx->tones);
    tone_midfreq = tx->p.center;
    if (tx->p.reverse) {
        freqa = tone_midfreq + (tone_bw / 2.0);
        freqb = tone_midfreq - (tone_bw / 2.0);
    } else {
        freqa = tone_midfreq - (tone_bw / 2.0);
        freqb = tone_midfreq + (tone_bw / 2.0);
    }
    tx->preamblephase = 0;
    for (i = 0; i < OLIVIA_SR4; i++)
        tx->tonebuff[2 * OLIVIA_SR4 + i] = tx->tonebuff[i] = tx_nco(tx, freqa) * tx->ampshape[i];
    tx->preamblephase = 0;
    for (i = 0; i < OLIVIA_SR4; i++)
        tx->tonebuff[3 * OLIVIA_SR4 + i] = tx->tonebuff[OLIVIA_SR4 + i] =
            tx_nco(tx, freqb) * tx->ampshape[i];
    stage_put(tx, tx->tonebuff, OLIVIA_TONE_DURATION);
}

static int get_tx_char(folivia_tx *tx)
{
    /* fldigi get_tx_char(): the next keyboard character, ETX after a stop request, else
     * NODATA (fl_digi.cxx:9770-) */
    uint8_t c;
    if (tx->kbd_count == 0)
        return tx->stop_requested ? GET_TX_CHAR_ETX : GET_TX_CHAR_NODATA;
    c = tx->kbd[tx->kbd_head];
    tx->kbd_head = (tx->kbd_head + 1) % TX_KEYBOARD_LEN;
    tx->kbd_count--;
    tx->chars_taken++;
    return c;
}

/* One olivia::tx_process() / contestia::tx_process() call (olivia.cxx:160-229,
 * contestia.cxx:143-203), output appended to the stage. */
static void tx_step(folivia_tx *tx)
{
    int c = 0;
    size_t len;
    if (tx->preamblesent != 1) {
        send_tones(tx);
        tx->preamblesent = 1;
        /* "Olivia Transmitter class requires at least character" */
        mfsk_fifo_write(&tx->input, 0);
    }
    if (tx->stopflag || (mfsk_fifo_read_ready(&tx->input) < tx->bits_per_symbol)) {
        if (!tx->stopflag && (c = get_tx_char(tx)) == GET_TX_CHAR_ETX)
            tx->stopflag = 1;
        if (tx->stopflag) {
            tx->state |= TX_STATE_STOPREQ;
        } else {
            if (c == GET_TX_CHAR_NODATA)
                c = 0;
            if (tx->p.contestia) {
                /* contestia.cxx:171-181 */
                if (c > (tx->p.eight_bit ? 255 : 127))
                    c = '.';
                if (c > 127) {
                    c &= 127;
                    mfsk_fifo_write(&tx->input, 127);
                }
                mfsk_fifo_write(&tx->input, (uint8_t)c);
            } else if (c > 127) {
                /* olivia.cxx:195-206 */
                if (tx->p.eight_bit && c <= 255) {
                    mfsk_fifo_write(&tx->input, 127);
                    mfsk_fifo_write(&tx->input, (uint8_t)(c & 127));
                } else {
                    c = '.';
                    mfsk_fifo_write(&tx->input, (uint8_t)c);
                }
            } else {
                mfsk_fifo_write(&tx->input, (uint8_t)c);
            }
        }
    }
    len = transmitter_output(tx, tx->stage + tx->stage_len);
    tx->stage_len += len;
    if (tx->stopflag && tx->state == 0 && tx->postamblesent != 1) {
        tx->postamblesent = 1;
        send_tones(tx);
        if (!tx->p.contestia) {
            /* olivia.cxx:219-220: one block of silence after the stop tones */
            memset(tx->stage + tx->stage_len, 0, OLIVIA_SCBLOCKSIZE * sizeof(double));
            tx->stage_len += OLIVIA_SCBLOCKSIZE;
        }
    }
    if (!(tx->state & TX_STATE_RUNNING)) {
        tx->stopflag = 0;
        tx->finished = 1;
    }
}

void folivia_tx_reset(folivia_tx *tx)
{
    /* olivia::tx_init, olivia.cxx:62-93; contestia::tx_init, contestia.cxx:54-80 */
    if (!tx)
        return;
    tx->preamblesent = 0;
    tx->postamblesent = 0;
    tx->stopflag = 0;
    tx->preamblephase = 0;
    transmitter_preset(tx);
    tx->state |= TX_STATE_RUNNING; /* Tx->Start() */
    tx->kbd_head = tx->kbd_count = 0;
    tx->stop_requested = 0;
    tx->chars_taken = 0;
    tx->stage_len = tx->stage_pos = 0;
    tx->finished = 0;
    tx->samples = 0;
    tx->rng = tx->p.seed ? tx->p.seed : 0x2545F491u;
}

void folivia_tx_destroy(folivia_tx *tx)
{
    if (!tx)
        return;
    mfsk_fifo_free(&tx->input);
    free(tx->cosine_table);
    free(tx->out_tap);
    free(tx->modulator_output);
    mfsk_rateconv_free(&tx->converter);
    free(tx->converter_output);
    free(tx->stage);
    free(tx);
}

folivia_tx *folivia_tx_create(const folivia_config *cfg)
{
    folivia_tx *tx;
    olivia_params p;
    int i;

    if (olivia_params_from_config(cfg, &p) < 0)
        return NULL;
    tx = (folivia_tx *)calloc(1, sizeof *tx);
    if (!tx)
        return NULL;
    tx->p = p;
    /* olivia::restart, olivia.cxx:324-331: Tones, Bandwidth, rates */
    tx->tones = p.tones;
    tx->bandwidth = p.bandwidth;
    tx->reverse = p.reverse;
    /* tx_init: FirstCarrierMultiplier (double fc_offset for Olivia, olivia.cxx:78-85;
     * Bandwidth / 2 for Contestia, contestia.cxx:67-73) */
    if (p.contestia) {
        if (p.reverse)
            tx->first_carrier_multiplier = (float)((p.center + (double)(p.bandwidth / 2)) / 500);
        else
            tx->first_carrier_multiplier = (float)((p.center - (double)(p.bandwidth / 2)) / 500);
    } else {
        double fc_offset = olivia_fc_offset(p.bandwidth, p.tones);
        if (p.reverse)
            tx->first_carrier_multiplier = (float)((p.center + fc_offset) / 500.0);
        else
            tx->first_carrier_multiplier = (float)((p.center - fc_offset) / 500.0);
    }
    if (!(tx->first_carrier_multiplier > 0))
        goto error;
    /* MFSK_Transmitter::Preset, pj_mfsk.h:1693-1747 */
    tx->bits_per_symbol = mfsk_log2((uint32_t)tx->tones);
    tx->tones = mfsk_exp2((uint32_t)tx->bits_per_symbol);
    if (mfsk_fifo_init(&tx->input, 1024) < 0)
        goto error;
    tx->scrambling_code = p.contestia ? CONTESTIA_SCRAMBLING_CODE : OLIVIA_SCRAMBLING_CODE;
    tx->symbols_per_block = mfsk_exp2((uint32_t)((p.contestia ? 6 : 7) - 1));
    tx->symbol_len = (size_t)1 << (tx->bits_per_symbol + 7 -
                                   mfsk_log2((uint32_t)(tx->bandwidth / 125)));
    tx->bandwidth = mfsk_exp2((uint32_t)mfsk_log2((uint32_t)(tx->bandwidth / 125))) * 125;
    tx->first_carrier = (size_t)((tx->symbol_len / 16) * tx->first_carrier_multiplier) + 1;
    if (tx->reverse) {
        if (tx->first_carrier < 2 + MFSK_CARRIER_SEPAR * (tx->tones - 1) ||
            tx->first_carrier >= tx->symbol_len / 2)
            goto error;
    } else if (tx->first_carrier + MFSK_CARRIER_SEPAR * (tx->tones - 1) >= tx->symbol_len / 2) {
        goto error;
    }
    tx->cosine_table = (double *)calloc(tx->symbol_len, sizeof(double));
    tx->out_tap = (double *)calloc(tx->symbol_len, sizeof(double));
    tx->modulator_output = (double *)calloc(tx->symbol_len / 2, sizeof(double));
    if (!tx->cosine_table || !tx->out_tap || !tx->modulator_output)
        goto error;
    if (mfsk_rateconv_init(&tx->converter, FOLIVIA_SAMPLE_RATE / FOLIVIA_SAMPLE_RATE) < 0)
        goto error;
    tx->max_output_len = (size_t)ceil(tx->symbol_len / 2 * FOLIVIA_SAMPLE_RATE /
                                      FOLIVIA_SAMPLE_RATE + 2);
    tx->converter_output = (double *)calloc(tx->max_output_len, sizeof(double));
    /* one step: start tones + one batch + stop tones + one silent block */
    tx->stage_cap = 2 * OLIVIA_TONE_DURATION + tx->max_output_len + OLIVIA_SCBLOCKSIZE;
    tx->stage = (double *)calloc(tx->stage_cap, sizeof(double));
    if (!tx->converter_output || !tx->stage)
        goto error;
    /* olivia::olivia, olivia.cxx:522-526: tone envelope */
    for (i = 0; i < OLIVIA_SR4; i++)
        tx->ampshape[i] = 1.0;
    for (i = 0; i < OLIVIA_SR4 / 8; i++)
        tx->ampshape[i] = tx->ampshape[OLIVIA_SR4 - 1 - i] =
            0.5 * (1.0 - cos(M_PI * i / (OLIVIA_SR4 / 8)));
    folivia_tx_reset(tx);
    return tx;

error:
    folivia_tx_destroy(tx);
    return NULL;
}

int folivia_tx_put_char(folivia_tx *tx, uint8_t c)
{
    if (!tx || tx->stop_requested || tx->finished || tx->kbd_count >= TX_KEYBOARD_LEN)
        return 0;
    tx->kbd[(tx->kbd_head + tx->kbd_count) % TX_KEYBOARD_LEN] = c;
    tx->kbd_count++;
    return 1;
}

void folivia_tx_stop(folivia_tx *tx)
{
    if (tx)
        tx->stop_requested = 1;
}

size_t folivia_tx_render(folivia_tx *tx, double *out, size_t max)
{
    size_t done = 0;
    if (!tx || !out)
        return 0;
    while (done < max) {
        size_t n;
        if (tx->stage_pos == tx->stage_len) {
            if (tx->finished)
                break;
            tx->stage_len = tx->stage_pos = 0;
            tx_step(tx);
            continue;
        }
        n = tx->stage_len - tx->stage_pos;
        if (n > max - done)
            n = max - done;
        memcpy(out + done, tx->stage + tx->stage_pos, n * sizeof(double));
        tx->stage_pos += n;
        done += n;
    }
    tx->samples += done;
    return done;
}

int folivia_tx_idle(const folivia_tx *tx)
{
    return !tx || (tx->finished && tx->stage_pos == tx->stage_len);
}

size_t folivia_tx_chars_taken(const folivia_tx *tx)
{
    return tx ? tx->chars_taken : 0;
}

size_t folivia_tx_samples(const folivia_tx *tx)
{
    return tx ? tx->samples : 0;
}

void folivia_tx_set_rand(folivia_tx *tx, int (*fn)(void *ctx), void *ctx)
{
    if (!tx)
        return;
    tx->rand_fn = fn;
    tx->rand_ctx = ctx;
}

void folivia_tx_info(const folivia_tx *tx, folivia_info *info)
{
    /* MFSK_Transmitter::BaudRate, BlockPeriod, CharactersPerSecond, pj_mfsk.h:1757-1769 */
    double bin;
    if (!tx || !info)
        return;
    memset(info, 0, sizeof *info);
    bin = FOLIVIA_SAMPLE_RATE / tx->symbol_len;
    info->tones = (int)tx->tones;
    info->bandwidth_hz = (double)tx->bandwidth;
    info->bits_per_symbol = (int)tx->bits_per_symbol;
    info->symbols_per_block = (int)tx->symbols_per_block;
    info->symbol_len = tx->symbol_len;
    info->symbol_separ = tx->symbol_separ;
    info->first_carrier = tx->first_carrier;
    info->tone_spacing_hz = bin * MFSK_CARRIER_SEPAR;
    info->baud = FOLIVIA_SAMPLE_RATE / tx->symbol_separ;
    info->block_period_s = (tx->symbols_per_block * tx->symbol_separ) / FOLIVIA_SAMPLE_RATE;
    info->chars_per_second = tx->bits_per_symbol * FOLIVIA_SAMPLE_RATE /
                             (tx->symbols_per_block * tx->symbol_separ);
    if (tx->reverse) {
        info->high_hz = bin * (tx->first_carrier - 2);
        info->low_hz = info->high_hz - bin * MFSK_CARRIER_SEPAR * (tx->tones - 1);
    } else {
        info->low_hz = bin * tx->first_carrier;
        info->high_hz = info->low_hz + bin * MFSK_CARRIER_SEPAR * (tx->tones - 1);
    }
}
