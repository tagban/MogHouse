#!/usr/bin/env python3
"""Decode a DAT's textures to PNG, so you can look at what a mesh is asking for.

    python tools/dumptex.py ".../ROM/0/78.DAT"                  # every texture
    python tools/dumptex.py ".../ROM/0/78.DAT" win_m01c t_win11a

Texture chunks are not obfuscated, so this needs no key table - unlike models
and MZBs. Names are matched on the second half of the 16-byte field, which is
the texture's own name; the first half is the group it belongs to.

Written because "the room renders grey" and "the room's texture is grey" look
identical from inside the renderer. win_m01c turned out to be cobblestone,
grass and leaves, which said the models near the player were outdoor pieces
and the renderer was drawing them correctly.
"""
import struct, sys, zlib, os

def blocks_bc(data, w, h, dxt1):
    out = bytearray(w * h * 4)
    stride = 8 if dxt1 else 16
    bi = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            off = bi * stride; bi += 1
            if off + stride > len(data): return out
            if dxt1:
                alpha = [255] * 16
                c = off
            else:
                a = data[off:off+8]
                alpha = []
                for k in range(8):
                    alpha.append((a[k] & 0x0F) * 17)
                    alpha.append((a[k] >> 4) * 17)
                c = off + 8
            c0, c1 = struct.unpack_from("<HH", data, c)
            bits = struct.unpack_from("<I", data, c + 4)[0]
            def rgb(v):
                return (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
            p = [rgb(c0), rgb(c1)]
            if dxt1 and c0 <= c1:
                p.append(tuple((p[0][i] + p[1][i]) // 2 for i in range(3)))
                p.append((0, 0, 0))
            else:
                p.append(tuple((2 * p[0][i] + p[1][i]) // 3 for i in range(3)))
                p.append(tuple((p[0][i] + 2 * p[1][i]) // 3 for i in range(3)))
            for t in range(16):
                px, py = bx + (t % 4), by + (t // 4)
                if px >= w or py >= h: continue
                col = p[(bits >> (2 * t)) & 3]
                o = (py * w + px) * 4
                out[o:o+3] = bytes(col)
                out[o+3] = alpha[t]
    return out

def png(path, w, h, rgba):
    raw = b"".join(b"\x00" + bytes(rgba[y*w*4:(y+1)*w*4]) for y in range(h))
    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    open(path, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b""))

dat = open(sys.argv[1], "rb").read()
wanted = sys.argv[2:]
outdir = os.path.dirname(os.path.abspath(sys.argv[0]))
off = 0
while off + 16 <= len(dat):
    packed = struct.unpack_from("<I", dat, off + 4)[0]
    t, length = packed & 0x7F, ((packed >> 7) & 0x7FFFF) * 16
    if length < 16 or off + length > len(dat): break
    if t == 0x20:
        d = dat[off + 16: off + length]
        if len(d) > 0x45:
            name = d[1:17].decode("ascii", "replace").strip()
            short = name.split()[-1] if name.split() else name
            if not wanted or short in wanted:
                w, hgt = struct.unpack_from("<ii", d, 0x15)
                kind = d[0x39:0x3D]
                if d[0] == 0xA1 and kind[:1] in (b"1", b"3") and 0 < w <= 4096 and 0 < hgt <= 4096:
                    rgba = blocks_bc(d[0x45:], w, hgt, kind[:1] == b"1")
                    p = os.path.join(outdir, "tex_%s.png" % short)
                    png(p, w, hgt, rgba)
                    print("  %-12s %4dx%-4d %s -> %s" % (short, w, hgt, kind.decode('ascii','replace'), p))
    off += length
