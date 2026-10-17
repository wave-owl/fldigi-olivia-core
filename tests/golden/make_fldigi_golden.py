#!/usr/bin/env python3
"""Golden data from fldigi 4.2.13 itself for fldigi-olivia-core (stdlib only).

Drives the headless fldigi bench build (configure --enable-benchmark plus the TX
extension of wave-owl's archived change fix-psk-rtty-vs-fldigi, bench/BUILD.md) for every
fixed Olivia and Contestia submode of fldigi 4.2.13 (src/globals/globals.cxx):

* TX: fldigi renders TEXT (bench-tx, txlevel -12 dB, PCM16, 8000 Hz). The FNV-1a 64 hash
  of every vector goes to tx_hashes.txt; a representative subset is kept as raw
  little-endian int16 in tx/<name>.s16.
* RX: fldigi decodes 1 s silence + the TX vector + 12 s silence, clean and buried in
  deterministic noise at -5 and -10 dB (2500 Hz reference bandwidth, see noisy()). The raw
  bytes fldigi hands to put_rx_char() go to rx/<name>_<snr>.txt.
* rand_bits.u8: rand() & 1 for the first 65536 calls after srand(1) on the machine that
  made the goldens (fldigi's MFSK modulator draws its phase jumps from libc rand(),
  pj_mfsk.h:185; a fresh fldigi process starts at seed 1).

    FLDIGI_BENCH_BIN=/path/to/src-bench/src/fldigi python3 tests/golden/make_fldigi_golden.py

The C tests (tests/test_olivia_fldigi.c) rebuild the same noisy inputs bit for bit, so only
the clean subset needs to be stored.
SPDX-License-Identifier: GPL-3.0-or-later
"""

from __future__ import annotations

import array
import math
import os
import shutil
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

HERE = Path(__file__).resolve().parent
FLDIGI = os.environ.get("FLDIGI_BENCH_BIN", "fldigi")
CARRIER_HZ = 1500
TEXT_CQ = "CQ CQ DE DG6FL DG6FL K"
TEXT_PANGRAM = "the quick brown fox jumps over the lazy dog 0123456789 <test?> 73"

# fldigi 4.2.13 trx_mode ids (index into mode_info[], src/globals/globals.cxx:51-189).
OLIVIA = [(4, 125), (4, 250), (4, 500), (4, 1000), (4, 2000), (8, 125), (8, 250), (8, 500),
          (8, 1000), (8, 2000), (16, 500), (16, 1000), (16, 2000), (32, 1000), (32, 2000),
          (64, 500), (64, 1000), (64, 2000)]
CONTESTIA = [(4, 125), (4, 250), (4, 500), (4, 1000), (4, 2000), (8, 125), (8, 250),
             (8, 500), (8, 1000), (8, 2000), (16, 250), (16, 500), (16, 1000), (16, 2000),
             (32, 1000), (32, 2000), (64, 500), (64, 1000), (64, 2000)]
STORED = {("olivia", 4, 250), ("olivia", 8, 500), ("olivia", 16, 500), ("olivia", 32, 1000),
          ("olivia", 64, 2000), ("contestia", 4, 125), ("contestia", 8, 250),
          ("contestia", 16, 500), ("contestia", 32, 1000), ("contestia", 64, 1000)}
SNRS = ("clean", "m05", "m10")


def bw_name(bw: int) -> str:
    return f"{bw // 1000}K" if bw >= 1000 else str(bw)


def submodes():
    """(file stem, fldigi id, fldigi sname, mode, tones, bw, text key)."""
    out = []
    for i, (t, b) in enumerate(OLIVIA):
        out.append((f"olivia_{t}_{b}", 92 + i, f"OLIVIA-{t}/{bw_name(b)}", "olivia", t, b,
                    "cq"))
    for i, (t, b) in enumerate(CONTESTIA):
        out.append((f"contestia_{t}_{b}", 3 + i, f"Cont-{t}/{bw_name(b)}", "contestia", t, b,
                    "cq"))
    out.append(("olivia_8_500_pangram", 99, "OLIVIA-8/500", "olivia", 8, 500, "pangram"))
    out.append(("contestia_8_250_pangram", 9, "Cont-8/250", "contestia", 8, 250, "pangram"))
    return out


def run_fldigi(mid: int, sname: str, args, env_extra=None) -> str:
    cfg = tempfile.mkdtemp(prefix="fldigi-olivia-golden-")
    Path(cfg, "fldigi_def.xml").write_text("<FLDIGI_DEFS>\n</FLDIGI_DEFS>\n")
    env = {k: v for k, v in os.environ.items() if not k.startswith("FLDIGI_BENCH_TX")}
    env.update(env_extra or {})
    try:
        p = subprocess.run([FLDIGI, "--config-dir", cfg, "--benchmark-modem", str(mid),
                            "--benchmark-frequency", str(CARRIER_HZ), *args],
                           env=env, capture_output=True, text=True, timeout=1800)
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
    if f"mode={sname} " not in p.stderr:
        raise RuntimeError(f"fldigi did not run {sname}: {p.stderr[-400:]}")
    return p.stderr


