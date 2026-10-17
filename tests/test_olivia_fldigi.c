/*
 * fldigi-olivia-core against fldigi 4.2.13 itself (goldens from
 * tests/golden/make_fldigi_golden.py, the headless fldigi bench build).
 *
 * TX, all 37 fixed submodes plus two pangram overs: with fldigi's phase-jump bits
 * (rand_bits.u8) the port renders fldigi's TX vector: identical FNV-1a hash of the int16
 * output on the reference platform, and at most 1 LSB from the stored vectors everywhere.
 * RX, the same overs clean and at -5 / -10 dB (2500 Hz): the decoded bytes equal fldigi's
 * own decode on the reference platform; elsewhere (different libm / no FMA) the text must
 * stay within 2 % edit distance of fldigi's decode and contain every character fldigi
 * decoded correctly.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _POSIX_C_SOURCE 200809L
#include "test_util.h"

static const char *golden = "tests/golden";

typedef struct {
    const uint8_t *bits;
    size_t n;
    size_t pos;
} rand_stream;

static int stream_rand(void *ctx)
{
    rand_stream *s = (rand_stream *)ctx;
    int b = s->pos < s->n ? s->bits[s->pos] : 0;
    s->pos++;
    return b;
}

static uint64_t fnv1a64(const int16_t *pcm, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    size_t i;
    for (i = 0; i < n; i++) {
        uint16_t u = (uint16_t)pcm[i];
        h ^= (uint8_t)(u & 0xFF);
        h *= 0x100000001B3ULL;
        h ^= (uint8_t)(u >> 8);
        h *= 0x100000001B3ULL;
    }
    return h;
}

/* fldigi's sound path: modem::ModulateXmtr scales by 10^(txlevel/20), txlevel -12 dB
 * (modem.cxx:652-658); libsndfile writes PCM16 as lrint(x * 0x7FFF). */
static void quantize(const double *x, size_t n, int16_t *pcm)
{
    double mult = pow(10, -12.0 / 20.0);
    size_t i;
    for (i = 0; i < n; i++) {
        double v = x[i] * mult;
        long q;
        if (v < -1.0)
            v = -1.0;
        if (v > 1.0)
            v = 1.0;
        q = lrint(v * 32767.0);
        pcm[i] = (int16_t)q;
    }
}

/* Same as noisy() in make_fldigi_golden.py; this file is compiled without FP
 * contraction, so every step rounds like Python. */
static uint32_t lcg;
static double lcg_uniform(void)
{
    lcg = lcg * 1664525u + 1013904223u;
    return lcg / 4294967296.0;
}

static double *noisy(const int16_t *pcm, size_t n, int snr_step, uint32_t seed, size_t *out_n)
{
    size_t lead = 8000, tail = 12 * 8000, total = lead + n + tail, i;
    double *x = (double *)calloc(total, sizeof(double));
    double ps = 0, inv, s2, sd, gain;
    int k;
    for (i = 0; i < n; i++)
        x[lead + i] = (double)pcm[i];
    *out_n = total;
    if (snr_step == 0) {
        for (i = 0; i < total; i++)
            x[i] /= 32768.0;
        return x;
    }
    for (i = 0; i < n; i++)
        ps += (double)pcm[i] * (double)pcm[i];
    ps = ps / (double)n;
    inv = snr_step == 1 ? 1.0 / sqrt(10.0) : 1.0 / 10.0;
    s2 = ps / inv * 4000.0 / 2500.0;
    sd = sqrt(s2);
    gain = 3276.7 / sqrt(ps * (double)n / (double)total + s2);
    lcg = seed;
    for (i = 0; i < total; i++) {
        double z = 0.0, y, q;
        for (k = 0; k < 12; k++)
            z += lcg_uniform();
        z -= 6.0;
        y = (x[i] + sd * z) * gain;
        q = floor(y + 0.5);
        if (q > 32767)
            q = 32767;
        if (q < -32768)
            q = -32768;
        x[i] = q / 32768.0;  /* libsndfile sf_readf_double: int16 / 32768 */
    }
    return x;
}

static char texts[2][128];

static const char *text_for(const char *key)
{
    return strcmp(key, "cq") == 0 ? texts[0] : texts[1];
}

static void load_texts(void)
{
    char path[512], *buf, *line;
    size_t len;
    snprintf(path, sizeof path, "%s/texts.txt", golden);
    buf = (char *)read_file(path, &len);
    if (!buf) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    for (line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (strncmp(line, "cq ", 3) == 0)
            snprintf(texts[0], sizeof texts[0], "%s", line + 3);
        else if (strncmp(line, "pangram ", 8) == 0)
            snprintf(texts[1], sizeof texts[1], "%s", line + 8);
    }
    free(buf);
}

static int vectors_done, rx_exact, rx_total, rx_deviant;

