"""Resolve FFXI file ids to paths, the way the client does.

The retail install indexes its content by a flat file id rather than by path.
Two tables at the install root map one to the other, both with one entry per id:

    VTABLE.DAT   u8   the ROM number holding it, 0 if it is not installed
    FTABLE.DAT   u16  (directory << 7) | file

so id -> ROM{rom}/{directory}/{file}.DAT, where ROM 1 is the folder called
plain "ROM".

Derived by trying candidate bit splits against the install and keeping the one
that resolved: shift 7 resolves 82,912 of 82,912 present ids, shift 8 manages
50.3% and shift 9 27.6%.
"""

import struct
from pathlib import Path


class FileTable:
    def __init__(self, install_root):
        self.root = Path(install_root)
        self._vtable = (self.root / "VTABLE.DAT").read_bytes()
        self._ftable = (self.root / "FTABLE.DAT").read_bytes()
        if len(self._ftable) != len(self._vtable) * 2:
            raise ValueError("VTABLE and FTABLE disagree on how many ids there are")

    def __len__(self):
        return len(self._vtable)

    def path(self, file_id):
        """The path for a file id, or None if it is not installed."""
        rom = self._vtable[file_id]
        if rom == 0:
            return None
        packed = struct.unpack_from("<H", self._ftable, file_id * 2)[0]
        folder = "ROM" if rom == 1 else f"ROM{rom}"
        return self.root / folder / str(packed >> 7) / f"{packed & 0x7F}.DAT"

    def present(self):
        """Every installed file id, in order."""
        for file_id in range(len(self)):
            if self._vtable[file_id]:
                yield file_id
