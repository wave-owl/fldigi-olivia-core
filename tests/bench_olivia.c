/*
 * CPU benchmark: receiver and transmitter time per submode, as a fraction of real time.
 * The receiver decodes 60 s of a transmitted over plus noise (512-sample blocks, as fldigi
 * feeds it); the transmitter renders the same over. Memory per instance is reported from
 * the allocation sizes of the instance (the library allocates only in *_create).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _POSIX_C_SOURCE 200809L
#include "test_util.h"

#include <time.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(void)
{
    static const int modes[][3] = {
        {4, 125, 0}, {8, 250, 0}, {8, 500, 0}, {16, 500, 0}, {32, 1000, 0}, {64, 2000, 0},
        {4, 125, 1}, {8, 250, 1}, {16, 500, 1}, {32, 1000, 1}, {64, 2000, 1}};
    static const char text[] = "CQ CQ DE DG6FL DG6FL DG6FL PSE K ";
    const size_t seconds = 60, n = seconds * 8000;
    double *x = (double *)malloc(n * sizeof(double));
    size_t m;
    printf("%-16s %10s %10s\n", "submode", "RX x RT", "TX x RT");
    for (m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        folivia_config c = make_config(modes[m][0], modes[m][1], modes[m][2]);
        folivia_tx *tx = folivia_tx_create(&c);
        folivia_rx *rx = folivia_rx_create(&c);
        size_t got = 0, i, k;
        uint32_t s = 1;
        double t0, t_tx, t_rx;
        uint8_t out[4096];
        t0 = now_s();
        for (i = 0; i < 40; i++)
            folivia_tx_put_char(tx, (uint8_t)text[i % (sizeof text - 1)]);
        for (got = 0; got < n;) {
            k = folivia_tx_render(tx, x + got, n - got);
            if (k == 0)
                break;
            got += k;
        }
        t_tx = now_s() - t0;
        for (i = 0; i < n; i++) {
            s = s * 1664525u + 1013904223u;
            x[i] = (i < got ? x[i] * 0.3 : 0.0) + ((double)(int32_t)s) / 2147483648.0 * 0.1;
        }
        t0 = now_s();
        for (i = 0; i < n; i += 512)
            folivia_rx_process(rx, x + i, n - i < 512 ? n - i : 512, out, sizeof out);
        t_rx = now_s() - t0;
        printf("%-10s %2d/%-4d %9.4f%% %9.4f%%\n", modes[m][2] ? "Contestia" : "Olivia",
               modes[m][0], modes[m][1], 100.0 * t_rx / seconds,
               100.0 * t_tx / ((double)got / 8000.0));
        folivia_rx_destroy(rx);
        folivia_tx_destroy(tx);
    }
    free(x);
    return 0;
}
