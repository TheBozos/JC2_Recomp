"""Prepares JC2's executable for BozoEngine's recompiler, offline, from the user's own disc files.

SLUS_203.09 is stripped and has one loadable section for code and data together (0x100000-0x6C4680), so the
recompiler would read its data as code. This keeps the load image as it is and only describes the code spans with
section headers: the main code 0x100000-0x4D8800, then (after the VU1 microcode and its data, 0x4D8800-0x4E3768, never
run by the EE) 0x4E3768-0x4E3E80 (it ends with a kernel-mode handler at 0x4E3DE8), and the late block 0x6B2B00-0x6B9310
(the static constructors; their table follows at 0x6B9310). It also writes the explicit entry
points of the offline function map (recomp/ghidra/jc2_functions.csv, filtered to the code spans).

  python -I tools/jc2_bozo_prepare.py <repo root> <out dir> [game dir]
Writes <out>/jc2.elf and <out>/jc2.entries. Nothing is executed; the output is game-derived and stays in build/.
"""
from pathlib import Path
import csv
import hashlib
import struct
import sys

SH = struct.Struct('<10I')
CODE = [(0x100000, 0x4D8800), (0x4E3768, 0x4E3E80), (0x6B2B00, 0x6B9310)]


def headers(blob):
    off = struct.unpack_from('<I', blob, 0x20)[0]
    count = struct.unpack_from('<H', blob, 0x30)[0]
    return [list(SH.unpack_from(blob, off + i * 40)) for i in range(count)]


def main(root, out, game=None):
    root = Path(root)
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    game = Path(game) if game else root / 'game'
    elf = (game / 'SLUS_203.09').read_bytes()
    print(f'SLUS_203.09 sha1 {hashlib.sha1(elf).hexdigest()}')
    sh = headers(elf)
    code = sh[4]
    if code[3] != 0x100000 or not code[2] & 4:
        raise ValueError('SLUS_203.09: unexpected section layout (not the NTSC-U release this port reads)')
    stri = struct.unpack_from('<H', elf, 0x32)[0]
    names = bytearray(elf[sh[stri][4]:sh[stri][4] + sh[stri][5]])
    # Only the code spans stay executable. The zero-size overlay sections at 0x940000 describe no bytes: drop them
    # (the overlays are separate images, compiled one by one).
    base_off = code[4]
    new = [list(s) for s in sh[:4]]
    for i, (lo, hi) in enumerate(CODE):
        name = len(names)
        names += f'.text{i}'.encode() + bytes(1)
        new.append([name, 1, 6, lo, base_off + lo - code[3], hi - lo, 0, 0, 4, 0])
    sections = new
    blob = bytearray(elf[:base_off + code[5]])
    shstr_index = 1  # .shstrtab stays at index 1
    sections[shstr_index][4] = len(blob)
    sections[shstr_index][5] = len(names)
    blob += names
    blob += bytes((-len(blob)) % 4)
    struct.pack_into('<I', blob, 0x20, len(blob))
    struct.pack_into('<H', blob, 0x30, len(sections))
    struct.pack_into('<H', blob, 0x32, shstr_index)
    blob += b''.join(SH.pack(*s) for s in sections)
    # Program headers: the first LOAD (the whole resident image) only; the empty overlay reservations go.
    struct.pack_into('<H', blob, 0x2C, 1)
    (out / 'jc2.elf').write_bytes(blob)

    entries = []
    with (root / 'recomp/ghidra/jc2_functions.csv').open(newline='') as f:
        for row in csv.DictReader(f):
            a = int(row['Start'], 16)
            if any(lo <= a < hi for lo, hi in CODE) and a % 4 == 0:
                entries.append(a)
    entries = sorted(set(entries))
    (out / 'jc2.entries').write_text(''.join(f'0x{a:x}\n' for a in entries))
    print('jc2.elf: code ' + ', '.join(f'{lo:#x}-{hi:#x}' for lo, hi in CODE) + f'; {len(entries)} entries')


if __name__ == '__main__':
    main(*sys.argv[1:])