def read_s16(path: Path) -> array.array:
    with wave.open(str(path)) as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2 and w.getframerate() == 8000
        a = array.array("h", w.readframes(w.getnframes()))
    if sys.byteorder == "big":
        a.byteswap()
    return a


def write_wav(path: Path, pcm) -> None:
    a = array.array("h", pcm)
    if sys.byteorder == "big":
        a.byteswap()
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(8000)
        w.writeframes(a.tobytes())


def fnv1a64(pcm) -> int:
    h = 0xCBF29CE484222325
    for v in pcm:
        u = v & 0xFFFF
        for byte in (u & 0xFF, u >> 8):
            h ^= byte
            h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class Lcg:
    """32-bit LCG (Numerical Recipes); the same in tests/test_olivia_fldigi.c."""

    def __init__(self, seed: int):
        self.s = seed & 0xFFFFFFFF

    def uniform(self) -> float:
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.s / 4294967296.0


def noisy(pcm, snr: str, seed: int):
    """1 s + pcm + 12 s, plus Irwin-Hall Gaussian noise at -5 or -10 dB SNR in 2500 Hz.

    Only +, -, *, / and sqrt (all correctly rounded), so C reproduces it bit for bit.
    SNR = mean square of the burst / (noise variance * 2500 / 4000).
    """
    lead, tail = 8000, 12 * 8000
    padded = [0.0] * lead + [float(v) for v in pcm] + [0.0] * tail
    if snr == "clean":
        return [int(v) for v in padded]
    ps = 0.0
    for v in pcm:
        ps += float(v) * float(v)
    ps = ps / len(pcm)
    inv = 1.0 / math.sqrt(10.0) if snr == "m05" else 1.0 / 10.0  # 10^(-snr/10)
    s2 = ps / inv * 4000.0 / 2500.0
    sd = math.sqrt(s2)
    gain = 3276.7 / math.sqrt(ps * len(pcm) / len(padded) + s2)  # 0.1 RMS of full scale
    rng = Lcg(seed)
    out = []
    for v in padded:
        z = 0.0
        for _ in range(12):
            z += rng.uniform()
        z -= 6.0
        y = (v + sd * z) * gain
        q = math.floor(y + 0.5)
        out.append(max(-32768, min(32767, int(q))))
    return out


def rand_bits(n: int = 65536) -> bytes:
    """rand() & 1 after srand(1) from this machine's libc, as fldigi draws them."""
    import ctypes
    libc = ctypes.CDLL(None)
    libc.srand(1)
    return bytes(libc.rand() & 1 for _ in range(n))


def main() -> int:
    (HERE / "tx").mkdir(exist_ok=True)
    (HERE / "rx").mkdir(exist_ok=True)
    texts = {"cq": TEXT_CQ, "pangram": TEXT_PANGRAM}
    lines = []
    with tempfile.TemporaryDirectory() as td:
        for idx, (stem, mid, sname, mode, tones, bw, key) in enumerate(submodes()):
            wav = Path(td, stem + ".wav")
            run_fldigi(mid, sname, ["--benchmark-input", "1"],
                       {"FLDIGI_BENCH_TX_TEXT": texts[key], "FLDIGI_BENCH_TX_WAV": str(wav)})
            pcm = read_s16(wav)
            lines.append(f"{stem} {mode} {tones} {bw} {key} {idx} {len(pcm)} "
                         f"{fnv1a64(pcm):016x}")
            if (mode, tones, bw) in STORED or key == "pangram":
                raw = array.array("h", pcm)
                if sys.byteorder == "big":
                    raw.byteswap()
                Path(HERE, "tx", stem + ".s16").write_bytes(raw.tobytes())
            for i, snr in enumerate(SNRS):
                inp = Path(td, f"{stem}_{snr}.wav")
                write_wav(inp, noisy(pcm, snr, 1000 * idx + i))
                out = Path(td, "rx.txt")
                if out.exists():
                    out.unlink()
                run_fldigi(mid, sname, ["--benchmark-input", str(inp),
                                        "--benchmark-output", str(out)])
                text = out.read_bytes() if out.exists() else b""
                Path(HERE, "rx", f"{stem}_{snr}.txt").write_bytes(text)
            print(lines[-1], flush=True)
    header = ("# stem mode tones bandwidth text index samples fnv1a64(int16 LE) -- fldigi "
              "4.2.13 bench TX, txlevel -12 dB; noise seed = 1000 * index + snr step\n")
    Path(HERE, "tx_hashes.txt").write_text(header + "\n".join(lines) + "\n")
    Path(HERE, "rand_bits.u8").write_bytes(rand_bits())
    Path(HERE, "texts.txt").write_text(f"cq {TEXT_CQ}\npangram {TEXT_PANGRAM}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
