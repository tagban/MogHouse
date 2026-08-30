"""Pull the SK2 (0x29) skeleton out of a DAT and test candidate layouts.

Written to settle the bone stride empirically rather than trusting a
struct definition copied from elsewhere.
"""
import struct
import sys


def chunks(data):
    off = 0
    while off + 16 <= len(data):
        packed = struct.unpack_from("<I", data, off + 4)[0]
        typ = packed & 0x7F
        length = ((packed >> 7) & 0x7FFFF) * 16
        if length < 16 or off + length > len(data):
            break
        yield typ, data[off:off + 4], data[off + 16:off + length]
        off += length


def main(path):
    payload = None
    for typ, ident, body in chunks(open(path, "rb").read()):
        if typ == 0x29:
            payload = (ident, body)
            break
    if payload is None:
        print("no SK2 chunk")
        return
    ident, body = payload
    print(f"{ident!r} payload {len(body)} bytes")
    print(body[:80].hex(" "))

    for base in (0, 2, 4):
        count = struct.unpack_from("<H", body, base - 2 if base else 0)[0] if base else None
        pass

    h = struct.unpack_from("<4H", body, 0)
    print("first four u16:", h)

    for count in set(h):
        if not 1 <= count <= 512:
            continue
        for base in (2, 4, 6, 8):
            for stride in (28, 30, 32, 34, 36):
                if base + count * stride > len(body):
                    continue
                bad = 0
                selfroot = 0
                offunit = 0
                for i in range(count):
                    o = base + i * stride
                    p = struct.unpack_from("<H", body, o)[0]
                    q = struct.unpack_from("<4f", body, o + 2)
                    n = sum(c * c for c in q) ** 0.5
                    if p >= count:
                        bad += 1
                    if p == i:
                        selfroot += 1
                    if abs(n - 1.0) > 0.02:
                        offunit += 1
                if bad == 0 and offunit == 0:
                    print(f"  FIT count={count} base={base} stride={stride} roots={selfroot}")


if __name__ == "__main__":
    main(sys.argv[1])
