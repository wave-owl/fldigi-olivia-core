/*
 * fldigi-olivia-core unit tests: API contract, parameter checks, derived parameters,
 * TX -> RX loopback for every fixed submode of both modes, reverse, 8-bit escape, the
 * Contestia character set, chunk invariance of RX and TX, reset and independent instances.
 * Comparisons with fldigi 4.2.13 itself are in test_olivia_fldigi.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test_util.h"

/* Every fixed submode of fldigi 4.2.13, src/globals/globals.cxx:59-81 and 168-189. */
static const int olivia_modes[][2] = {
    {4, 125}, {4, 250}, {4, 500}, {4, 1000}, {4, 2000}, {8, 125}, {8, 250}, {8, 500},
    {8, 1000}, {8, 2000}, {16, 500}, {16, 1000}, {16, 2000}, {32, 1000}, {32, 2000},
    {64, 500}, {64, 1000}, {64, 2000}};
static const int contestia_modes[][2] = {
    {4, 125}, {4, 250}, {4, 500}, {4, 1000}, {4, 2000}, {8, 125}, {8, 250}, {8, 500},
    {8, 1000}, {8, 2000}, {16, 250}, {16, 500}, {16, 1000}, {16, 2000}, {32, 1000},
    {32, 2000}, {64, 500}, {64, 1000}, {64, 2000}};

/* Decode a whole over (process, then flush); printable characters only. */
static uint8_t *loopback_decode(const folivia_config *cfg, const double *audio, size_t n,
                                size_t chunk, size_t *got)
{
    folivia_rx *rx = folivia_rx_create(cfg);
    size_t total = n + 2 * 8000, i, k = 0, m, f;
    double *x = (double *)calloc(total, sizeof(double));
    uint8_t *out, *flushed, *clean;
    memcpy(x + 8000, audio, n * sizeof(double));
    out = decode(rx, x, total, chunk, &m);
    flushed = (uint8_t *)malloc(4096);
    f = folivia_rx_flush(rx, flushed, 4096);
    clean = (uint8_t *)malloc(m + f + 1);
    for (i = 0; i < m + f; i++) {
        uint8_t c = i < m ? out[i] : flushed[i - m];
        if (c >= 32 || c == '\r' || c == '\n')
            clean[k++] = c;
    }
    clean[k] = 0;
    *got = k;
    free(x);
    free(out);
    free(flushed);
    folivia_rx_destroy(rx);
    return clean;
}

static void test_version(void)
{
    const char *v = folivia_version();
    CHECK(strstr(v, "fldigi-olivia-core ") == v, "version prefix: %s", v);
    CHECK(strstr(v, " contract=olivia-c1 ") != NULL, "contract tag: %s", v);
    CHECK(strstr(v, " abi=1 ") != NULL, "abi tag: %s", v);
    CHECK(strstr(v, " tx=1") != NULL, "tx tag: %s", v);
    CHECK(strstr(v, " dsp=") != NULL, "dsp tag: %s", v);
}

