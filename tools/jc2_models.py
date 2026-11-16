"""Jade Cocoon 2 (SLUS-20309) 3D model parser: reads models straight from CDDATA.000 resource bytes.

A model resource is an archive: u32 count, then count u32 sub-file offsets (from the resource start).
A sub-file that starts with b'0IVX' (+'00.1') is a model ("IVX0"); others are TIM2 texture banks,
animation data, etc.  Three model variants exist on the disc:
    LDMX 00.1 / SKIN 00.1  skinned characters    (builders 0x14ADD0 -> 0x14B230, VU1 program A 0x4DC6E0)
    LDMX 00.2 / NORM 00.2  static rooms/props    (builders 0x148EA0 -> 0x149350, VU1 program B 0x4D8810)
    LDMX 00.1 / NORM 00.1  static, v1 table layout (same chain format as 00.2)
docs/RECOMP_NOTES.md ("3D models") has the layout. Nothing here reads RAM.

    import jc2_models
    m = jc2_models.parse_resource(resource_bytes)      # {subfile_index: Model}
    for sm in m[1].submeshes: for b in sm.batches: b.pos, b.uv, b.nrm, b.wt / b.col, b.bones, b.tris()
"""
import struct

def _u32(d, o): return struct.unpack_from('<I', d, o)[0]
def _f4(d, o): return struct.unpack_from('<4f', d, o)

# ---------------------------------------------------------------------------------------------
class Material:
    """0x60 bytes (emitted per part by 0x14BC00 skinned / 0x149CE0 static):
    +0x00 flags (0x2 prelit vertex colours, 0x8 double-sided (VU gets flags^8: bit3 = backface cull),
          0x20 textured, 0x40 env-map, 0x100 two-pass, 0x800/0x1000 static-builder paths; 0x4000 set at
          runtime = 'bbox fully inside the view', not on disc),
    +0x0c u8 texture bank, +0x0d u8 slot, +0x0e u16 index  (-> 0x13DB60 texture record),
    +0x10 vec4 colour -> VU[51] (vf29 in program A), +0x20 vec4 -> VU[52] (vf30; .w is an int, 0x32),
    +0x30 GS PRIM (u64; -> GIF tags in VU[56..59]), +0x38 ALPHA_1, +0x40 CLAMP_1, +0x48 TEST_1,
    +0x50 MIPTBP1_1, +0x58 MIPTBP2_1 (u64 GS register values)."""
    def __init__(s, d, off):
        s.off = off
        s.raw = bytes(d[off:off + 0x60])
        s.flags = _u32(d, off)
        s.tex_bank, s.tex_slot = d[off + 0xc], d[off + 0xd]
        s.tex_index = struct.unpack_from('<H', d, off + 0xe)[0]
        s.colour = struct.unpack_from('<4f', d, off + 0x10)
        s.colour2 = struct.unpack_from('<3fI', d, off + 0x20)
        s.prim = struct.unpack_from('<Q', d, off + 0x30)[0]
        s.alpha = struct.unpack_from('<Q', d, off + 0x38)[0]
        s.clamp = struct.unpack_from('<Q', d, off + 0x40)[0]
        s.test = struct.unpack_from('<Q', d, off + 0x48)[0]
        s.miptbp1 = struct.unpack_from('<Q', d, off + 0x50)[0]
        s.miptbp2 = struct.unpack_from('<Q', d, off + 0x58)[0]
    @property
    def textured(s): return bool(s.flags & 0x20)
    def __repr__(s): return 'Material(@%#x flags=%#x tex=%d,%d,%d prim=%#x)' % (s.off, s.flags, s.tex_bank, s.tex_slot, s.tex_index, s.prim)

class Batch:
    """One VU1 batch = one GIF strip (or fan).  Arrays are per vertex."""
    def __init__(s):
        s.pos, s.uv, s.nrm, s.nrm_raw, s.wt, s.col = [], [], [], [], [], []
        s.bones = None          # skinned: 4 palette indices for slots 0..3 (0xff = unused), from the CNT tag ADDR bytes
        s.bone_uploads = []     # skinned: [(vu_qword_addr_rel_top, ...)] of the REF uploads that precede this batch
        s.header = None         # skinned: the batch header qword unpacked to top+0x40 (4 u32)
        s.giftag0 = s.giftag = None
        s.prim = None; s.wind = 1.0; s.nv = 0; s.vifnop_nv = None
        s.bbox = None
    @property
    def rigid(s): return bool(s.header and s.header[0] & 0x100)
    @property
    def reuse_matrices(s): return bool(s.header and (s.header[0] & 0xff) == 0xff)
    @property
    def slots(s):
        """bone slots in use (program A: slot 0 always, then until the first header byte 0xff)"""
        if s.header is None: return []
        if s.rigid: return [0]
        sl = [0]
        for k in range(1, 4):
            if s.header[k] & 0xff == 0xff: break
            sl.append(k)
        return sl
    def tris(s, prim=None):
        """triangle index triples (i0,i1,i2) in GS vertex order, plus the VU's winding sign per triangle
        (program A/B: sign = |vf31.y| * giftag.w, flipped every vertex)."""
        p = s.prim if prim is None else prim
        out = []; w = s.wind
        for i in range(s.nv):
            if i >= 2:
                out.append(((i - 2, i - 1, i) if p & 7 == 4 else (0, i - 1, i), w))
            w = -w
        return out

