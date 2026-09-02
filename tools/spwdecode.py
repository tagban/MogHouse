"""Decode FFXI's sound effects - sound/win/se/**/*.spw - to WAV.

A .spw is a 48-byte "SeWave" header and then audio, in one of two encodings.
Both are already understood elsewhere: the ADPCM is the same PlayStation
four-coefficient scheme the music uses (see renderer/ffxi/bgw.h), and the rest
is plain 16-bit PCM.

    +0x00  "SeWave\0\0"
    +0x08  file size
    +0x0c  encoding: 0 ADPCM, 1 PCM, 3 something else - 940 files, not read here
    +0x10  the file's own number within its folder
    +0x14  blocks
    +0x18  the block to loop back to, or -1
    +0x1c  the sample rate, split: a value here and the rate minus it at +0x20
    +0x24  48, where the audio starts
    +0x2c  samples per block: 16 for ADPCM, 1 for PCM

Channels come out of the arithmetic rather than a field: the payload is
blocks * bytes, and for sixteen ADPCM samples that is nine bytes a channel.

The sample rate is the pair at +0x1c and +0x20 added together, wrapping. Every
sound effect in a retail install comes to 48000. The music does the same thing
at +0x20 and +0x24 and is not constant there - 82 tracks say 44100 and 29 say
48000 - which is what gives the field away as a rate rather than a checksum.
"""

import argparse
import struct
import sys
import wave
from pathlib import Path

FILTER0 = [0.0, 60.0 / 64.0, 115.0 / 64.0, 98.0 / 64.0, 122.0 / 64.0]
FILTER1 = [0.0, 0.0, -52.0 / 64.0, -55.0 / 64.0, -60.0 / 64.0]

ADPCM_BYTES_PER_CHANNEL = 9
ADPCM_SAMPLES_PER_BLOCK = 16


class Unsupported(Exception):
    pass


def read_header(data):
    if data[:6] != b"SeWave":
        raise Unsupported("not a SeWave file")

    size, encoding, ident, blocks, loop, rate_a, rate_b, offset = struct.unpack_from("<IIIIiIII", data, 8)
    rate = (rate_a + rate_b) & 0xFFFFFFFF
    per_block = struct.unpack_from("<I", data, 0x2C)[0]
    payload = len(data) - offset
    if blocks <= 0 or payload <= 0:
        raise Unsupported("no audio in it")

    stride = payload / blocks
    if stride != int(stride):
        raise Unsupported(f"{payload} bytes does not divide into {blocks} blocks")
    stride = int(stride)

    if encoding == 0 and per_block == ADPCM_SAMPLES_PER_BLOCK:
        if stride % ADPCM_BYTES_PER_CHANNEL:
            raise Unsupported(f"{stride}-byte ADPCM block is not whole channels")
        channels = stride // ADPCM_BYTES_PER_CHANNEL
    elif encoding == 1 and per_block == 1:
        if stride % 2:
            raise Unsupported(f"{stride}-byte PCM sample is not whole channels")
        channels = stride // 2
    else:
        raise Unsupported(f"encoding {encoding} with {per_block} samples a block")

    if channels not in (1, 2):
        raise Unsupported(f"{channels} channels")

    return dict(encoding=encoding, ident=ident, blocks=blocks, loop=loop,
                offset=offset, stride=stride, channels=channels,
                rate=rate if 8000 <= rate <= 192000 else None)


def decode_adpcm(payload, blocks, channels):
    """Interleaved 16-bit samples, the same way BgwStream decodes music."""
    out = bytearray()
    history = [[0.0, 0.0] for _ in range(channels)]

    for block in range(blocks):
        base = block * channels * ADPCM_BYTES_PER_CHANNEL
        frames = [[0] * channels for _ in range(ADPCM_SAMPLES_PER_BLOCK)]

        for channel in range(channels):
            half = base + channel * ADPCM_BYTES_PER_CHANNEL
            predictor = payload[half] >> 4
            shift = payload[half] & 0x0F
            if predictor > 4:
                predictor = 0

            previous, before = history[channel]
            for byte in range(8):
                raw = payload[half + 1 + byte]
                # Low nibble first: the samples run in the order the nibbles
                # are written, not the order they read on screen.
                for n, nibble in enumerate((raw & 0x0F, raw >> 4)):
                    signed4 = nibble - 16 if nibble > 7 else nibble
                    sample = float(signed4 << (12 - shift)) + FILTER0[predictor] * previous + FILTER1[predictor] * before
                    sample = max(-32768.0, min(32767.0, sample))
                    before, previous = previous, sample
                    frames[byte * 2 + n][channel] = int(sample)

            history[channel] = [previous, before]

        for frame in frames:
            for value in frame:
                out += struct.pack("<h", value)

    return bytes(out)


def decode(path):
    data = Path(path).read_bytes()
    header = read_header(data)
    payload = data[header["offset"]:]

    if header["encoding"] == 0:
        samples = decode_adpcm(payload, header["blocks"], header["channels"])
    else:
        samples = payload[:header["blocks"] * header["stride"]]

    return samples, header


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("spw", nargs="+", help="files to decode")
    parser.add_argument("--out", default=".", help="where to write the WAVs")
    parser.add_argument("--rate", type=int, default=0,
                        help="override the rate in the file, which is usually 48000")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    written = skipped = 0
    for name in args.spw:
        path = Path(name)
        try:
            samples, header = decode(path)
        except Unsupported as why:
            if not args.quiet:
                print(f"{path.name}: {why}", file=sys.stderr)
            skipped += 1
            continue

        rate = args.rate or header["rate"] or 48000
        target = out / (path.stem + ".wav")
        with wave.open(str(target), "wb") as w:
            w.setnchannels(header["channels"])
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(samples)

        written += 1
        if not args.quiet:
            seconds = len(samples) / 2 / header["channels"] / rate
            loop = "" if header["loop"] < 0 else f", loops at block {header['loop']}"
            print(f"{path.name} -> {target.name}  {seconds:5.2f}s  {rate} Hz  "
                  f"{header['channels']}ch  {'ADPCM' if header['encoding'] == 0 else 'PCM'}{loop}")

    if skipped:
        print(f"{written} written, {skipped} skipped", file=sys.stderr)


if __name__ == "__main__":
    main()
