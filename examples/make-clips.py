#!/usr/bin/env python3
"""Writes the starter clips of the autoband example.

Every loop here is written by this script and dedicated to the public
domain (CC0 1.0). Run it from the examples/ folder; it rewrites
autoband/clips/ byte for byte. Each clip is a Standard MIDI File of format
0, 96 ticks a quarter note, four bars of 4/4 in A minor.
"""

import os
import struct

DIVISION = 96
BARS = 4
BEATS = 4 * BARS


def quantity(v):
    """A MIDI variable-length quantity."""
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.insert(0, 0x80 | (v & 0x7F))
        v >>= 7
    return bytes(out)


def midi_file(notes, channel=0):
    """Notes as (start, length, note, velocity), in beats, as a format 0
    file four bars long."""
    events = []
    for start, length, note, velocity in notes:
        on = round(start * DIVISION)
        off = round((start + length) * DIVISION)
        events.append((on, 1, bytes([0x90 | channel, note, velocity])))
        events.append((off, 0, bytes([0x80 | channel, note, 0])))
    events.sort(key=lambda e: (e[0], e[1]))  # note-offs before note-ons at one tick
    track = b""
    last = 0
    for tick, _, data in events:
        track += quantity(tick - last) + data
        last = tick
    track += quantity(BEATS * DIVISION - last) + b"\xff\x2f\x00"
    header = struct.pack(">IHHH", 6, 0, 1, DIVISION)
    return b"MThd" + header + b"MTrk" + struct.pack(">I", len(track)) + track


# The chords, a bar each: Am, F, C, G. Their roots in the bass's octave.
ROOTS = [33, 29, 36, 31]                    # A1 F1 C2 G1
CHORDS = [[57, 60, 64], [53, 57, 60], [55, 60, 64], [55, 59, 62]]


def bass_clips():
    roots = []                              # eighths on the root
    walk = []                               # root, fifth, octave, approach
    octaves = []                            # octave-jumping eighths
    push = []                               # syncopated: on 1, the and of 2, and 4
    for bar, root in enumerate(ROOTS):
        t = 4 * bar
        nxt = ROOTS[(bar + 1) % 4]
        for i in range(8):
            roots.append((t + i / 2, 0.4, root, 100 if i % 2 == 0 else 80))
            octaves.append((t + i / 2, 0.4, root + (12 if i % 2 else 0), 96))
        for i, n in enumerate([root, root + 7, root + 12, nxt + (1 if nxt < root else -1)]):
            walk.append((t + i, 0.9, n, 100))
        for start, length in [(0, 1.25), (1.5, 0.5), (2.5, 1.0), (3.5, 0.5)]:
            push.append((t + start, length, root, 104))
    return {"1 eighths": roots, "2 walking": walk, "3 octaves": octaves, "4 push": push}


def pad_clips():
    held = []                               # each chord held for its bar
    pulses = []                             # each chord on the beats
    for bar, chord in enumerate(CHORDS):
        for n in chord:
            held.append((4 * bar, 3.9, n, 70))
            for beat in range(4):
                pulses.append((4 * bar + beat, 0.45, n, 64))
    return {"1 held": held, "2 pulses": pulses}


def lead_clips():
    motif = [(0, 76), (0.5, 72), (1, 69), (2, 71), (3, 72), (3.5, 71),
             (4, 69), (5, 65), (6, 67), (7, 64),
             (8, 67), (8.5, 69), (9, 72), (10, 76), (11, 74),
             (12, 71), (13, 67), (14, 71), (15, 74)]
    arpeggio = []                           # the chord's notes, up an octave, in sixteenths
    for bar, chord in enumerate(CHORDS):
        tones = [n + 12 for n in chord] + [chord[0] + 24]
        for i in range(16):
            arpeggio.append((4 * bar + i / 4, 0.2, tones[i % 4], 84 if i % 4 == 0 else 66))
    return {"1 motif": [(t, 0.45, n, 96) for t, n in motif], "2 arpeggio": arpeggio}


KICK, SNARE, CLAP, CLOSED, OPEN = 36, 38, 39, 42, 46


def drum_clips():
    four = []                               # four on the floor, eighth hats, an open hat on each and of 4
    backbeat = []                           # kick on 1 and the and of 2, snare on 2 and 4
    broken = []                             # a broken beat with claps
    for bar in range(BARS):
        t = 4 * bar
        for beat in range(4):
            four.append((t + beat, 0.25, KICK, 110))
        for i in range(8):
            hat = OPEN if i == 7 else CLOSED
            four.append((t + i / 2, 0.2, hat, 80 if i % 2 else 64))
            backbeat.append((t + i / 2, 0.2, CLOSED, 76 if i % 2 == 0 else 60))
        for beat in (1, 3):
            four.append((t + beat, 0.25, CLAP, 96))
            backbeat.append((t + beat, 0.25, SNARE, 108))
        for start in (0, 2.5):
            backbeat.append((t + start, 0.25, KICK, 112))
        for start in (0, 0.75, 2.5, 3.25):
            broken.append((t + start, 0.25, KICK, 108))
        for start in (1, 3):
            broken.append((t + start, 0.25, SNARE, 104))
        broken.append((t + 3.75, 0.2, CLAP, 70))
        for i in range(16):
            if i % 4 != 2:
                broken.append((t + i / 4, 0.15, CLOSED, 70 if i % 2 == 0 else 52))
    return {"1 four on the floor": four, "2 backbeat": backbeat, "3 broken": broken}


def main():
    banks = {"bass": bass_clips(), "pad": pad_clips(), "lead": lead_clips(), "drums": drum_clips()}
    for bank, clips in banks.items():
        folder = os.path.join("autoband", "clips", bank)
        os.makedirs(folder, exist_ok=True)
        for name, notes in clips.items():
            with open(os.path.join(folder, name + ".mid"), "wb") as f:
                f.write(midi_file(sorted(notes), channel=9 if bank == "drums" else 0))


if __name__ == "__main__":
    main()
