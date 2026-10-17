/*
 * fldigi-olivia-core: Olivia and Contestia MFSK receiver and transmitter after fldigi
 * 4.2.13 (Pawel Jalocha's MFSK engine src/include/jalocha/pj_mfsk.h and the modem
 * wrappers src/olivia/olivia.cxx, src/contestia/contestia.cxx).
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * Derived from fldigi 4.2.13, Copyright (C) Pawel Jalocha SP9VRC, Dave Freese W1HKJ and
 * the fldigi developers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Contract "olivia-c1", ABI 1, capability tag "tx=1".
 * RX: 8 kHz audio in (doubles), decoded characters out (fldigi's 8-bit unescape and
 *     control-character filter applied).
 * TX: characters in, 8 kHz audio out exactly as fldigi hands it to the sound card
 *     (start tones, per-batch normalised MFSK symbols, stop tones), before fldigi's TX
 *     level.
 * One library serves both modes; the mode is the `contestia` flag of the configuration.
 */
#ifndef FLDIGI_OLIVIA_H
#define FLDIGI_OLIVIA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FOLIVIA_CONTRACT "olivia-c1"
#define FOLIVIA_ABI 1
/* fldigi's Olivia/Contestia modem rate (olivia.cxx:324). */
#define FOLIVIA_SAMPLE_RATE 8000.0
/* fldigi's default audio centre ("sweet spot" 1500 Hz). */
#define FOLIVIA_DEFAULT_CENTER_HZ 1500.0

typedef struct folivia_config {
    double sample_rate_hz;  /* must be 8000 */
    int tones;              /* 2, 4, 8, 16, 32, 64, 128 or 256 (fldigi's tone menu) */
    double bandwidth_hz;    /* 125, 250, 500, 1000 or 2000 */
    double center_freq_hz;  /* audio centre of the signal (fldigi "frequency") */
    int contestia;          /* 0 = Olivia (7-bit FEC characters), 1 = Contestia (6-bit) */
    int olivia8bit;         /* fldigi olivia8bit / contestia8bit (default 1): bytes > 127
                               travel as 127 + (c & 127); RX undoes the escape */
    int reverse;            /* carriers sent high to low (fldigi "reverse") */
    double sync_threshold;  /* 0: squelch off as in fldigi (Olivia 0, Contestia 3);
                               > 0: Rx->SyncThreshold, clamped to Olivia 0..90 and
                               Contestia 3..90 (fldigi: squelch level / 5 + 3) */
    int sync_margin;        /* tune margin in tone spacings, 0 = fldigi default 8 */
    int sync_integ_len;     /* integration period in FEC blocks, 0 = fldigi default 4 */
    int start_tones;        /* TX: send fldigi's start/stop tones (default 1) */
    uint32_t seed;          /* TX: seed of the phase-jump generator (fldigi uses rand()) */
} folivia_config;

/* Derived parameters of an instance (MFSK_Receiver/MFSK_Transmitter after Preset()). */
typedef struct folivia_info {
    int tones;               /* tones after fldigi's rounding to a power of two */
    double bandwidth_hz;     /* bandwidth after fldigi's rounding */
    int bits_per_symbol;     /* log2(tones) = characters per FEC block */
    int symbols_per_block;   /* 64 (Olivia) or 32 (Contestia) */
    size_t symbol_len;       /* FFT length of one symbol in samples */
    size_t symbol_separ;     /* symbol period in samples (symbol_len / 2) */
    size_t first_carrier;    /* FFT bin of the first (lowest, or highest if reversed) tone */
    double tone_spacing_hz;  /* 2 bins */
    double baud;             /* symbols per second */
    double block_period_s;   /* duration of one FEC block */
    double chars_per_second; /* bits_per_symbol / block_period_s */
    double low_hz;           /* lowest tone (centre of bin) */
    double high_hz;          /* highest tone */
    double tune_margin_hz;   /* RX: +/- search range of the synchroniser */
    double sync_threshold;   /* RX: effective sync S/N threshold */
} folivia_info;

typedef struct folivia_rx folivia_rx;
typedef struct folivia_tx folivia_tx;

/* "fldigi-olivia-core <x.y.z> contract=olivia-c1 abi=1 tx=1 dsp=<dsp version>" */
const char *folivia_version(void);

