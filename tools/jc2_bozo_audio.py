"""Builds JC2's sound driver as native C++ with BozoEngine's offline IRX tools (tools/native_irx), from the player's
own disc: Sony's LIBSD, MODMIDI and MODHSYN, and Genki's M2IOPSYS and M2SOUND (the driver the game talks to over SIF
RPC, service 0x10000). Each module is relocated at the base the console's IOP gave it, then compiled; nothing runs.

  python -I tools/jc2_bozo_audio.py <repo root> <out dir> [game dir] [engine dir]
Writes <out>/<module>_iop.cpp and <out>/jc2_audio_modules.h (the images for CompiledLibraries). Game-derived output:
it stays in build/.
"""
from pathlib import Path
import subprocess
import sys

# (module, IOP load base): the bases JC2's IOP gave them on the console's boot order (SIO2MAN at 0x10000 first).
MODULES = [('LIBSD', 0x4F500), ('MODMIDI', 0x54200), ('MODHSYN', 0x57F00), ('UAMAN', 0x66E00), ('IMODEMAN', 0x6A100),
           ('M2IOPSYS', 0xAC900), ('M2SOUND', 0xAE300), ('M2IMODE', 0xBC800)]


def main(root, out, game=None, engine=None):
    root = Path(root)
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    game = Path(game) if game else root / 'game'
    engine = Path(engine) if engine else root / 'BozoEngine'
    sys.path.insert(0, str(engine / 'tools/native_irx'))
    from relocate import relocate
    from recompile import Module
    lines = ['#pragma once', '#include "bozo/native/compiled_libraries.h"', '#include <iterator>',
             'inline void addJc2AudioImages(bozo::native::irx::CompiledLibraries &host) {']
    for name, base in MODULES:
        info = relocate(game / 'IRX' / f'{name}.IRX', base, out / name)
        module = Module(str(out / (name + '.json')))
        exports, imports = module.tables()
        subprocess.run([sys.executable, '-I', str(engine / 'tools/native_irx/recompile.py'), str(out / (name + '.json')),
                        '--out', str(out)], check=True)
        data = (out / (name + '.bin')).read_bytes()
        lines += ['{', 'bozo::native::irx::Image image;', f'image.name="{info["name"]}"; image.hash=0x{info["hash"]}ull;',
                  f'image.base={base:#x}; image.entry={info["entry"]}; image.gp={info["gp"]};']
        lines += ['static const uint8_t bytes[]={'] + [' '.join(f'0x{v:02x},' for v in data[i:i + 32])
                                                     for i in range(0, len(data), 32)]
        lines += ['};', 'image.bytes.assign(std::begin(bytes), std::end(bytes));']
        for label, entries in (('imports', imports), ('exports', exports)):
            lines += [f'image.{label}={{'] + [f'{{0x{addr:x},"{lib}",{ordinal}}},'
                                               for addr, (lib, ordinal) in entries.items()] + ['};']
        lines += [f'image.nativeLibsd={str(name == "LIBSD").lower()};', 'host.addImage(std::move(image));', '}']
        print(f'{name}: base {base:#x}, {len(data)} bytes, {len(imports)} imports, {len(exports)} exports')
    lines += ['}']
    (out / 'jc2_audio_modules.h').write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main(*sys.argv[1:])
