#!/usr/bin/env python3
"""Writes the sample files of the M5 examples.

Every sound here is synthesized by this script, from a fixed seed, and is
dedicated to the public domain (CC0 1.0). Run it from the examples/
folder; it rewrites each example's samples/ folder byte for byte.
"""

import math
import os
import random
import struct


def write_wav(path, samples, rate):
    """Mono 16-bit PCM."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = b"".join(struct.pack("<h", max(-32767, min(32767, round(s * 32767)))) for s in samples)
    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def normalize(x, peak):
    top = max(abs(v) for v in x) or 1.0
    return [v * peak / top for v in x]


def melody(rate=22050, seconds=6.0):
    """A short synth melody: nine notes in A minor pentatonic at 100 BPM,
    each two band-limited sawtooths 6 cents apart, bright at the attack and
    darker as the note sustains."""
    beat = 0.6
    # (start in beats, MIDI note, length in beats)
    notes = [(0, 69, 1), (1, 72, 0.5), (1.5, 74, 0.5), (2, 76, 1.5), (3.5, 74, 0.5),
             (4, 72, 1), (5, 67, 0.5), (5.5, 64, 0.5), (6, 69, 2.5)]
    n = int(rate * seconds)
    out = [0.0] * n
    release = 0.12
    for start, note, length in notes:
        f0 = 440.0 * 2 ** ((note - 69) / 12)
        first = int(start * beat * rate)
        held = length * beat
        for k in range(min(n - first, int((held + 5 * release) * rate))):
            time = k / rate
            # 5 ms attack, a decay to 0.7 over about 0.25 s, then an
            # exponential release after the note's end.
            level = min(1.0, time / 0.005) * (0.7 + 0.3 * math.exp(-time / 0.25))
            if time > held:
                level *= math.exp(-(time - held) / release)
            # The partials fall off faster as the note goes on, as a
            # closing low-pass would make them.
            width = 2.0 + 10.0 * math.exp(-time / 0.3)
            v = 0.0
            for cents in (-3.0, 3.0):
                f = f0 * 2 ** (cents / 1200)
                for h in range(1, int(rate / 2 / f) + 1):
                    weight = math.exp(-(h - 1) / width) / h
                    if weight < 1e-4:
                        break
                    v += weight * math.sin(2 * math.pi * f * h * time)
            out[first + k] += level * v
    return normalize(out, 0.5)


def piano(rate=22050, seconds=2.0, f0=261.63):
    """Middle C, struck: slightly stretched partials, the higher ones dying first."""
    n = int(rate * seconds)
    stretch = 0.0004
    out = []
    for t in range(n):
        time = t / rate
        v = 0.0
        for h in range(1, 13):
            f = f0 * h * math.sqrt(1 + stretch * h * h)
            if f > rate / 2:
                break
            v += math.sin(2 * math.pi * f * time) * math.exp(-time * (0.8 + 0.35 * h)) / h
        attack = min(1.0, time / 0.004)
        out.append(v * attack)
    return normalize(out, 0.7)


def glass(rate=22050, seconds=3.0):
    """Struck glass: inharmonic bell partials at a few pitches."""
    rng = random.Random(2)
    n = int(rate * seconds)
    out = [0.0] * n
    ratios = [1.0, 2.76, 5.40, 8.93]
    for start in range(0, n, int(0.4 * rate)):
        f0 = rng.choice([523.25, 587.33, 659.25, 783.99, 880.0])
        for i, r in enumerate(ratios):
            decay = 1.2 / (1 + i)
            for k in range(n - start):
                time = k / rate
                out[start + k] += math.sin(2 * math.pi * f0 * r * time) * math.exp(-time / decay) / (1 + i)
    return normalize(out, 0.6)


def tables(size=2048, count=8):
    """Eight tables from a sine to a bright saw to a hollow square."""
    out = []
    for j in range(count):
        m = j / (count - 1)
        partials = 1 + int(m * 63)
        table = [0.0] * size
        for h in range(1, partials + 1):
            # Even harmonics fade out over the second half of the morph.
            weight = (1.0 / h) * (1.0 if h % 2 == 1 else max(0.0, 1.0 - 2.0 * max(0.0, m - 0.5)))
            for k in range(size):
                table[k] += weight * math.sin(2 * math.pi * h * k / size)
        out.extend(normalize(table, 0.8))
    return out


if __name__ == "__main__":
    write_wav("spectral-audioscape/samples/melody.wav", melody(), 22050)
    write_wav("sampler-keys/samples/c4.wav", piano(), 22050)
    write_wav("grain-cloud/samples/glass.wav", glass(), 22050)
    # The piece loads the tables with :resample #f, so their rate does not
    # matter; 48 kHz is the server's default.
    write_wav("wavetable/samples/tables.wav", tables(), 48000)
