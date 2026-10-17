# fldigi-olivia-core

[![CI](https://github.com/wave-owl/fldigi-olivia-core/actions/workflows/ci.yml/badge.svg)](https://github.com/wave-owl/fldigi-olivia-core/actions/workflows/ci.yml)

A C99 library that receives and transmits **Olivia** and **Contestia** exactly the way
fldigi 4.2.13 does: audio in, characters out, and characters in, audio out. It is a
line-by-line port of Pawel Jalocha's MFSK engine as fldigi uses it
(`src/include/jalocha/pj_mfsk.h`) together with the modem logic of `src/olivia/olivia.cxx`
and `src/contestia/contestia.cxx`. One library serves both modes; the mode is a
configuration flag. On the reference build the transmitter reproduces fldigi's TX audio
sample for sample and the receiver prints fldigi's characters byte for byte (see
[Verification](#verification)). It is the only Olivia/Contestia implementation of
[wave-owl](https://github.com/wave-owl/wave-owl).

| | |
|---|---|
| **Is** | A single-channel Olivia/Contestia **receiver** for every submode fldigi offers (2 to 256 tones, 125 to 2000 Hz; the 18 + 19 fixed submodes of `src/globals/globals.cxx`): 8 kHz audio in, decoded characters out (with fldigi's 8-bit unescape and control-character filter), plus the synchroniser's S/N and frequency offset. |
| **Is** | The matching **transmitter** (capability tag `tx=1`): keyboard characters in, 8 kHz audio out with fldigi's start and stop tones, per-batch normalisation and character framing. |
| **Is not** | A skimmer or submode detector: each instance decodes one submode at one audio centre (with fldigi's ±8 tone-spacing tuning search). No RSID, no ARQ/KISS framing, no waterfall S/N display. |
| | An I/O layer, threaded or allocating at run time: no sound card or file access, no threads, all memory allocated in `*_create()`, no global or static mutable state. |

## Contents

- [Signal chain: receiver](#signal-chain-receiver)
- [Signal chain: transmitter](#signal-chain-transmitter)
- [Submodes](#submodes)
- [Differences from fldigi](#differences-from-fldigi)
- [Configuration reference](#configuration-reference)
- [API reference](#api-reference)
- [Usage examples](#usage-examples)
- [Contract and versioning](#contract-and-versioning)
- [Verification](#verification)
- [Performance and memory](#performance-and-memory)
- [Building](#building)
- [Maintenance and fldigi updates](#maintenance-and-fldigi-updates)
- [Roadmap](#roadmap)
- [License and credits](#license-and-credits)

## Signal chain: receiver

All citations refer to fldigi tag `v4.2.13` (mirror `wave-owl/fldigi`, commit
`e7c3ab3709ad62a7d91829364eb48a05fa325c0d`). The C code is in `src/olivia_rx.c`; every
function names the fldigi lines it ports. The C++ templates run with `Type = double`
(`MFSK_Receiver<double>`, `olivia.cxx:518`).

```
 audio x[n] (double, 8000 Hz)
      |  folivia_rx_process(): batches of at most 2046 samples
      v
 RateConverter (pj_mfsk.h:1435-1614): 16-tap Blackman-Harris windowed sinc at 3/8 of the
 rate, OutputRate = 1 (fldigi runs the converter even at equal rates)
      |  InputBuffer (pj_mfsk.h:2131)
      v
 MFSK_InputProcessor (pj_mfsk.h:292-598), window W = 16 x SymbolLen, 50 % overlap:
   two real windows in one FFT -> per spectrum: limit narrow peaks to 4 x the local mean
   (3 passes, W/64 bins), average the energy (W/96 then W/64 bins), divide each bin by
   the square root of its local energy (noise whitening) -> inverse FFT, overlap-add,
   clip at 2.5 x RMS (twice)                         (ProcessSpectra, pj_mfsk.h:467-485)
      |  whitened audio, W samples per window
      v
 MFSK_Demodulator (pj_mfsk.h:704-1046): every SymbolSepar = SymbolLen/2 samples one FFT of
   two half-symbol-offset slices, window 1 - cos(2 pi t / SymbolLen); tone energies of
   Tones x 2 bins plus +/- SyncMargin bins
      |  SoftDecode (pj_mfsk.h:955-982): squared energies -> soft bits, Gray decoded
      v
 2 slices x (2 SyncMargin + 1) frequency offsets of MFSK_SoftDecoder (pj_mfsk.h:1219-1430):
   de-interleave (rotate per bit), de-scramble (Olivia 0xE257E6D0291574EC shift 13,
   Contestia 0xEDB88320 shift 5), fast Hadamard transform of 64 (Olivia) or 32 (Contestia)
   points, peak = character, signal |peak|, noise energy of the rest
      |
      v
 MFSK_Receiver::ProcessSymbol (pj_mfsk.h:2275-2363): LowPass3 integrators (SyncIntegLen
   blocks) of signal and noise per block phase and offset; best phase and offset win;
   half a block later the integrated S/N is compared with SyncThreshold and the block
   (delayed by SyncIntegLen) is released
      |  FIFO of 1024 characters (pj_fifo.h)
      v
 olivia::rx_process (olivia.cxx:276-281): 8-bit unescape (127 + c -> c + 128,
   olivia.cxx:142-158), characters <= 7 dropped
      v
 characters (uint8_t)
```

### Geometry

`SymbolLen = 2^(BitsPerSymbol + 7 - log2(Bandwidth / 125))` samples (`pj_mfsk.h:2060`), the
tone spacing is `2 x 8000 / SymbolLen = Bandwidth / Tones` and the symbol rate
`8000 / SymbolSepar` equals the tone spacing. The first carrier sits at FFT bin
`(size_t)((SymbolLen / 16) x FirstCarrierMultiplier) + 1` (`pj_mfsk.h:2062-2063`) with the
multiplier a `float` computed by the modem wrapper:

| Mode | `FirstCarrierMultiplier` (RX) | fldigi |
|---|---|---|
| Olivia | `(centre - (int)(Bandwidth (1 - 0.5 / Tones) / 2)) / 500` | `olivia.cxx:256-269` (the offset is an `int` there) |
| Contestia | `(centre - Bandwidth / 2) / 500` (integer division of the bandwidth) | `contestia.cxx:219-229` |

For OLIVIA 8/500 at 1500 Hz this gives bin 41 of 256: tones 1281.25 .. 1718.75 Hz in
62.5 Hz steps. Reverse (carriers high to low) adds instead of subtracting.

### Squelch

fldigi sets `Rx->SyncThreshold` on every `rx_process` call (`olivia.cxx:271-272`,
`contestia.cxx:232-233`): squelch off gives **0 for Olivia** (every block is printed, also
noise) and **3 for Contestia**; squelch on gives `level / 5 + 3`, clamped to 0..90 (Olivia)
or 3..90 (Contestia). `sync_threshold = 0` selects fldigi's "squelch off"; any other value is
the threshold itself, clamped like fldigi. fldigi's GUI default (squelch on, level 5) is 4.0.

## Signal chain: transmitter

C code in `src/olivia_tx.c`.

```
 keyboard characters (folivia_tx_put_char)      get_tx_char(): NODATA -> 0 (idle), ETX at stop
      |  olivia::tx_process (olivia.cxx:160-229) / contestia::tx_process (contestia.cxx:143-203)
      |    first call: start tones, one NUL into the transmitter (olivia.cxx:176-181)
      |    one character per call while fewer than BitsPerSymbol are queued
      |    Olivia > 127: 127 + (c & 127) with olivia8bit, else '.'; Contestia: '.' above
      |    255/127, then 127 + (c & 127)
      v
 MFSK_Transmitter::Output (pj_mfsk.h:1813-1847), one symbol per call:
   every SymbolsPerBlock symbols: BitsPerSymbol characters -> MFSK_Encoder::EncodeBlock
   (pj_mfsk.h:1179-1207): Contestia character map (pj_mfsk.h:1137-1153), one signed
   Hadamard row per character (IFHT), scramble, diagonal interleave into the tone bits
      |
 MFSK_Modulator::Send (pj_mfsk.h:167-188): Gray code, carrier FirstCarrier + 2 x symbol,
   phase continued over the symbol overlap, +/- SymbolLen/4 phase jump from rand(),
   symbol shape 1 - cos(2 pi t / SymbolLen) added into the overlap buffer (AddSymbol,
   pj_mfsk.h:223-236), SymbolSepar samples out
      |
 RateConverter (OutputRate 1) -> each batch divided by its own peak (pj_mfsk.h:1839-1844)
      v
 stop: the current block is completed, then stop tones (olivia.cxx:216-221); Olivia adds
 512 samples of silence
      v
 audio in [-1, 1], 8000 Hz (fldigi's TX level, -12 dB by default, is left to the caller)
```

**Start and stop tones** (`olivia::send_tones`, `olivia.cxx:105-132`): 8192 samples, two
alternating 2048-sample bursts on `centre ± tone_bw / 2` with raised-cosine edges of 256
samples (`olivia.cxx:522-524`). `tone_bw` is fldigi's modem bandwidth
`Bandwidth - Bandwidth / Tones` in integer arithmetic (`olivia.cxx:379`,
`modem.cxx:361-365`), so the tones sit on the outermost carriers (1281 / 1719 Hz for 8/500).

**Over length** (text queued before the first render): `2 x 8192` tone samples,
`1 + ceil((characters - 1) / BitsPerSymbol)` FEC blocks of `SymbolsPerBlock x SymbolSepar`
samples (the first block carries fldigi's leading NUL and one character, with 2 tones only the
NUL, `olivia.cxx:176-206`), the last half symbol, and for Olivia 512 samples of silence.
OLIVIA 8/500 with 22 characters: 16384 + 8 x 8192 + 128 + 512 = 82560 samples, as fldigi.

## Submodes

All fixed submodes of fldigi 4.2.13 (`src/globals/globals.cxx:59-81, 168-189`); any other
power-of-two tone count 2 .. 256 with 125 .. 2000 Hz works the same way (fldigi's "custom"
Olivia/Contestia).

| Tones / bandwidth | Olivia | Contestia | symbol rate (Bd) | Olivia chars/s | Contestia chars/s |
|---|:-:|:-:|---:|---:|---:|
| 4/125 | x | x | 31.25 | 0.98 | 1.95 |
| 4/250 | x | x | 62.5 | 1.95 | 3.91 |
| 4/500 | x | x | 125 | 3.91 | 7.81 |
| 4/1000 | x | x | 250 | 7.81 | 15.6 |
| 4/2000 | x | x | 500 | 15.6 | 31.3 |
| 8/125 | x | x | 15.625 | 0.73 | 1.46 |
| 8/250 | x | x | 31.25 | 1.46 | 2.93 |
| 8/500 | x | x | 62.5 | 2.93 | 5.86 |
| 8/1000 | x | x | 125 | 5.86 | 11.7 |
| 8/2000 | x | x | 250 | 11.7 | 23.4 |
| 16/250 | | x | 15.625 | | 1.95 |
| 16/500 | x | x | 31.25 | 1.95 | 3.91 |
| 16/1000 | x | x | 62.5 | 3.91 | 7.81 |
| 16/2000 | x | x | 125 | 7.81 | 15.6 |
| 32/1000 | x | x | 31.25 | 2.44 | 4.88 |
| 32/2000 | x | x | 62.5 | 4.88 | 9.77 |
| 64/500 | x | x | 7.8125 | 0.73 | 1.46 |
| 64/1000 | x | x | 15.625 | 1.46 | 2.93 |
| 64/2000 | x | x | 31.25 | 2.93 | 5.86 |

Olivia sends 7-bit characters in 64-symbol blocks; Contestia 6-bit characters (upper case,
digits, punctuation 33..90, space, CR, backspace; everything else becomes `?`) in 32-symbol
blocks, so it is twice as fast and somewhat less robust.

## Differences from fldigi

The port follows fldigi statement by statement. Where it does not, it is listed here.

1. **Phase-jump generator.** `MFSK_Modulator::Send` flips the symbol phase by ±SymbolLen/4
   depending on libc `rand() & 1` (`pj_mfsk.h:185`), a process-global, unseeded generator.
   The port uses a per-instance xorshift32 seeded from `folivia_config.seed`. The jumps
   only spread the spectrum; receivers ignore them. `folivia_tx_set_rand()` plugs in any
   other source: with fldigi's sequence the output is fldigi's (see
   [Verification](#verification)).
2. **One read past an array.** `MFSK_Receiver::ProcessSymbol` takes the noise estimate from
   `NoiseEnergyPtr` after the offset loop, i.e. offset 0 of the *next* block phase
   (`pj_mfsk.h:2337`). For the last block phase that is one element past the end of
   `SyncNoiseEnergy` (undefined behaviour). The port reads element 0 instead (the next
   phase cyclically) and counts these events (`folivia_rx_deviation_count()`). It happens
   only when the best block phase is exactly half a block away from the last phase; then
   fldigi's decision depends on whatever memory follows the array.
3. **No reallocation.** fldigi reallocates `BoxFilter` taps on every spectrum
   (`pj_mfsk.h:264-277`) and would lose buffered audio if one `rx_process` call brought
   more than 2046 new samples (`Seq::EnsureSpace` does not copy, `pj_struc.h:145-155`).
   The port allocates everything once and feeds long calls in 2046-sample batches; the
   result is identical to fldigi's 512-sample feeding.
4. **Silent batch.** fldigi divides each TX batch by its peak even when the peak is 0
   (0/0); the port keeps such a batch silent. fldigi's own sequence never produces one.
5. **Not ported:** the waterfall-based S/N display (`olivia.cxx:289-310`), RSID, video ID,
   the GUI, ARQ/KISS I/O. `folivia_rx_snr_db()` reports the synchroniser's S/N in dB.
6. **Reset.** `folivia_rx_reset()` also clears the demodulator's input taps, which fldigi's
   `Reset()` keeps, so that a reset receiver equals a new one.

## Configuration reference

```c
typedef struct folivia_config {
    double sample_rate_hz;  /* 8000 */
    int tones;              /* 2, 4, 8, ..., 256 */
    double bandwidth_hz;    /* 125, 250, 500, 1000, 2000 */
    double center_freq_hz;  /* audio centre (fldigi "frequency"), 1500 by default */
    int contestia;          /* 0 Olivia, 1 Contestia */
    int olivia8bit;         /* fldigi olivia8bit / contestia8bit, default 1 */
    int reverse;            /* carriers high to low */
    double sync_threshold;  /* 0 = squelch off (Olivia 0, Contestia 3), else the threshold */
    int sync_margin;        /* tune margin in tone spacings, 0 = 8 */
    int sync_integ_len;     /* integration in FEC blocks, 0 = 4 */
    int start_tones;        /* TX start/stop tones, default 1 */
    uint32_t seed;          /* TX phase-jump seed */
} folivia_config;
```

| Field | Checked | wave-owl | Notes |
|---|---|---|---|
| `sample_rate_hz` | exactly 8000 | 8000 | fldigi's Olivia modem rate (`olivia.cxx:324`). |
| `tones` | power of two, 2..256 | per submode | fldigi's tone menu (`configuration.h` OLIVIATONES). |
| `bandwidth_hz` | 125, 250, 500, 1000, 2000 | per submode | `125 x 2^bw` (`olivia.cxx:325`). |
| `center_freq_hz` | `0 < f < 4000`, and every FFT bin of the band plus the tune margin inside the spectrum | 1500 | fldigi's sweet spot. |
| `contestia` | – | per mode | Character size, scrambler, block length, map. |
| `olivia8bit` | – | 1 | Both modes' 8-bit escape flag (`olivia8bit`, `contestia8bit`; default true). |
| `reverse` | – | 0 | fldigi's "reverse" (LSB); see the geometry above. |
| `sync_threshold` | `>= 0` | 0 (off, as the fldigi bench) | See [Squelch](#squelch). |
| `sync_margin` | 0..64 | 0 (= 8) | `OLIVIASMARGIN`, at least 2 (`pj_mfsk.h:2047`). |
| `sync_integ_len` | 0..64 | 0 (= 4) | `OLIVIASINTEG`, at least 2 (`pj_mfsk.h:2049`). |
| `start_tones` | – | 1 | `OLIVIASTARTTONES` (default true). |
| `seed` | – | per over | Any value; 0 uses a fixed default. |

`*_create()` returns NULL for a NULL config, any failed check, or out of memory.

## API reference

All declarations are in `include/fldigi_olivia.h`.

| Function | Description |
|---|---|
| `const char *folivia_version(void)` | `"fldigi-olivia-core 0.1.0 contract=olivia-c1 abi=1 tx=1 dsp=0.1.0"`. Static. |
| `folivia_rx *folivia_rx_create(const folivia_config *cfg)` | New receiver; the config is copied. All memory is allocated here (about 1 MiB, see below). |
| `void folivia_rx_destroy(folivia_rx *rx)` | Free; NULL is allowed. |
| `void folivia_rx_reset(folivia_rx *rx)` | Back to the state after create. |
| `size_t folivia_rx_process(folivia_rx *rx, const double *x, size_t n, uint8_t *chars, size_t max)` | Feed `n` samples (any chunking, the output does not depend on it). Returns the number of characters decoded; at most `max` are written (the rest are counted and dropped). A call releases at most `2 x BitsPerSymbol` characters per symbol period, so `(n / symbol_separ + 33) x 16` is always enough. |
| `size_t folivia_rx_flush(folivia_rx *rx, uint8_t *chars, size_t max)` | `olivia::rx_flush` (`olivia.cxx:95-103`): push the pipeline out with silence (end of a recording). fldigi passes these characters to the screen unfiltered, and so does the port. |
| `double folivia_rx_frequency_offset_hz(const folivia_rx *rx)` | Tuning error of the best sync candidate (`pj_mfsk.h:2216-2219`). |
| `double folivia_rx_sync_snr(const folivia_rx *rx)` | `SignalToNoiseRatio()`: integrated FEC signal / noise amplitude of the last released block (0 before; NaN is possible, as in fldigi). |
| `double folivia_rx_snr_db(const folivia_rx *rx)` | `20 log10(sync_snr)`, -99 when not positive. Not a channel S/N. |
| `double folivia_rx_metric(const folivia_rx *rx)` | fldigi's signal metric `clamp(5 (snr - 3), 0, 100)` (`olivia.cxx:302`). |
| `void folivia_rx_info(const folivia_rx *rx, folivia_info *info)` | Derived parameters: tones, bandwidth, bits per symbol, block length, symbol length, first carrier, tone spacing, baud, block period, characters per second, lowest/highest tone, tune margin, effective threshold. |
| `size_t folivia_rx_deviation_count(const folivia_rx *rx)` | How often difference 2 above occurred since create/reset. |
| `folivia_tx *folivia_tx_create(const folivia_config *cfg)` | New transmitter, ready for an over (fldigi `tx_init`). |
| `void folivia_tx_destroy(folivia_tx *tx)` / `folivia_tx_reset(folivia_tx *tx)` | Free / start a fresh over. |
| `int folivia_tx_put_char(folivia_tx *tx, uint8_t c)` | Queue a keyboard character (4096 max); 0 when full or stopping. |
| `void folivia_tx_stop(folivia_tx *tx)` | End the over after the queued text (fldigi ETX). Without a stop an empty queue sends NUL idles, like fldigi with an empty TX buffer. |
| `size_t folivia_tx_render(folivia_tx *tx, double *out, size_t max)` | Up to `max` samples; 0 when the over is complete. Any chunking gives the same samples. |
| `int folivia_tx_idle(const folivia_tx *tx)` | 1 when complete and fully rendered. |
| `size_t folivia_tx_chars_taken(const folivia_tx *tx)` | Keyboard characters consumed (fldigi's echo point). |
| `size_t folivia_tx_samples(const folivia_tx *tx)` | Samples rendered in this over. |
| `void folivia_tx_info(const folivia_tx *tx, folivia_info *info)` | As for the receiver. |
| `void folivia_tx_set_rand(folivia_tx *tx, int (*fn)(void *), void *ctx)` | Verification hook: phase-jump bits from `fn(ctx) & 1`. |

### Ownership and threads

Instances are independent: no global or static mutable state (the only file-scope objects
are `static const` tables), so different threads may run different instances. One instance
must not be used from two threads at once. The library keeps no pointer to caller buffers.

## Usage examples

### C: decode a raw recording

```c
/* olivia_rx.c: raw s16le mono 8 kHz audio on stdin -> decoded OLIVIA 8/500 text. */
#include <stdint.h>
#include <stdio.h>
#include "fldigi_olivia.h"

int main(void)
{
    folivia_config cfg = {8000, 8, 500, 1500, 0, 1, 0, 0, 0, 0, 1, 1};
    folivia_rx *rx = folivia_rx_create(&cfg);
    unsigned char raw[2 * 4096];
    double x[4096];
    uint8_t out[8192];
    size_t nbytes, n, i, got;
    if (!rx)
        return 1;
    while ((nbytes = fread(raw, 1, sizeof raw, stdin)) >= 2) {
        n = nbytes / 2;
        for (i = 0; i < n; i++)
            x[i] = (int16_t)(uint16_t)(raw[2 * i] | (raw[2 * i + 1] << 8)) / 32768.0;
        got = folivia_rx_process(rx, x, n, out, sizeof out);
        fwrite(out, 1, got < sizeof out ? got : sizeof out, stdout);
    }
    got = folivia_rx_flush(rx, out, sizeof out);
    fwrite(out, 1, got < sizeof out ? got : sizeof out, stdout);
    fprintf(stderr, "\nsync S/N %.1f, offset %+.1f Hz\n", folivia_rx_sync_snr(rx),
            folivia_rx_frequency_offset_hz(rx));
    folivia_rx_destroy(rx);
    return 0;
}
```

### C: transmit

```c
folivia_config cfg = {8000, 8, 250, 1500, 1, 1, 0, 0, 0, 0, 1, 42};   /* Contestia 8/250 */
folivia_tx *tx = folivia_tx_create(&cfg);
const char *text = "CQ CQ DE DG6FL K";
double buf[4096];
size_t n;
while (*text)
    folivia_tx_put_char(tx, (uint8_t)*text++);
folivia_tx_stop(tx);
while ((n = folivia_tx_render(tx, buf, 4096)) > 0)
    play(buf, n);   /* 8 kHz audio in [-1, 1] */
folivia_tx_destroy(tx);
```

### Python (ctypes)

wave-owl's binding is `src/olivia_native.py`; the essentials:

```python
import ctypes, numpy as np

class Config(ctypes.Structure):
    _fields_ = [("sample_rate_hz", ctypes.c_double), ("tones", ctypes.c_int),
                ("bandwidth_hz", ctypes.c_double), ("center_freq_hz", ctypes.c_double),
                ("contestia", ctypes.c_int), ("olivia8bit", ctypes.c_int),
                ("reverse", ctypes.c_int), ("sync_threshold", ctypes.c_double),
                ("sync_margin", ctypes.c_int), ("sync_integ_len", ctypes.c_int),
                ("start_tones", ctypes.c_int), ("seed", ctypes.c_uint32)]

lib = ctypes.CDLL("build/libfldigi_olivia.dylib")   # .so on Linux
lib.folivia_version.restype = ctypes.c_char_p
lib.folivia_rx_create.restype = ctypes.c_void_p
lib.folivia_rx_process.restype = ctypes.c_size_t
lib.folivia_rx_process.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_double),
                                   ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
cfg = Config(8000, 32, 1000, 1500, 0, 1, 0, 0, 0, 0, 1, 1)
rx = ctypes.c_void_p(lib.folivia_rx_create(ctypes.byref(cfg)))
audio = np.zeros(8000, dtype=np.float64)            # your 8 kHz audio
out = ctypes.create_string_buffer(4096)
n = lib.folivia_rx_process(rx, audio.ctypes.data_as(ctypes.POINTER(ctypes.c_double)),
                           audio.size, out, 4096)
print(out.raw[:n].decode("latin-1"))
```

## Contract and versioning

`folivia_version()` reports `contract=olivia-c1 abi=1` and the capability tag `tx=1`, plus
the `fldigi-dsp-core` version it was built with. A loader (wave-owl `src/olivia_native.py`)
refuses a library whose contract or ABI differs. The contract changes with any
incompatible change of the API or of the meaning of a configuration field; the ABI integer
with any change of the structure layouts. Additions raise the minor version.

The shared DSP primitives come from [`wave-owl/fldigi-dsp-core`](https://github.com/wave-owl/fldigi-dsp-core)
as the submodule `dsp/`. `dsp.lock` pins its version, commit and the content checksum of the
linked files; `tools/check_dsp.sh` runs before anything is compiled and stops the build on a
mismatch (wave-owl R-920.3).

## Verification

Three layers, all in CI (Ubuntu and macOS, plus ASan/UBSan):

**1. fldigi itself** (`tests/test_olivia_fldigi.c`, goldens from
`tests/golden/make_fldigi_golden.py`, which drives the headless fldigi 4.2.13 bench build
of wave-owl's archived change `fix-psk-rtty-vs-fldigi`, `bench/BUILD.md`):

- **TX**, all 37 fixed submodes plus an Olivia and a Contestia pangram (39 overs): fldigi
  renders `CQ CQ DE DG6FL DG6FL K`; the port renders the same text with fldigi's phase-jump
  bits (`rand_bits.u8`: `rand() & 1` after `srand(1)`, as a fresh fldigi process draws them)
  and the result is quantised like fldigi's sound path (× 10^(-12/20), `lrint(x · 32767)`).
  On the reference build the FNV-1a hash of every vector is **identical** to fldigi's; 12
  vectors are also stored (`tests/golden/tx/*.s16`) and match within 1 LSB on every
  platform (0 LSB on the reference build).
- **RX**, the same 39 overs clean and with deterministic noise at -5 and -10 dB SNR in
  2500 Hz (117 decodes, the noise is rebuilt bit for bit in C): the port's output bytes,
  including fldigi's garbage before and after the burst, are **identical to fldigi's
  output in 112 of 117 decodes**. The 5 others are exactly the decodes in which difference 2
  (the out-of-array read) occurred; there the sent text comes out with the same number of
  errors as in fldigi (one case: 22 vs 20 edits in a 2000 Hz Contestia burst at -10 dB that
  neither decodes). On other platforms (other libm, no FMA) the test requires the same
  decode quality instead of identical bytes; on Ubuntu x86-64 (GitHub CI) the stored TX
  vectors still match with 0 LSB, 87 of 117 decodes are byte-identical and all others have
  the same text errors as fldigi's (the differences are in the garbage around the bursts).

  The reference build is macOS on arm64 with clang's default floating-point contraction
  (`-ffp-contract=on`) and `-O2`/`-O3`, i.e. how fldigi itself was built for the bench; the
  library is compiled with `-ffp-contract=on` for this reason.
- `tests/golden/make_fldigi_golden.py` regenerates everything (about 2 minutes).

**2. Unit tests** (`tests/test_olivia.c`): API and configuration checks, derived
parameters, TX → RX loopback for all 37 fixed submodes, reverse, 8-bit escape and `.`
substitution, the Contestia character map, chunk invariance of RX (1 .. 1 Mi samples) and
TX, reset, two independent instances of both modes interleaved, noise only, queue limits.

**3. Shared primitives**: `dsp/` has its own golden tests against fldigi's headers.

The decode-quality curves against fldigi's (CER 0 per SNR, 10 trials per dB) are measured
in wave-owl (`scripts/measure_fldigi_thresholds.py`,
`src/modes/conformance/targets/fldigi-v4.2.13.json`).

## Performance and memory

`make bench` (Apple M4, `-O3`, 60 s of audio, 512-sample blocks):

| Submode | RX, % of real time | TX, % of real time |
|---|---:|---:|
| Olivia 4/125 | 0.10 | 0.02 |
| Olivia 8/500 | 0.16 | 0.01 |
| Olivia 32/1000 | 0.15 | 0.01 |
| Olivia 64/2000 | 0.18 | 0.01 |
| Contestia 4/125 | 0.06 | 0.01 |
| Contestia 8/250 | 0.07 | 0.01 |
| Contestia 64/2000 | 0.12 | 0.01 |

Memory per instance: receiver 0.7 .. 1.2 MiB (dominated by the 16-symbol input window and
its FFT tables), transmitter 0.25 MiB (mostly the start/stop tone and output staging
buffers). Nothing is allocated after `*_create()`.

## Building

```
git clone --recurse-submodules https://github.com/wave-owl/fldigi-olivia-core
make            # dsp/ pin check, build/libfldigi_olivia.a and .dylib/.so
make test       # unit tests and the fldigi goldens
make sanitize   # the same under ASan + UBSan
make bench      # CPU benchmark
make BUILD=/abs/dir shared   # out-of-tree build (wave-owl: make build-olivia-core)
```

Plain make, C99, `-Wall -Wextra -Werror -pedantic`, no dependencies besides libm. Without
`dsp/` the build stops with a hint to `git submodule update --init --recursive`.

## Maintenance and fldigi updates

- Every block cites the fldigi 4.2.13 lines it ports. wave-owl's
  `scripts/fldigi_upstream_check.py` (skill `fldigi-upstream-sync`) diffs those lines against a
  new fldigi tag and lists what to review.
- After a review, regenerate the goldens with the new bench build
  (`tests/golden/make_fldigi_golden.py`); TX hashes and RX bytes tell at once whether the port
  still equals fldigi.
- A new `fldigi-dsp-core` is adopted by moving `dsp/` and updating `dsp.lock`; the goldens
  must stay green.

## Roadmap

- RSID for Olivia/Contestia (fldigi has it; out of scope for 0.1).
- Blind submode detection is not planned (fldigi has none either; its "auto" entries are GUI
  presets).

## License and credits

GPL-3.0-or-later, as fldigi. The MFSK engine is by Pawel Jalocha SP9VRC (2004); the fldigi
modems are by Dave Freese W1HKJ and the fldigi developers, adapted from gMFSK by Tomi
Manninen OH2BNS. C port, goldens and tests: Gerolf Ziegenhain DG6FL, 2026. See
`CHANGELOG.md`.
