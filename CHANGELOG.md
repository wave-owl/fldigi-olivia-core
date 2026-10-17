# Changelog

All notable changes to fldigi-olivia-core. Versions follow semantic versioning; the
contract tag `olivia-c1` and `abi=1` change only with incompatible changes.

## 0.1.0 - 2026-10-17

First release (wave-owl change `add-fldigi-olivia-core`, R-810).

- Receiver: C99 port of fldigi 4.2.13's MFSK engine (`pj_mfsk.h`: RateConverter,
  MFSK_InputProcessor, MFSK_Demodulator, MFSK_SoftDecoder, MFSK_Receiver) and the
  Olivia/Contestia `rx_process` logic (squelch, 8-bit unescape, character filter), every
  submode fldigi offers (2..256 tones, 125..2000 Hz, all 18 + 19 fixed submodes).
- Transmitter (`tx=1`): MFSK_Encoder, MFSK_Modulator, MFSK_Transmitter and fldigi's
  `tx_process`/`send_tones` (start/stop tones, 8-bit escape, Contestia map, per-batch
  normalisation); seeded phase-jump generator with a verification hook.
- Goldens from fldigi itself: 39 TX overs identical (hash) to fldigi's on the reference
  build, 112 of 117 RX decodes byte-identical to fldigi's output, the rest explained by the
  one documented deviation (pj_mfsk.h:2337 out-of-array read).
- Shared DSP from `wave-owl/fldigi-dsp-core` 0.1.0 as submodule `dsp/`, pinned by
  `dsp.lock` and checked before every build.
- CI on Ubuntu and macOS with ASan/UBSan; CPU benchmark.
