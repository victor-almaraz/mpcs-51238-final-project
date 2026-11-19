#!/usr/bin/env python3
"""Writes the audio fixtures for server_tests (samples.md §15.1).

Every fixture holds the same 8 stereo frames at 8000 Hz, each sample a
multiple of 1/8, which every integer format stores exactly, so a decoder
must return them exactly. Run from this directory; the files are
committed, and this script reproduces them byte for byte.
"""

import struct

LEFT = [0, 1, 2, 3, -1, -2, -3, -8]
RIGHT = [4, -4, 2, -2, 0, 7, -7, 1]
RATE = 8000
FRAMES = list(zip(LEFT, RIGHT))


def integer(k, bits):
    """Sample k/8 as a signed integer of the given width."""
    return k * (1 << (bits - 1)) // 8


def pcm(bits, big_endian, unsigned8=False):
    out = bytearray()
    for frame in FRAMES:
        for k in frame:
            v = integer(k, bits)
            if bits == 8:
                out += struct.pack("B" if unsigned8 else "b", v + 128 if unsigned8 else v)
            elif bits == 24:
                b = (v & 0xFFFFFF).to_bytes(3, "big" if big_endian else "little")
                out += b
            else:
                out += struct.pack((">" if big_endian else "<") + {16: "h", 32: "i"}[bits], v)
    return bytes(out)


def floats(bits, big_endian):
    fmt = (">" if big_endian else "<") + ("f" if bits == 32 else "d")
    return b"".join(struct.pack(fmt, k / 8) for frame in FRAMES for k in frame)


def chunk(name, data, big_endian):
    size = struct.pack(">I" if big_endian else "<I", len(data))
    return name + size + data + (b"\0" if len(data) % 2 else b"")


def wav(bits, is_float):
    data = floats(bits, False) if is_float else pcm(bits, False, unsigned8=True)
    block = 2 * bits // 8
    fmt = struct.pack("<HHIIHH", 3 if is_float else 1, 2, RATE, RATE * block, block, bits)
    body = b"WAVE" + chunk(b"fmt ", fmt, False) + chunk(b"data", data, False)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def extended(x):
    """An IEEE 754 80-bit extended float, as AIFF stores its sample rate."""
    exponent = 16383 + x.bit_length() - 1
    mantissa = x << (64 - x.bit_length())
    return struct.pack(">HQ", exponent, mantissa)


def aiff(bits, compression=None):
    frames = len(FRAMES)
    if compression is None:
        comm = struct.pack(">hIh", 2, frames, bits) + extended(RATE)
        data = pcm(bits, True)
        kind = b"AIFF"
        extra = b""
    else:
        name = b"not compressed" if compression == b"NONE" else b"IEEE float"
        comm = (struct.pack(">hIh", 2, frames, bits) + extended(RATE) + compression +
                bytes([len(name)]) + name + (b"\0" if (len(name) + 1) % 2 else b""))
        data = pcm(bits, True) if compression == b"NONE" else floats(bits, True)
        kind = b"AIFC"
        extra = chunk(b"FVER", struct.pack(">I", 0xA2805140), True)
    ssnd = struct.pack(">II", 0, 0) + data
    body = kind + extra + chunk(b"COMM", comm, True) + chunk(b"SSND", ssnd, True)
    return b"FORM" + struct.pack(">I", len(body)) + body


class bits_writer:
    def __init__(self):
        self.value = 0
        self.count = 0

    def put(self, value, width):
        self.value = (self.value << width) | (value & ((1 << width) - 1))
        self.count += width

    def bytes(self):
        assert self.count % 8 == 0
        return self.value.to_bytes(self.count // 8, "big")


def crc(data, width, poly):
    reg = 0
    top = 1 << (width - 1)
    mask = (1 << width) - 1
    for byte in data:
        reg ^= byte << (width - 8)
        for _ in range(8):
            reg = ((reg << 1) ^ poly) & mask if reg & top else (reg << 1) & mask
    return reg


def flac(bits):
    """One frame of verbatim (uncompressed) subframes, which every FLAC
    decoder reads."""
    frames = len(FRAMES)
    info = bits_writer()
    info.put(frames, 16)        # minimum block size
    info.put(frames, 16)        # maximum block size
    info.put(0, 24)             # minimum frame size: unknown
    info.put(0, 24)             # maximum frame size: unknown
    info.put(RATE, 20)
    info.put(2 - 1, 3)          # channels - 1
    info.put(bits - 1, 5)       # bits per sample - 1
    info.put(frames, 36)        # total samples
    streaminfo = info.bytes() + bytes(16)  # MD5 unknown
    header = bytes([0x80]) + len(streaminfo).to_bytes(3, "big")  # last block, STREAMINFO

    h = bits_writer()
    h.put(0b11111111111110, 14)  # sync
    h.put(0, 1)                  # reserved
    h.put(0, 1)                  # fixed block size
    h.put(0b0110, 4)             # block size: 8 bits after the header
    h.put(0b0000, 4)             # sample rate: from STREAMINFO
    h.put(0b0001, 4)             # two independent channels
    h.put(0b000, 3)              # sample size: from STREAMINFO
    h.put(0, 1)                  # reserved
    h.put(0, 8)                  # frame number 0, UTF-8 coded
    h.put(frames - 1, 8)         # block size - 1
    frame = h.bytes()
    frame += bytes([crc(frame, 8, 0x07)])
    s = bits_writer()
    for channel in (LEFT, RIGHT):
        s.put(0, 1)              # padding
        s.put(0b000001, 6)       # verbatim
        s.put(0, 1)              # no wasted bits
        for k in channel:
            s.put(integer(k, bits), bits)
    if s.count % 8:
        s.put(0, 8 - s.count % 8)
    frame += s.bytes()
    frame += crc(frame, 16, 0x8005).to_bytes(2, "big")
    return b"fLaC" + header + streaminfo + frame


FILES = {
    "u8.wav": wav(8, False),
    "s16.wav": wav(16, False),
    "s24.wav": wav(24, False),
    "s32.wav": wav(32, False),
    "f32.wav": wav(32, True),
    "f64.wav": wav(64, True),
    "s8.aiff": aiff(8),
    "s16.aiff": aiff(16),
    "s24.aiff": aiff(24),
    "s32.aiff": aiff(32),
    "f32.aifc": aiff(32, b"fl32"),
    "f64.aifc": aiff(64, b"fl64"),
    "s16.flac": flac(16),
    "s24.flac": flac(24),
}

if __name__ == "__main__":
    for name, data in FILES.items():
        with open(name, "wb") as f:
            f.write(data)
