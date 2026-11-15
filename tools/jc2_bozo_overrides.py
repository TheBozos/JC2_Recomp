"""Lists JC2's port registrations for bozo-externals --overrides: every address runtime/jc2_*.cpp registers with
registerFunction, given as a literal (registerFunction(0x15b400u, ...)) or as a named constant
(constexpr uint32_t kFrameWait = 0x0015B400u; ... registerFunction(kFrameWait, ...)).

  python -I tools/jc2_bozo_overrides.py <repo root> <out.txt>
"""
from pathlib import Path
import re
import sys


def main(root, out):
    constants, used, literals, natives = {}, set(), set(), set()
    for path in sorted((Path(root) / 'runtime').glob('jc2_*.cpp')):
        text = path.read_text()
        for name, value in re.findall(r'constexpr\s+uint32_t\s+(k\w+)\s*=\s*(0x[0-9A-Fa-f]+)u?\s*;', text):
            constants[name] = int(value, 16)
        # A native instruction site: a constant whose line says "// native site".
        for value in re.findall(r'constexpr\s+uint32_t\s+k\w+\s*=\s*(0x[0-9A-Fa-f]+)u?\s*;\s*//\s*native site', text):
            natives.add(int(value, 16))
        for arg in re.findall(r'registerFunction\(\s*([^,\s]+)\s*,', text):
            if arg.startswith('0x'):
                literals.add(int(arg.rstrip('uU'), 16))
            else:
                used.add(arg)
    missing = sorted(used - constants.keys())
    if missing:
        raise SystemExit(f'registerFunction with unknown constants: {", ".join(missing)}')
    addresses = sorted(literals | {constants[n] for n in used})
    Path(out).write_text(''.join(f'override 0x{a:x}\n' for a in addresses) +
                         ''.join(f'native 0x{a:x}\n' for a in sorted(natives)))
    print(f'{len(addresses)} overrides, {len(natives)} native sites -> {out}')


if __name__ == '__main__':
    main(*sys.argv[1:])
