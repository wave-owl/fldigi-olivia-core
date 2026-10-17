/*
 * fldigi-olivia-core test helpers (header-only).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef OLIVIA_TEST_UTIL_H
#define OLIVIA_TEST_UTIL_H

#include "fldigi_olivia.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures __attribute__((unused));

#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                         \
            fprintf(stderr, __VA_ARGS__);                                                \
            fputc('\n', stderr);                                                         \
        }                                                                                \
    } while (0)

/* The platform the goldens were made on (fldigi 4.2.13 bench, macOS arm64, clang with
 * its default FP contraction, optimised so that cos/sin pairs become one sincos call):
 * there the port must reproduce fldigi bit for bit. The sanitizer build (-O1) defines
 * OLIVIA_NOT_REFERENCE. */
#if defined(__APPLE__) && defined(__aarch64__) && !defined(OLIVIA_NOT_REFERENCE)
#define REFERENCE_PLATFORM 1
#else
#define REFERENCE_PLATFORM 0
#endif

static inline void *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

static inline folivia_config make_config(int tones, double bw, int contestia)
{
    folivia_config c;
    memset(&c, 0, sizeof c);
    c.sample_rate_hz = FOLIVIA_SAMPLE_RATE;
    c.tones = tones;
    c.bandwidth_hz = bw;
    c.center_freq_hz = FOLIVIA_DEFAULT_CENTER_HZ;
    c.contestia = contestia;
    c.olivia8bit = 1;
    c.start_tones = 1;
    c.seed = 12345;
    return c;
}

/* Render a whole over: text, stop, everything until the transmitter is idle. Returns a
 * malloc'ed buffer of *n samples. chunk = samples per render call. */
static inline double *render_over(folivia_tx *tx, const char *text, size_t len, size_t chunk,
                           size_t *n)
{
    size_t cap = 1 << 16, got = 0, i, k;
    double *buf = (double *)malloc(cap * sizeof(double));
    for (i = 0; i < len; i++)
        folivia_tx_put_char(tx, (uint8_t)text[i]);
    folivia_tx_stop(tx);
    for (;;) {
        if (got + chunk > cap) {
            cap *= 2;
            buf = (double *)realloc(buf, cap * sizeof(double));
        }
        k = folivia_tx_render(tx, buf + got, chunk);
        if (k == 0)
            break;
        got += k;
    }
    *n = got;
    return buf;
}

/* Decode audio in chunks of `chunk` samples; returns the characters (malloc'ed, NUL
 * terminated) and their count in *n. */
static inline uint8_t *decode(folivia_rx *rx, const double *x, size_t n, size_t chunk, size_t *got)
{
    size_t cap = n / 4 + 1024, pos = 0, total = 0;
    uint8_t *out = (uint8_t *)malloc(cap + 1);
    while (pos < n) {
        size_t k = n - pos < chunk ? n - pos : chunk;
        size_t r = folivia_rx_process(rx, x + pos, k, out + total, cap - total);
        total += r;
        pos += k;
    }
    out[total] = 0;
    *got = total;
    return out;
}

/* Edit distance between `ref` and the best-matching window of `hyp` (garbage before and
 * after the burst is free, as in the wave-owl bench CER). */
static inline size_t window_distance(const uint8_t *ref, size_t nr, const uint8_t *hyp, size_t nh)
{
    size_t *prev = (size_t *)malloc((nh + 1) * sizeof(size_t));
    size_t *cur = (size_t *)malloc((nh + 1) * sizeof(size_t));
    size_t i, j, best;
    for (j = 0; j <= nh; j++)
        prev[j] = 0;
    for (i = 1; i <= nr; i++) {
        size_t *t;
        cur[0] = i;
        for (j = 1; j <= nh; j++) {
            size_t sub = prev[j - 1] + (ref[i - 1] != hyp[j - 1]);
            size_t del = prev[j] + 1, ins = cur[j - 1] + 1;
            cur[j] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
        }
        t = prev;
        prev = cur;
        cur = t;
    }
    best = prev[0];
    for (j = 1; j <= nh; j++)
        if (prev[j] < best)
            best = prev[j];
    free(prev);
    free(cur);
    return best;
}

/* Does `text` occur in `hay`? */
static inline int contains(const uint8_t *hay, size_t n, const char *text)
{
    size_t m = strlen(text), i;
    if (m > n)
        return 0;
    for (i = 0; i + m <= n; i++)
        if (memcmp(hay + i, text, m) == 0)
            return 1;
    return 0;
}

#endif
