"""Converts Jade Cocoon 2's 3D models offline, from the player's own disc, into BozoEngine's mesh pack (PS2XMSH1).

The models are CDDATA resources (archives: u32 count, then the sub-files' offsets) whose '0IVX' sub-files hold the
meshes as prebuilt VIF chains (tools/jc2_models.py parses them). Each distinct model sub-file becomes one pack file,
named 'M' + the first 16 hex digits of its SHA-1 (resources share models: 1,888 sub-files, fewer distinct ones), its
objects keyed by a submesh's chain offset in the sub-file (the DMA CALL target the game's builders emit, minus the
sub-file's address). One more file, 'RESOURCES', lists per resource id where its model sub-files are and which pack
file each is: the runtime knows which resource the game loaded where (runtime/jc2_meshes.cpp), so a chain address
resolves to (resource, sub-file, offset) without reading any packet.

Per object (a submesh): kind 0 static (VU1 program B) or 1 skinned (program A); positions (x, y, z, 0) one per
vertex in strip order; per batch one strip: prim, material word (bit 0 the starting winding is negative, 1 has s/t,
2 has normals, 3 has vertex colours, 4 rigid, 5 keeps the previous batch's matrices, 6-7 the bone slots used - 1),
slot = the batch's four bone palette indices (skinned; byte k for slot k, 0xFF unused; -1 static); a vertex's rgba is
its colour (static, 0x80 = 1.0) or its four bone weights (skinned, /255). Sections: 'UV  ' f32 s, t per vertex,
'NRM ' s16 x, y, z, 0 per vertex (1.15).

  python -I tools/jc2_mesh_pack.py <game dir> <out.pack> [jobs]
"""
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
import hashlib
import mmap
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'BozoEngine' / 'tools'))
import jc2_models  # noqa: E402
from native_pack.mesh import MeshPackWriter  # noqa: E402


def resources(game):
    loc = (Path(game) / 'CDDATA.LOC').read_bytes()
    count = struct.unpack_from('<I', loc, 0)[0]
    out = []
    for rid in range(count):  # entry id: LOC + 4 + 16 * id (sector, size, sectors, 0)
        if 16 * rid + 16 > len(loc):
            break
        _, sector, size, _ = struct.unpack_from('<4I', loc, 16 * rid)
        if size:
            out.append((rid, sector * 2048, size))
    return out


def model_subfiles(data):
    """[(offset, size)] of a resource's model sub-files ('0IVX'), [] if it is not an archive of them."""
    if len(data) < 8:
        return []
    n = struct.unpack_from('<I', data, 0)[0]
    if not 0 < n < 256 or 4 + 4 * n > len(data):
        return []
    offs = list(struct.unpack_from(f'<{n}I', data, 4)) + [len(data)]
    if any(b < a for a, b in zip(offs, offs[1:])) or offs[0] < 4 + 4 * n:
        return []
    return [(offs[i], offs[i + 1] - offs[i]) for i in range(n) if data[offs[i]:offs[i] + 4] == b'0IVX']


def convert(image):
    """One model sub-file's objects: [(offset, kind, positions bytes, uv bytes, normal bytes, strips)]."""
    model = jc2_models.Model(image)
    objects = []
    for sm in model.submeshes:
        positions, uv, nrm, strips = [], [], [], []
        index = 0
        any_uv = any(b.has_uv for b in sm.batches)
        any_nrm = any(b.has_nrm for b in sm.batches)
        for b in sm.batches:
            slots = len(b.slots) if model.skinned else 1
            word = ((1 if b.wind < 0 else 0) | (2 if b.has_uv else 0) | (4 if b.has_nrm else 0) |
                    (8 if getattr(b, 'has_vc', False) else 0) | (16 if b.rigid else 0) |
                    (32 if b.reuse_matrices else 0) | (max(slots, 1) - 1) << 6)
            slot = struct.unpack('<i', bytes(b.bones))[0] if model.skinned and b.bones else -1
            verts = []
            for i in range(b.nv):
                positions.append(struct.pack('<4f', *b.pos[i], 0.0))
                if any_uv:
                    uv.append(struct.pack('<2f', *(b.uv[i] if b.has_uv else (0.0, 0.0))))
                if any_nrm:
                    nrm.append(struct.pack('<4h', *(b.nrm_raw[i][:3] if b.has_nrm else (0, 0, 0)), 0))
                if model.skinned:
                    rgba = tuple(b.wt[i]) if b.wt else (255, 0, 0, 0)
                else:
                    rgba = tuple(b.col[i]) if b.col else (0x80, 0x80, 0x80, 0x80)
                verts.append((index, 0, 0, 0, 0, rgba, 0, 0))
                index += 1
            strips.append((b.prim or 4, word, slot, verts))
        if index > 0xFFFF:
            raise ValueError(f'submesh at {sm.chain_off:#x}: {index} vertices')
        objects.append((sm.chain_off, 1 if model.skinned else 0, positions, b''.join(uv), b''.join(nrm), strips))
    return objects


def _job(args):
    name, image = args
    try:
        return name, convert(image), None
    except Exception as e:  # reported, the rest goes on
        return name, None, f'{type(e).__name__}: {e}'


def main(game, out, jobs='4'):
    game = Path(game)
    with open(game / 'CDDATA.000', 'rb') as fh:
        cd = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        index, unique = [], {}
        for rid, start, size in resources(game):
            data = cd[start:start + size]
            for off, n in model_subfiles(data):
                image = bytes(data[off:off + n])
                name = 'M' + hashlib.sha1(image).hexdigest()[:16]
                unique.setdefault(name, image)
                index.append((rid, off, n, name))
    names = sorted(unique)
    order = {n: i + 1 for i, n in enumerate(names)}  # file 0 is RESOURCES
    pack = MeshPackWriter()
    res = pack.add_file('RESOURCES', b''.join(struct.pack('<4I', rid, off, n, order[name]) for rid, off, n, name in index))
    res.add_object(0).add_section('RES ', b''.join(struct.pack('<4I', rid, off, n, order[name])
                                                   for rid, off, n, name in index))
    failed, objects, vertices = [], 0, 0
    with ProcessPoolExecutor(max_workers=int(jobs)) as pool:
        results = dict((name, (conv, err)) for name, conv, err in
                       pool.map(_job, ((n, unique[n]) for n in names), chunksize=4))
    for name in names:
        conv, err = results[name]
        f = pack.add_file(name, unique[name])
        if err:
            failed.append(f'{name}: {err}')
            continue
        for offset, kind, positions, uv, nrm, strips in conv:
            o = f.add_object(offset, kind)
            o.positions = [struct.unpack('<4f', p) for p in positions]
            if uv:
                o.add_section('UV  ', uv)
            if nrm:
                o.add_section('NRM ', nrm)
            for prim, word, slot, verts in strips:
                o.add_strip(prim, 0, word, slot, verts)
            objects += 1
            vertices += len(positions)
    pack.write(out)
    print(f'{len(index)} model sub-files in {len({r for r, *_ in index})} resources, {len(names)} distinct: '
          f'{objects} submeshes, {vertices} vertices; {len(failed)} failed')
    for line in failed[:10]:
        print('  ' + line)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:]))