class Submesh:
    """0x30-byte header then a DMA chain (the builder emits a DMA CALL to header+0x30).
    +0 u32 total vertices, +4 u32 batches, +8 u32 chain qwc (chain length = (qwc+1)*16 incl. the RET tag),
    +0x10 bbox min (4f), +0x20 bbox max (4f)."""
    def __init__(s, d, off, skinned):
        s.off = off; s.chain_off = off + 0x30
        s.nverts, s.nbatches, s.qwc = struct.unpack_from('<3I', d, off)
        s.word3 = _u32(d, off + 12)
        s.bbox = (_f4(d, off + 0x10), _f4(d, off + 0x20))
        s.size = 0x30 + (s.qwc + 1) * 16
        s.batches = decode_chain(d, s.chain_off, skinned)
    def __repr__(s): return 'Submesh(@%#x nv=%d nb=%d qwc=%#x)' % (s.off, s.nverts, s.nbatches, s.qwc)

class Node:
    """0x50 bytes: +0 flags (0x10 = has mesh, 0x20/0x40...), +4 flags2 (bit0/1 billboard), +8 flags3 (bit0 hidden),
    +0xc..+0x16 u16 hierarchy indices, +0x18 u16 mesh index (v1 layout), +0x20 translation, +0x30 rotation (rad), +0x40 scale."""
    def __init__(s, d, off):
        s.off = off
        s.flags, s.flags2, s.flags3 = struct.unpack_from('<3I', d, off)
        s.h = struct.unpack_from('<6H', d, off + 0xc)
        s.mesh_index = struct.unpack_from('<H', d, off + 0x18)[0]
        s.t, s.r, s.sc = _f4(d, off + 0x20), _f4(d, off + 0x30), _f4(d, off + 0x40)
        s.mesh = None
    @property
    def has_mesh(s): return bool(s.flags & 0x10)

class Mesh:
    """v1: 0x30 header (+4 item count, +0x10/+0x20 bbox) + count x (u32 material index, u32 submesh index).
    v2 (static 00.2): 0x30 header (+0xc item count, bbox) + count x [Material 0x60][Submesh], inline after its node."""
    def __init__(s, off, count, bbox):
        s.off = off; s.count = count; s.bbox = bbox; s.items = []   # [(Material, Submesh)]