static void test_config_checks(void)
{
    folivia_config c = make_config(8, 500, 0), bad;
    folivia_rx *rx;
    folivia_tx *tx;
    CHECK(folivia_rx_create(NULL) == NULL, "NULL config");
    CHECK(folivia_tx_create(NULL) == NULL, "NULL config (tx)");
#define REJECT(field, value)                                                             \
    do {                                                                                 \
        bad = c;                                                                         \
        bad.field = value;                                                               \
        rx = folivia_rx_create(&bad);                                                    \
        tx = folivia_tx_create(&bad);                                                    \
        CHECK(rx == NULL && tx == NULL, "accepted " #field " = " #value);               \
        folivia_rx_destroy(rx);                                                          \
        folivia_tx_destroy(tx);                                                          \
    } while (0)
    REJECT(sample_rate_hz, 48000);
    REJECT(sample_rate_hz, 7999);
    REJECT(tones, 1);
    REJECT(tones, 6);
    REJECT(tones, 512);
    REJECT(bandwidth_hz, 100);
    REJECT(bandwidth_hz, 750);
    REJECT(bandwidth_hz, 4000);
    REJECT(center_freq_hz, 0);
    REJECT(center_freq_hz, 4000);
    REJECT(center_freq_hz, 200);   /* the 500 Hz band would start below 0 Hz */
    REJECT(center_freq_hz, 3900);  /* above the spectrum */
    REJECT(sync_threshold, -1);
    REJECT(sync_margin, -1);
    REJECT(sync_margin, 65);
    REJECT(sync_integ_len, 65);
#undef REJECT
    /* fldigi's full tone menu: 2 .. 256 tones */
    bad = make_config(2, 125, 0);
    rx = folivia_rx_create(&bad);
    CHECK(rx != NULL, "2 tones refused");
    folivia_rx_destroy(rx);
    bad = make_config(256, 2000, 0);
    rx = folivia_rx_create(&bad);
    CHECK(rx != NULL, "256 tones refused");
    folivia_rx_destroy(rx);
    folivia_rx_destroy(NULL);
    folivia_tx_destroy(NULL);
    folivia_rx_reset(NULL);
    folivia_tx_reset(NULL);
    CHECK(folivia_rx_process(NULL, NULL, 0, NULL, 0) == 0, "NULL process");
    CHECK(folivia_tx_render(NULL, NULL, 10) == 0, "NULL render");
}

static void test_info(void)
{
    /* MFSK_Receiver parameters for OLIVIA 8/500 and 32/1000 (pj_mfsk.h:2060-2063, 2168) */
    folivia_config c = make_config(8, 500, 0);
    folivia_rx *rx = folivia_rx_create(&c);
    folivia_tx *tx = folivia_tx_create(&c);
    folivia_info ri, ti;
    folivia_rx_info(rx, &ri);
    folivia_tx_info(tx, &ti);
    CHECK(ri.symbol_len == 256 && ri.symbol_separ == 128, "8/500 symbol %zu/%zu",
          ri.symbol_len, ri.symbol_separ);
    CHECK(ri.baud == 62.5 && ri.tone_spacing_hz == 62.5, "8/500 baud %g spacing %g", ri.baud,
          ri.tone_spacing_hz);
    CHECK(ri.symbols_per_block == 64 && ri.bits_per_symbol == 3, "8/500 block");
    CHECK(fabs(ri.block_period_s - 1.024) < 1e-12, "8/500 block period %g",
          ri.block_period_s);
    /* (1500 - 234) / 500 = 2.532 -> (size_t)(16 * 2.532) + 1 = 41 -> 1281.25 Hz */
    CHECK(ri.first_carrier == 41 && ri.low_hz == 1281.25 && ri.high_hz == 1718.75,
          "8/500 RX carriers %zu %g %g", ri.first_carrier, ri.low_hz, ri.high_hz);
    CHECK(ti.first_carrier == 41 && ti.low_hz == 1281.25, "8/500 TX carriers %zu %g",
          ti.first_carrier, ti.low_hz);
    CHECK(ri.tune_margin_hz == 8 * 31.25, "tune margin %g", ri.tune_margin_hz);
    CHECK(ri.sync_threshold == 0.0, "Olivia squelch off -> threshold 0, got %g",
          ri.sync_threshold);
    folivia_rx_destroy(rx);
    folivia_tx_destroy(tx);

    c = make_config(32, 1000, 0);
    rx = folivia_rx_create(&c);
    folivia_rx_info(rx, &ri);
    CHECK(ri.symbol_len == 512 && ri.baud == 31.25 && ri.bits_per_symbol == 5,
          "32/1000 %zu %g", ri.symbol_len, ri.baud);
    CHECK(fabs(ri.chars_per_second - 5 / 2.048) < 1e-12, "32/1000 cps %g",
          ri.chars_per_second);
    folivia_rx_destroy(rx);

    c = make_config(8, 250, 1);
    rx = folivia_rx_create(&c);
    folivia_rx_info(rx, &ri);
    CHECK(ri.symbols_per_block == 32, "Contestia block %d", ri.symbols_per_block);
    CHECK(ri.sync_threshold == 3.0, "Contestia squelch off -> threshold 3, got %g",
          ri.sync_threshold);
    folivia_rx_destroy(rx);
    c.sync_threshold = 1.0;  /* clamped to Contestia's 3..90 */
    rx = folivia_rx_create(&c);
    folivia_rx_info(rx, &ri);
    CHECK(ri.sync_threshold == 3.0, "Contestia threshold clamp %g", ri.sync_threshold);
    folivia_rx_destroy(rx);
    c.sync_threshold = 200.0;
    rx = folivia_rx_create(&c);
    folivia_rx_info(rx, &ri);
    CHECK(ri.sync_threshold == 90.0, "threshold clamp 90 %g", ri.sync_threshold);
    folivia_rx_destroy(rx);
}

/* Every fixed submode decodes its own transmission (wave-owl AC-813.1). */
static void test_loopback_all(void)
{
    static const char text[] = "CQ CQ DE DG6FL DG6FL K";
    size_t m;
    for (m = 0; m < 2; m++) {
        size_t count = m ? sizeof contestia_modes / sizeof contestia_modes[0]
                         : sizeof olivia_modes / sizeof olivia_modes[0];
        size_t i;
        for (i = 0; i < count; i++) {
            const int *tb = m ? contestia_modes[i] : olivia_modes[i];
            folivia_config c = make_config(tb[0], tb[1], (int)m);
            folivia_tx *tx = folivia_tx_create(&c);
            size_t n, got;
            double *audio;
            uint8_t *out;
            CHECK(tx != NULL, "%s %d/%d tx_create", m ? "Contestia" : "Olivia", tb[0], tb[1]);
            if (!tx)
                continue;
            audio = render_over(tx, text, sizeof text - 1, 1000, &n);
            CHECK(folivia_tx_idle(tx), "not idle after the over");
            CHECK(folivia_tx_chars_taken(tx) == sizeof text - 1, "taken %zu",
                  folivia_tx_chars_taken(tx));
            CHECK(folivia_tx_samples(tx) == n, "samples %zu vs %zu", folivia_tx_samples(tx),
                  n);
            out = loopback_decode(&c, audio, n, 512, &got);
            CHECK(contains(out, got, text), "%s %d/%d loopback: [%s]",
                  m ? "Contestia" : "Olivia", tb[0], tb[1], out);
            free(out);
            free(audio);
            folivia_tx_destroy(tx);
        }
    }
}

/* fldigi normalises every symbol batch to peak 1; the tones ramp in from 0. */
static void test_tx_levels(void)
{
    folivia_config c = make_config(16, 500, 0);
    folivia_tx *tx = folivia_tx_create(&c), *tx2;
    size_t n, n2, i;
    double *audio = render_over(tx, "TEST", 4, 777, &n), *a2, peak = 0;
    for (i = 0; i < n; i++)
        if (fabs(audio[i]) > peak)
            peak = fabs(audio[i]);
    CHECK(peak <= 1.0 && peak > 0.99, "peak %g", peak);
    CHECK(audio[0] == 0.0, "tone ramp starts at 0: %g", audio[0]);
    c.start_tones = 0;
    tx2 = folivia_tx_create(&c);
    a2 = render_over(tx2, "TEST", 4, 777, &n2);
    /* start and stop tones are 2 x 8192 samples (the 512 silent samples after the stop
     * tones are sent either way, olivia.cxx:216-221) */
    CHECK(n == n2 + 2 * 8192, "start tones add %zu samples", n - n2);
    free(audio);
    free(a2);
    folivia_tx_destroy(tx);
    folivia_tx_destroy(tx2);
}

static void test_tx_chunking(void)
{
    static const char text[] = "chunk test 0123";
    folivia_config c = make_config(32, 1000, 0);
    size_t chunks[] = {1, 7, 128, 4096, 100000}, i, n0 = 0, n;
    double *ref = NULL;
    for (i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        folivia_tx *tx = folivia_tx_create(&c);
        double *a = render_over(tx, text, sizeof text - 1, chunks[i], &n);
        if (!ref) {
            ref = a;
            n0 = n;
        } else {
            CHECK(n == n0 && memcmp(a, ref, n * sizeof(double)) == 0, "TX chunk %zu differs",
                  chunks[i]);
            free(a);
        }
        folivia_tx_destroy(tx);
    }
    free(ref);
}

static void test_rx_chunking(void)
{
    static const char text[] = "RX CHUNK INVARIANCE";
    folivia_config c = make_config(8, 250, 1);
    folivia_tx *tx = folivia_tx_create(&c);
    size_t n, i, k, chunks[] = {1, 3, 511, 2046, 2047, 5000, 1 << 20};
    double *audio = render_over(tx, text, sizeof text - 1, 512, &n);
    size_t total = n + 8000 + 80000;
    double *x = (double *)calloc(total, sizeof(double));
    uint8_t *ref = NULL;
    size_t nref = 0;
    double snr0 = 0, fo0 = 0;
    /* a little deterministic noise so that every decision depends on every sample */
    uint32_t s = 7;
    for (i = 0; i < total; i++) {
        s = s * 1664525u + 1013904223u;
        x[i] = (i >= 8000 && i < 8000 + n ? audio[i - 8000] * 0.25 : 0.0) +
               ((double)(int32_t)s) / 2147483648.0 * 0.05;
    }
    for (k = 0; k < sizeof chunks / sizeof chunks[0]; k++) {
        folivia_rx *rx = folivia_rx_create(&c);
        size_t got;
        uint8_t *out = decode(rx, x, total, chunks[k], &got);
        if (!ref) {
            ref = out;
            nref = got;
            snr0 = folivia_rx_sync_snr(rx);
            fo0 = folivia_rx_frequency_offset_hz(rx);
            CHECK(contains(out, got, text), "chunk reference lost the text [%s]", out);
        } else {
            CHECK(got == nref && memcmp(out, ref, got) == 0, "RX chunk %zu differs", chunks[k]);
            CHECK(folivia_rx_sync_snr(rx) == snr0 && folivia_rx_frequency_offset_hz(rx) == fo0,
                  "RX chunk %zu state differs", chunks[k]);
            free(out);
        }
        folivia_rx_destroy(rx);
    }
    free(ref);
    free(x);
    free(audio);
    folivia_tx_destroy(tx);
}

static void test_reverse(void)
{
    static const char text[] = "REVERSE MODE TEST";
    int m;
    for (m = 0; m < 2; m++) {
        folivia_config c = make_config(16, 500, m);
        folivia_tx *tx;
        size_t n, got;
        double *audio;
        uint8_t *out;
        folivia_info ti;
        c.reverse = 1;
        tx = folivia_tx_create(&c);
        folivia_tx_info(tx, &ti);
        CHECK(ti.high_hz > 1500 && ti.low_hz < 1500, "reverse TX band %g..%g", ti.low_hz,
              ti.high_hz);
        audio = render_over(tx, text, sizeof text - 1, 512, &n);
        out = loopback_decode(&c, audio, n, 512, &got);
        CHECK(contains(out, got, text), "%s reverse loopback [%s]", m ? "Contestia" : "Olivia",
              out);
        free(out);
        free(audio);
        folivia_tx_destroy(tx);
    }
}

static void test_escape_and_charset(void)
{
    /* Olivia 8-bit: bytes > 127 travel as 127 + (c & 127) (olivia.cxx:195-203) and come
     * back unescaped (olivia.cxx:142-158). */
    static const char latin1[] = "GR\xDC\xDF" "E AUS M\xDC" "NCHEN";
    static const char ascii[] = "GR.." "E AUS M." "NCHEN";
    folivia_config c = make_config(8, 500, 0);
    folivia_tx *tx = folivia_tx_create(&c);
    size_t n, got;
    double *audio = render_over(tx, latin1, sizeof latin1 - 1, 512, &n);
    uint8_t *out = loopback_decode(&c, audio, n, 512, &got);
    CHECK(contains(out, got, latin1), "8-bit escape loopback [%s]", out);
    free(out);
    free(audio);
    folivia_tx_destroy(tx);

    /* without olivia8bit the bytes become '.' */
    c.olivia8bit = 0;
    tx = folivia_tx_create(&c);
    audio = render_over(tx, latin1, sizeof latin1 - 1, 512, &n);
    out = loopback_decode(&c, audio, n, 512, &got);
    CHECK(contains(out, got, ascii), "7-bit '.' substitution [%s]", out);
    free(out);
    free(audio);
    folivia_tx_destroy(tx);

    /* Contestia: upper case, space, CR, digits and punctuation 33..90; other characters
     * become '?' (pj_mfsk.h:1137-1153, 1369-1378). */
    {
        static const char in[] = "cq de dg6fl {x} ~ 73!";
        static const char want[] = "CQ DE DG6FL ?X? ? 73!";
        folivia_config cc = make_config(8, 250, 1);
        tx = folivia_tx_create(&cc);
        audio = render_over(tx, in, sizeof in - 1, 512, &n);
        out = loopback_decode(&cc, audio, n, 512, &got);
        CHECK(contains(out, got, want), "Contestia charset [%s]", out);
        free(out);
        free(audio);
        folivia_tx_destroy(tx);
    }
}

/* One engine, two modes, independent instances (wave-owl AC-810.2). */
static void test_independent_instances(void)
{
    static const char t1[] = "FIRST OLIVIA SIGNAL", t2[] = "SECOND CONTESTIA SIGNAL";
    folivia_config co = make_config(8, 500, 0), cc = make_config(8, 500, 1);
    folivia_tx *a = folivia_tx_create(&co), *b = folivia_tx_create(&cc);
    size_t na, nb, ga, gb, i, step = 333;
    double *xa = render_over(a, t1, sizeof t1 - 1, 512, &na);
    double *xb = render_over(b, t2, sizeof t2 - 1, 512, &nb);
    folivia_rx *ra = folivia_rx_create(&co), *rb = folivia_rx_create(&cc);
    uint8_t oa[4096], ob[4096];
    size_t n = na > nb ? na : nb, total = n + 16000;
    double *pa = (double *)calloc(total, sizeof(double));
    double *pb = (double *)calloc(total, sizeof(double));
    memcpy(pa, xa, na * sizeof(double));
    memcpy(pb, xb, nb * sizeof(double));
    ga = gb = 0;
    for (i = 0; i < total; i += step) {  /* interleaved calls on the two instances */
        size_t k = total - i < step ? total - i : step;
        ga += folivia_rx_process(ra, pa + i, k, oa + ga, sizeof oa - ga);
        gb += folivia_rx_process(rb, pb + i, k, ob + gb, sizeof ob - gb);
    }
    ga += folivia_rx_flush(ra, oa + ga, sizeof oa - ga);
    gb += folivia_rx_flush(rb, ob + gb, sizeof ob - gb);
    CHECK(contains(oa, ga, t1), "Olivia instance lost its text");
    CHECK(contains(ob, gb, t2), "Contestia instance lost its text");
    CHECK(!contains(oa, ga, t2) && !contains(ob, gb, t1), "instances leak into each other");
    free(pa);
    free(pb);
    free(xa);
    free(xb);
    folivia_rx_destroy(ra);
    folivia_rx_destroy(rb);
    folivia_tx_destroy(a);
    folivia_tx_destroy(b);
}

static void test_reset(void)
{
    static const char text[] = "RESET";
    folivia_config c = make_config(16, 1000, 0);
    folivia_tx *tx = folivia_tx_create(&c);
    size_t n1, n2, g1, g2;
    double *a1 = render_over(tx, text, 5, 512, &n1), *a2;
    folivia_rx *rx = folivia_rx_create(&c);
    uint8_t *o1, *o2;
    folivia_tx_reset(tx);
    a2 = render_over(tx, text, 5, 512, &n2);
    CHECK(n1 == n2 && memcmp(a1, a2, n1 * sizeof(double)) == 0, "TX reset is not a fresh over");
    o1 = decode(rx, a1, n1, 512, &g1);
    folivia_rx_reset(rx);
    o2 = decode(rx, a1, n1, 512, &g2);
    CHECK(g1 == g2 && memcmp(o1, o2, g1) == 0, "RX reset is not a fresh receiver");
    free(o1);
    free(o2);
    free(a1);
    free(a2);
    folivia_rx_destroy(rx);
    folivia_tx_destroy(tx);
}

static void test_tx_queue(void)
{
    folivia_config c = make_config(4, 2000, 1);
    folivia_tx *tx = folivia_tx_create(&c);
    double buf[64];
    size_t i, ok = 0;
    for (i = 0; i < 5000; i++)
        ok += (size_t)folivia_tx_put_char(tx, 'A');
    CHECK(ok == 4096, "keyboard queue holds %zu", ok);
    /* without a stop the transmitter keeps sending (NUL idles after the text) */
    CHECK(folivia_tx_render(tx, buf, 64) == 64, "render");
    CHECK(!folivia_tx_idle(tx), "idle while running");
    folivia_tx_stop(tx);
    CHECK(folivia_tx_put_char(tx, 'B') == 0, "put after stop accepted");
    folivia_tx_destroy(tx);
}

static void test_noise_only(void)
{
    /* No crash, no hang, a finite S/N on noise only (wave-owl AC-61.2); Contestia's
     * threshold 3 keeps noise mostly silent. */
    folivia_config c = make_config(32, 1000, 1);
    folivia_rx *rx = folivia_rx_create(&c);
    size_t n = 8000 * 30, i, got;
    double *x = (double *)malloc(n * sizeof(double));
    uint8_t *out;
    uint32_t s = 99;
    for (i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        x[i] = ((double)(int32_t)s) / 2147483648.0;
    }
    out = decode(rx, x, n, 1000, &got);
    CHECK(got < 200, "noise produced %zu characters", got);
    CHECK(folivia_rx_snr_db(rx) < 40, "noise S/N %g dB", folivia_rx_snr_db(rx));
    free(out);
    free(x);
    folivia_rx_destroy(rx);
}

int main(void)
{
    test_version();
    test_config_checks();
    test_info();
    test_loopback_all();
    test_tx_levels();
    test_tx_chunking();
    test_rx_chunking();
    test_reverse();
    test_escape_and_charset();
    test_independent_instances();
    test_reset();
    test_tx_queue();
    test_noise_only();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("fldigi-olivia-core: all unit tests passed (%s)\n", folivia_version());
    return 0;
}
