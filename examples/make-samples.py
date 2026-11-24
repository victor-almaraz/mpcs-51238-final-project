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


def arpeggio(rate=44100, seconds=6.0):
    """An arpeggio in the impressionist manner on an FM piano: an Fmaj7
    chord rising over two octaves, F3 to F5, and falling back to A3, so the
    loop returns to F3. Sixteen notes 0.3125 s apart, each left to ring as
    under the sustain pedal, louder towards the turn."""
    notes = [53, 57, 60, 64, 65, 69, 72, 76,   # F3 A3 C4 E4 F4 A4 C5 E5
             77, 76, 72, 69, 65, 64, 60, 57]   # F5 E5 C5 A4 F4 E4 C4 A3
    step = 0.3125
    n = int(rate * seconds)
    out = [0.0] * n
    for i, note in enumerate(notes):
        f = 440.0 * 2 ** ((note - 69) / 12)
        velocity = 0.6 + 0.3 * math.sin(math.pi * i / (len(notes) - 1))
        # Higher notes die sooner, as a piano's do.
        decay = 2.5 * math.sqrt(261.63 / f)
        first = int(i * step * rate)
        for k in range(n - first):
            time = k / rate
            attack = min(1.0, time / 0.002)
            # The body: a carrier and a modulator at the same frequency,
            # the index falling from bright to mellow.
            index = 0.3 + 1.7 * velocity * math.exp(-time / 0.6)
            body = math.sin(2 * math.pi * f * time + index * math.sin(2 * math.pi * f * time))
            # The tine: a modulator at 14 times the frequency, gone in a
            # few tens of milliseconds.
            tine = math.sin(2 * math.pi * f * time + 1.5 * math.exp(-time / 0.05) * math.sin(2 * math.pi * 14 * f * time))
            out[first + k] += velocity * attack * (math.exp(-time / decay) * body + 0.25 * math.exp(-time / 0.3) * tine)
    # A short fade at the end, so the loop's turn does not click.
    fade = int(0.25 * rate)
    for k in range(fade):
        out[n - fade + k] *= 1.0 - k / fade
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
    write_wav("spectral-audioscape/samples/arpeggio.wav", arpeggio(), 44100)
    write_wav("sampler-keys/samples/c4.wav", piano(), 22050)
    write_wav("grain-cloud/samples/glass.wav", glass(), 22050)
    # The piece loads the tables with :resample #f, so their rate does not
    # matter; 48 kHz is the server's default.
    write_wav("wavetable/samples/tables.wav", tables(), 48000)