class Model:
    def __init__(s, d, base=0):
        """d: bytes of the IVX0 sub-file (base: its offset inside the resource, for reporting)"""
        assert d[:4] == b'0IVX', d[:4]
        s.base = base
        s.version = d[0x14:0x18].decode()         # '00.1' / '00.2'
        s.kind = d[0x18:0x1c][::-1].decode()      # 'SKIN' / 'NORM'
        s.skinned = s.kind == 'SKIN'
        s.groups, s.nodes, s.meshes, s.materials, s.submeshes = [], [], [], [], []
        if s.version == '00.2':
            s._parse_v2(d)
        else:
            s._parse_v1(d)
    def _parse_v1(s, d):
        ng, nn, nm, nmat, nsub, nx = struct.unpack_from('<6I', d, 0x20)
        s.counts = (ng, nn, nm, nmat, nsub, nx)
        o = 0x40
        for i in range(ng): s.groups.append(struct.unpack_from('<8I', d, o)); o += 0x20
        for i in range(nn): s.nodes.append(Node(d, o)); o += 0x50
        for i in range(nm):
            cnt = _u32(d, o + 4)
            m = Mesh(o, cnt, (_f4(d, o + 0x10), _f4(d, o + 0x20)))
            m.pairs = [struct.unpack_from('<2I', d, o + 0x30 + 8 * k) for k in range(cnt)]
            s.meshes.append(m); o += (0x30 + 8 * cnt + 15) & ~15
        for i in range(nmat): s.materials.append(Material(d, o)); o += 0x60
        for i in range(nsub):
            sm = Submesh(d, o, s.skinned); s.submeshes.append(sm); o += sm.size
        s.end = o
        for m in s.meshes:
            m.items = [(s.materials[a], s.submeshes[b]) for a, b in m.pairs]
        for n in s.nodes:
            if n.has_mesh: n.mesh = s.meshes[n.mesh_index]
    def _parse_v2(s, d):
        ng, nn = struct.unpack_from('<2I', d, 0x20)
        s.counts = (ng, nn)
        o = 0x30
        for i in range(ng): s.groups.append(struct.unpack_from('<8I', d, o)); o += 0x20
        o = (o + 0x1f) & ~0x1f if o & 0x1f else o
        o = 0x60 if ng == 1 else o
        for i in range(nn):
            n = Node(d, o); s.nodes.append(n); o += 0x50
            if n.has_mesh:
                cnt = _u32(d, o + 0xc)
                m = Mesh(o, cnt, (_f4(d, o + 0x10), _f4(d, o + 0x20)))
                o += 0x30
                for k in range(cnt):
                    mat = Material(d, o)
                    so = o + 0x60
                    if _u32(d, so + 12) == 0xcccccccc and not any(d[so:so + 8]):
                        so += 0x10; o += 0x10           # rare 16-byte pad (fill 0xcccccccc) before the submesh (e.g. res 1689)
                    sm = Submesh(d, so, s.skinned)
                    s.materials.append(mat); s.submeshes.append(sm); m.items.append((mat, sm))
                    o += 0x60 + sm.size
                n.mesh = m; s.meshes.append(m)
        s.end = o
    def __repr__(s):
        return 'Model(%s %s nodes=%d meshes=%d materials=%d submeshes=%d)' % (s.kind, s.version, len(s.nodes), len(s.meshes), len(s.materials), len(s.submeshes))

# ---------------------------------------------------------------------------------------------
def _s16(v): return v - 0x10000 if v & 0x8000 else v
def _bits_f(v): return struct.unpack('<f', struct.pack('<I', v & 0xffffffff))[0]

