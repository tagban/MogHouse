"""Decode the effect generator chunks (type 0x05) of a zone DAT.

Each generator is a header followed by a table of section offsets at 0x80 and
an opcode stream from the first section: op(u8) length-in-dwords(u8) pad(u16)
then payload. Op 0x01 names the model placed and where; 0x09 rotates it, 0x0f
scales it, 0x63 names a texture animation, 0x00 ends the stream.
"""
import struct, sys
sys.path.insert(0, r'C:\Users\Gaming\Desktop\MogHouse\tools')
from datscan import chunks, HEADER

path = sys.argv[1]
only = sys.argv[2] if len(sys.argv) > 2 else None
data = open(path, 'rb').read()

stack = []
found = []
for offset, name, ctype, length, parent, child in chunks(data):
    name = name.decode('latin1') if isinstance(name, bytes) else name
    if ctype == 0x00:
        if stack:
            stack.pop()
        continue
    if ctype == 0x01:
        stack.append(name.strip('\0 '))
        continue
    if ctype != 0x05:
        continue
    body = data[offset: offset + length]
    if len(body) < 0x94:
        continue
    sections = struct.unpack_from('<4I', body, 0x80)
    ops = {}
    order = []
    for start in sections:
        # each stream runs to the next stream's start; the length byte's top
        # three bits are flags (0xa4 = 4 words); op 0 is a one-word nop
        end = min([s for s in sections if s > start] + [len(body)])
        pos = start
        guard = 0
        while 0 < pos and pos + 4 <= end and guard < 200:
            guard += 1
            op, ln = body[pos], body[pos + 1] & 0x1f
            if ln == 0 or pos + ln * 4 > end:
                break
            payload = body[pos + 4: pos + ln * 4]
            if op:
                order.append((op, ln, payload))
                ops.setdefault(op, payload)
            pos += ln * 4
    model = None
    where = None
    rot = scale = None
    tex = None
    if 0x01 in ops and len(ops[0x01]) >= 0x1c:
        p = ops[0x01]
        model = p[8:12].decode('latin1').strip('\0 ')
        where = struct.unpack_from('<3f', p, 0x10)
    if 0x09 in ops and len(ops[0x09]) >= 12:
        rot = struct.unpack_from('<3f', ops[0x09], 0)
    if 0x0f in ops and len(ops[0x0f]) >= 12:
        scale = struct.unpack_from('<3f', ops[0x0f], 0)
    if 0x63 in ops and len(ops[0x63]) >= 8:
        tex = ops[0x63][4:8].decode('latin1').strip('\0 ')
    found.append(('/'.join(stack), name, model, where, rot, scale, tex, [o for o, _, _ in order]))

for d, name, model, where, rot, scale, tex, oplist in found:
    if only and only not in d:
        continue
    if where is None:
        continue
    print('%-22s %-5s model %-5s at %8.2f %8.2f %8.2f  rot %s  scale %s  tex %s  ops %s' % (
        d, name, model, where[0], where[1], where[2],
        ('%.2f %.2f %.2f' % rot) if rot else '-',
        ('%.2f %.2f %.2f' % scale) if scale else '-',
        tex or '-', ' '.join('%02x' % o for o in oplist)))
print(len(found), 'generators')