/* ---------------------------------------------------------------- receiver */

/* NULL on invalid configuration or out of memory. All allocation happens here. */
folivia_rx *folivia_rx_create(const folivia_config *cfg);
void folivia_rx_destroy(folivia_rx *rx);
/* fldigi rx_init(): MFSK_Receiver::Reset() and the escape state. */
void folivia_rx_reset(folivia_rx *rx);
/*
 * Process n samples (any chunking; the result does not depend on it). Writes at most
 * max_chars decoded characters and returns the number decoded (characters beyond
 * max_chars are counted but dropped). Characters are fldigi's put_rx_char() values:
 * unescaped (olivia8bit), codes <= 7 removed. A FEC block of bits_per_symbol characters
 * is released at most every half block period, so n / symbol_separ * 8 + 8 is a safe
 * buffer size.
 */
size_t folivia_rx_process(folivia_rx *rx, const double *x, size_t n,
                          uint8_t *chars, size_t max_chars);
/* olivia::rx_flush(): push the pipeline out with silence (end of a recording). */
size_t folivia_rx_flush(folivia_rx *rx, uint8_t *chars, size_t max_chars);
/* MFSK_Receiver::FrequencyOffset() in Hz (tuning error of the best sync candidate). */
double folivia_rx_frequency_offset_hz(const folivia_rx *rx);
/* MFSK_Receiver::SignalToNoiseRatio(): the synchroniser's FEC S/N (linear amplitude). */
double folivia_rx_sync_snr(const folivia_rx *rx);
/* 20 log10 of folivia_rx_sync_snr(), -99 when it is 0. Not a channel S/N. */
double folivia_rx_snr_db(const folivia_rx *rx);
/* fldigi's signal metric clamp(5 (sync_snr - 3), 0, 100) (olivia.cxx:302). */
double folivia_rx_metric(const folivia_rx *rx);
void folivia_rx_info(const folivia_rx *rx, folivia_info *info);
/* Diagnostics: how often the synchroniser took the one documented deviation from fldigi
 * since create/reset (pj_mfsk.h:2337 reads one element past its array for the last block
 * phase; this port reads the first element instead). While it is 0 the output equals
 * fldigi's on the reference platform. */
size_t folivia_rx_deviation_count(const folivia_rx *rx);

/* ---------------------------------------------------------------- transmitter */

/* NULL on invalid configuration or out of memory. All allocation happens here. The
 * instance starts like fldigi's tx_init(): preamble pending, transmitter running. */
folivia_tx *folivia_tx_create(const folivia_config *cfg);
void folivia_tx_destroy(folivia_tx *tx);
/* fldigi tx_init(): a fresh over (queued text, stop request and output are dropped). */
void folivia_tx_reset(folivia_tx *tx);
/* Queue one keyboard character (fldigi get_tx_char()). Returns 1, or 0 when the queue
 * (4096 characters) is full or the over is stopping. The escape of bytes > 127 and the
 * '.' substitution happen when the modem takes the character, as in tx_process(). */
int folivia_tx_put_char(folivia_tx *tx, uint8_t c);
/* End the over after the queued text (fldigi's ETX): the current FEC block is completed,
 * then the stop tones follow. Without stop, an empty queue sends NUL idle characters. */
void folivia_tx_stop(folivia_tx *tx);
/* Render up to max samples (any chunking). Returns the number written; 0 once the over
 * is complete. Audio is in [-1, 1] (fldigi normalises each symbol batch to peak 1). */
size_t folivia_tx_render(folivia_tx *tx, double *out, size_t max);
/* 1 when the over is complete and every sample has been rendered. */
int folivia_tx_idle(const folivia_tx *tx);
/* Keyboard characters taken by the modem so far (fldigi's echo point). */
size_t folivia_tx_chars_taken(const folivia_tx *tx);
/* Samples rendered so far in this over. */
size_t folivia_tx_samples(const folivia_tx *tx);
void folivia_tx_info(const folivia_tx *tx, folivia_info *info);
/* Verification hook: replace the phase-jump generator by fn(ctx) & 1 (fldigi calls
 * libc rand(), pj_mfsk.h:185). NULL restores the seeded generator. */
void folivia_tx_set_rand(folivia_tx *tx, int (*fn)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* FLDIGI_OLIVIA_H */