def decode_chain(d, off, skinned):
    """Walk a model DMA chain (CNT tags with inline VIF data, REF tags = bone-matrix uploads, RET ends it)
    and return its batches.  VIF: STCYCL, UNPACK (with FLG = relative to the double-buffered TOPS), MSCNT."""
    batches = []
    mem = {}; cl = wl = 4
    cur = Batch(); pending_refs = []; nops = []; runs = []
    a = off
    while True:
        t0, t1 = struct.unpack_from('<II', d, a)
        qwc = t0 & 0xffff; tid = (t0 >> 28) & 7
        if tid == 3:      # REF: bone matrices (addr is a placeholder (2) on disc, patched in RAM with palette+bone*0x80)
            vif = d[a + 8:a + 16]
            unp = _u32(vif, 4)
            pending_refs.append(((unp & 0x3ff), t1))
            a += 16; continue
        if tid == 6: break     # RET
        assert tid == 1, 'unexpected DMA tag %#x at %#x' % (t0, a)
        data = d[a + 8:a + 16 + qwc * 16]
        if skinned:
            cur.bones = [(t1 >> (8 * k)) & 0xff for k in range(4)]
        if cur.vifnop_nv is None and _u32(data, 4) >> 24 == 0:
            cur.vifnop_nv = _u32(data, 4) & 0xffff      # 2nd VIF code of the batch's CNT tag: NOP carrying the vertex count
        i = 0
        while i + 4 <= len(data):
            c = _u32(data, i); i += 4
            cmd = (c >> 24) & 0x7f; imm = c & 0xffff; num = (c >> 16) & 0xff
            if cmd >= 0x60:
                vn = (cmd >> 2) & 3; vl = cmd & 3; n = num or 256
                comps = vn + 1; bits = [32, 16, 8, 5][vl]
                sz = ((comps * bits * n + 31) // 32) * 4
                usn = (imm >> 14) & 1
                addr = imm & 0x3ff
                p = data[i:i + sz]; i += sz
                for k in range(n):
                    dst = addr + (k // wl) * cl + (k % wl) if wl < cl else addr + k
                    q = [0, 0, 0, 0]
                    for cc in range(comps):
                        if bits == 32: v = struct.unpack_from('<I', p, (k * comps + cc) * 4)[0]
                        elif bits == 16:
                            v = struct.unpack_from('<H', p, (k * comps + cc) * 2)[0]
                            if not usn and v & 0x8000: v |= 0xffff0000
                        else:
                            v = p[k * comps + cc]
                            if not usn and v & 0x80: v |= 0xffffff00
                        q[cc] = v
                    mem[dst] = (q, comps, bits)
                runs.append((addr, n, comps, bits, cl, wl))
            elif cmd == 1: cl = imm & 0xff; wl = imm >> 8
            elif cmd == 0: nops.append(imm)
            elif cmd == 0x17:          # MSCNT: the batch is complete
                _finish(cur, mem, skinned, nops, pending_refs, runs)
                batches.append(cur)
                cur = Batch(); mem = {}; nops = []; pending_refs = []; runs = []
            else:
                raise ValueError('VIF cmd %#x at %#x' % (c, a))
        a += 16 + qwc * 16
    return batches

def _finish(b, mem, skinned, nops, refs, runs):
    """Interpret one batch's VU memory image.  The layout is found from the UNPACK formats of the STCYCL(4,1) runs:
    V3-32 = position, V2-32 = st, V4-16 = normal (s1.15), V4-8 unsigned = colour (static) / weights (skinned).
    The 4 qwords before the positions are [GIF tag (PRIM|1 = fan variant), bbox min, bbox max, GIF tag
    (NLOOP = nv, PRIM, w = +-1 initial winding)]; for skinned models the qword before those is the batch
    header (4 x u32: slot bytes, 0xff = unused / 'keep previous matrices', 0x100 = rigid)."""
    b.bone_uploads = refs
    pr = [r for r in runs if r[2] == 3 and r[3] == 32 and r[5] < r[4]]
    pos0 = pr[0][0] if pr else (0x45 if skinned else 4)
    b.vu_pos = pos0
    if skinned:
        b.header = mem[pos0 - 5][0] if (pos0 - 5) in mem else None
    tag0 = mem[pos0 - 4][0]
    tag = mem[pos0 - 1][0]
    b.giftag0, b.giftag = tag0, tag
    b.nv = tag[0] & 0x7fff
    b.prim = ((tag[1] >> 15) & 0x7ff)
    b.wind = _bits_f(tag[3])
    b.bbox = (tuple(_bits_f(x) for x in mem[pos0 - 3][0]), tuple(_bits_f(x) for x in mem[pos0 - 2][0]))
    fmt = {}
    for addr, n, comps, bits, cl, wl in runs:
        if wl < cl and pos0 <= addr < pos0 + cl: fmt[addr - pos0] = (comps, bits)
    b.layout = {k: 'V%d-%d' % v for k, v in sorted(fmt.items())}
    st = [k for k, v in fmt.items() if v == (2, 32)]
    nm = [k for k, v in fmt.items() if v == (4, 16)]
    vc = [k for k, v in fmt.items() if v == (4, 8)]
    for i in range(b.nv):
        a = pos0 + 4 * i
        b.pos.append(tuple(_bits_f(x) for x in mem[a][0][:3]))
        if st: b.uv.append(tuple(_bits_f(x) for x in mem[a + st[0]][0][:2]))
        if nm:
            nr = mem[a + nm[0]][0]
            b.nrm_raw.append(tuple(_s16(x & 0xffff) for x in nr))
            b.nrm.append(tuple(_s16(x & 0xffff) / 32768.0 for x in nr[:3]))
        if vc:
            (b.wt if skinned else b.col).append(tuple(x & 0xff for x in mem[a + vc[0]][0]))
    b.has_vc = bool(vc) and not skinned   # static: per-vertex colour present (material flags & 2 = prelit)
    b.has_uv = bool(st); b.has_nrm = bool(nm)

# ---------------------------------------------------------------------------------------------
def subfiles(res):
    n = _u32(res, 0)
    offs = list(struct.unpack_from('<%dI' % n, res, 4)) + [len(res)]
    return [(offs[i], offs[i + 1] - offs[i]) for i in range(n)]

def parse_resource(res):
    """{subfile index: Model} for every IVX0 sub-file of a CDDATA resource"""
    out = {}
    for i, (o, n) in enumerate(subfiles(res)):
        if bytes(res[o:o + 4]) == b'0IVX':
            out[i] = Model(bytes(res[o:o + n]), o)
    return out

def submesh_index(models):
    """{resource offset of the chain (= DMA CALL target - resource base): (subfile, Model, Material, Submesh)}"""
    idx = {}
    for si, m in models.items():
        mats = {}
        for me in m.meshes:
            for mat, sm in me.items: mats[sm.off] = mat
        for sm in m.submeshes:
            idx[m.base + sm.chain_off] = (si, m, mats.get(sm.off), sm)
    return idx