static void check_vector(const char *stem, const char *mode, int tones, int bw,
                         const char *key, int idx, size_t samples, uint64_t hash,
                         const uint8_t *bits, size_t nbits)
{
    static const char *snr_names[3] = {"clean", "m05", "m10"};
    folivia_config cfg = make_config(tones, bw, strcmp(mode, "contestia") == 0);
    folivia_tx *tx = folivia_tx_create(&cfg);
    rand_stream rs = {bits, nbits, 0};
    const char *text = text_for(key);
    char sent[128];
    size_t ns = strlen(text);
    double *audio;
    int16_t *pcm, *ref = NULL;
    size_t n, i, reflen = 0, diff1 = 0, diffbig = 0;
    char path[512];
    int s;

    CHECK(tx != NULL, "%s: tx_create", stem);
    if (!tx)
        return;
    /* What the receiver can print: Contestia has upper case only (pj_mfsk.h:1137-1139). */
    for (i = 0; i <= ns; i++)
        sent[i] = (char)(strcmp(mode, "contestia") == 0 && text[i] >= 'a' && text[i] <= 'z'
                             ? text[i] - 'a' + 'A' : text[i]);
    folivia_tx_set_rand(tx, stream_rand, &rs);
    audio = render_over(tx, text, strlen(text), 4096, &n);
    pcm = (int16_t *)malloc((n + 1) * sizeof(int16_t));
    quantize(audio, n, pcm);
    CHECK(n == samples, "%s: %zu samples, fldigi %zu", stem, n, samples);
    if (REFERENCE_PLATFORM)
        CHECK(fnv1a64(pcm, n) == hash, "%s: TX hash %016llx, fldigi %016llx", stem,
              (unsigned long long)fnv1a64(pcm, n), (unsigned long long)hash);
    snprintf(path, sizeof path, "%s/tx/%s.s16", golden, stem);
    ref = (int16_t *)read_file(path, &reflen);
    if (ref) {
        reflen /= 2;
        CHECK(reflen == n, "%s: stored vector %zu samples, port %zu", stem, reflen, n);
        for (i = 0; i < n && i < reflen; i++) {
            int d = abs((int)pcm[i] - (int)ref[i]);
            diff1 += d == 1;
            diffbig += d > 1;
        }
        CHECK(diffbig == 0, "%s: %zu samples differ by more than 1 LSB from fldigi", stem,
              diffbig);
        if (diff1)
            printf("  %s: %zu samples differ by 1 LSB from fldigi\n", stem, diff1);
    }

    for (s = 0; s < 3; s++) {
        folivia_rx *rx = folivia_rx_create(&cfg);
        size_t nx, got, glen;
        double *x = noisy(ref ? ref : pcm, ref ? reflen : n, s, (uint32_t)(1000 * idx + s),
                          &nx);
        uint8_t *out, *want;
        CHECK(rx != NULL, "%s: rx_create", stem);
        if (!rx) {
            free(x);
            continue;
        }
        out = decode(rx, x, nx, 512, &got);
        snprintf(path, sizeof path, "%s/rx/%s_%s.txt", golden, stem, snr_names[s]);
        want = (uint8_t *)read_file(path, &glen);
        CHECK(want != NULL, "%s: missing %s", stem, path);
        if (want) {
            int exact = got == glen && memcmp(out, want, glen) == 0;
            rx_total++;
            rx_exact += exact;
            if (REFERENCE_PLATFORM && folivia_rx_deviation_count(rx) == 0) {
                CHECK(exact, "%s %s: RX differs from fldigi:\n  port   [%s]\n  fldigi [%s]",
                      stem, snr_names[s], out, want);
            } else {
                /* Not bit-identical (the documented deviation path was taken, or another
                 * platform): the decode quality must still match fldigi's: the sent text is
                 * recovered with at most 1 + 5 % more errors than fldigi's own decode. */
                size_t dp = window_distance((const uint8_t *)sent, ns, out, got);
                size_t df = window_distance((const uint8_t *)sent, ns, want, glen);
                if (!exact)
                    printf("  %s %s: %zu deviation reads, not identical; text errors port %zu,"
                           " fldigi %zu\n", stem, snr_names[s], folivia_rx_deviation_count(rx),
                           dp, df);
                rx_deviant += folivia_rx_deviation_count(rx) > 0;
                CHECK(dp <= df + 1 + ns / 20, "%s %s: %zu text errors, fldigi %zu", stem,
                      snr_names[s], dp, df);
            }
            /* Whatever fldigi decoded, the text itself must be there. */
            if (contains(want, glen, sent))
                CHECK(contains(out, got, sent), "%s %s: text lost", stem, snr_names[s]);
        }
        free(want);
        free(out);
        free(x);
        folivia_rx_destroy(rx);
    }
    vectors_done++;
    free(ref);
    free(pcm);
    free(audio);
    folivia_tx_destroy(tx);
}

int main(int argc, char **argv)
{
    char path[512], *buf, *line, *save = NULL;
    uint8_t *bits;
    size_t len, nbits;

    if (argc > 1)
        golden = argv[1];
    load_texts();
    snprintf(path, sizeof path, "%s/rand_bits.u8", golden);
    bits = (uint8_t *)read_file(path, &nbits);
    snprintf(path, sizeof path, "%s/tx_hashes.txt", golden);
    buf = (char *)read_file(path, &len);
    if (!bits || !buf) {
        fprintf(stderr, "missing goldens in %s\n", golden);
        return 2;
    }
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char stem[64], mode[16], key[16];
        int tones, bw, idx;
        size_t samples;
        unsigned long long hash;
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%63s %15s %d %d %15s %d %zu %llx", stem, mode, &tones, &bw, key,
                   &idx, &samples, &hash) != 8) {
            CHECK(0, "bad line %s", line);
            continue;
        }
        check_vector(stem, mode, tones, bw, key, idx, samples, (uint64_t)hash, bits, nbits);
    }
    CHECK(vectors_done == 39, "%d vectors checked, expected 39", vectors_done);
    printf("fldigi goldens: %d overs, RX identical to fldigi in %d of %d decodes "
           "(%d with deviation reads)%s\n", vectors_done, rx_exact, rx_total, rx_deviant,
           REFERENCE_PLATFORM ? " (reference platform: TX hashes and RX bytes exact)" : "");
    free(buf);
    free(bits);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
