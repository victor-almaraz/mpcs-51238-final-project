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


def field(rate=22050, seconds=6.0):
    """A field recording that never was: wind, rain, and a few birds."""
    rng = random.Random(1)
    n = int(rate * seconds)
    out = [0.0] * n
    # Wind: noise through a slow one-pole low-pass, its level swelling.
    low = 0.0
    for t in range(n):
        low += 0.02 * (rng.uniform(-1, 1) - low)
        swell = 0.6 + 0.4 * math.sin(2 * math.pi * 0.23 * t / rate)
        out[t] += 3.0 * low * swell
    # Rain: short bright drops, decaying in a few milliseconds.
    for _ in range(int(seconds * 40)):
        start = rng.randrange(n)
        level = rng.uniform(0.05, 0.3)
        hp = 0.0
        prev = 0.0
        for k in range(int(0.006 * rate)):
            if start + k >= n:
                break
            noise = rng.uniform(-1, 1)
            hp = 0.7 * (hp + noise - prev)
            prev = noise
            out[start + k] += level * hp * math.exp(-k / (0.0015 * rate))
    # Birds: falling chirps around 3 kHz.
    for _ in range(5):
        start = rng.randrange(int(n * 0.9))
        f0 = rng.uniform(2500, 4000)
        phase = 0.0
        for k in range(int(0.12 * rate)):
            f = f0 * (1.0 - 0.4 * k / (0.12 * rate))
            phase += 2 * math.pi * f / rate
            out[start + k] += 0.15 * math.sin(phase) * math.sin(math.pi * k / (0.12 * rate))
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
    write_wav("spectral-audioscape/samples/field.wav", field(), 22050)
    write_wav("sampler-keys/samples/c4.wav", piano(), 22050)
    write_wav("grain-cloud/samples/glass.wav", glass(), 22050)
    # The piece loads the tables with :resample #f, so their rate does not
    # matter; 48 kHz is the server's default.
    write_wav("wavetable/samples/tables.wav", tables(), 48000)
