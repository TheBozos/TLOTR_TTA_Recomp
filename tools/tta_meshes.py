#!/usr/bin/env python3
"""Offline mesh converter for The Lord of the Rings: The Third Age (SLUS-21027).

Reads the user's extracted disc (never committed) and converts the prebuilt VIF/GIF packets of every
LLObj resource ('ter ', 'Cobj', 'scbm') into a native vertex format, so the runtime never parses a
VIF or GIF packet:

    python tools/tta_meshes.py <game dir> <out dir> [--only SUBSTR ...] [--obj FILTER ...] [--png]

Resource payload (the 'OBF ' container LLObj_Create 0x2173d8 reads; data = payload start):
    +0x00 'OBF ' u32 0x105, +0x08 'HEAD' u32 0x70; +0x10 h node count, +0x18 u32 slot count
    +0x80 slot-count qwords, then the node chunks in depth-first order (0x217520):
      'EHLE' (bytes 'ELHE') record chunk, always 0x100 bytes; the record = chunk + 8:
          +0 h child count, +2 h inline size, +4 u32 geometry size (words), +8 h slot, +0x4c b flag,
          +0x58 bounding sphere (x, y, z, r), +0x88 b shader type, +0x8b b instancing mode, +0x8c h param
      'ELTL' inline chunk if record+2 != 0: {tag, u32 size} + size bytes (texture id list)
      'ELDA' geometry chunk if record+4 != 0: {tag, u32 size} + size bytes; geometry = chunk + 0x80
    The runtime node (0x60 bytes) holds +0 = record pointer, so (record pointer - payload start) is
    the record offset used as key here.

Geometry by shader type:
  * types 0-5 (and 8/11 if present): the geometry IS a VIF1 packet (DIRECT A+D, STROW/STCOL, STMASK,
    UNPACK, MSCNT). Per batch (one MSCNT) at TOPS: [A+D GIF tag + NLOOP register qwords] PRIM tag
    [+ 1 parameter qword for types 4/5] then NLOOP-vertex arrays, in this order (VU1 set-0 programs,
    e.g. 0x2418): position V3-32/V4-32 float, ST (V2-16 signed -> ITOF12, i.e. /4096; or V2-32
    float), colour V4-8 (RGBA, GS units 0x80 = 1.0), [type 4: second V4-8 colour = animated delta].
    ADC (no triangle ends at this vertex) = colour R & 1 (MTIR R; IAND 1; adds 131.w to XYZF.w).
    Several PRIM tags may share one MSCNT (the program loops until EOP).
  * type 21 (VU1 set 8e45395d, program 0x4c8 / "0x460"): geometry = 4x4 instance matrix (rows,
    p' = x*r0 + y*r1 + z*r2 + r3), +0x40 u16 instance#, u16 group, +0x44 u32 colour count (256),
    +0x48 u32 packet qwc, +0x4c u32 ?, +0x50 256 u32 colours (RGBA, UNPACKed V4-8 to VU 136 by the
    DS flush 0x2133d0), then (instance# == 1 only) the packet: PRIM tag then 2 qwords per vertex
    (x, y, z) (s, t, colour index | ADC bit 15) as V3-32; ST are floats. Nodes with instance# != 1
    draw the packet of the node with instance# 1 and the same group (table 0x448b20 filled by
    0x220f30 at create, copied by 0x220f88 after LLObj_Create): geometry is shared, matrix and
    colour table are per node.
  * type 17 (scbm WeightedSkin, VU1 set 8df1e059, batch program 0x378): kind 3, see skin_batch /
    skin_sections. Per kind-1 section ("part"): a byte stream of DMA chunks (palette entries
    {skeleton bone, VU offset}, material index) and a VIF stream; per MSCNT 3-qword vertices
    (normal*1024 + influence, position + matrix-cache slot), V4-8 influence
    records (4 palette offsets, 4 weights*127), and one entry per GS strip vertex (s, t /1024,
    source vertex, output slot | drawn flag) that may name a vertex of the previous batch.
    Material = name record -> texture id by the game's name hash 0x26ce88 (see load_name_hash).
    Parts with the other layout (header q0.x with bit 15, vertices at a 2-qword stride, then V4-16 / V3-16 / S-16
    index streams, no s/t or colour) are skipped with a warning: they are not drawn by the 8df1e059 programs but
    are shadow-volume / silhouette meshes (edges {v0, face, face, v1}, faces {3 refs}) read by the set-5 code
    (0x355780: four header ranges in q0/q1 .x/.y/.z/.w, OPMULA face normals), with no morph data.
    The structure of every type-17 geometry is also listed in <out>/scbm.txt (scbm_summary).
  * type 19 (water): the water module's EE edge list (shoreline wave/foam quads), converted at rest
    (kind 0; see water_batches for the layout and what is not decoded).

Keys: (type, index, record offset) is unique within one .SCP but NOT across the disc: every level
SCP reuses the same 'ter '/'scbm' indices for different payloads. Identical payloads share one file;
differing ones are <type>_<index>_v<n>.mesh (conflicts.txt), so the runtime needs more than (type, index):
(type, index, unpacked payload size, record offset) still collides for 51 nodes (small 'ter '
resources whose packets differ); adding the record's bounding sphere (record +0x58, 16 bytes, read
through runtime node+0) makes every key unique on the disc. index.txt carries the payload size.

Output: one file per resource <type>_<index>[_v<n>].mesh and index.txt. File layout (little endian):
    header : 'TTAM' u32 version (2), u32 node count, char[4] type, u32 index, u32 payload size
    node   : u32 record offset, i8 shader type, u8 kind (0 own batches, 1 type-21 instance of
             another node's batches, 2 not converted, 3 skinned), u16 node#, u32 batch count (all
             sections), u32 vertex count, u32 master node# (kind 1, else 0xffffffff),
             if shader type 21: 16 f32 matrix, u16 instance#, u16 group, u32 +0x4c word, 256 u32 colours
             then the batches; kind 3 instead: u32 section count, per section {u32 section index
             (its bit in the visibility mask G+0x280), u32 batch count, batches}
    skinned batch: as below but each vertex is 64 bytes {f32 x, y, z (bind/model space: the VU's
             (int/1024) * the geometry+0x18 scale; the skin set's VIF packet 0x354968 sets STROW
             8224.0 and VU35.x = 8224.0, so each 16-bit int v unpacks (STMOD 1) to 8224 + v/1024 and
             the bias leaves v/1024; normals and s, t use the same /1024), f32 nx, ny, nz (unit), f32 s, t, u32 rgba (0x80808080:
             the mesh carries no vertex colour, the VU lights it), u8 bone[4] (skeleton indices into
             *(G+0x290); the 3-byte palette entries could carry u16 indices but the disc's maximum
             is 148: a larger one is a warning), f32 weight[4] (the record's /127 values renormalised
             to sum 1; unused = 0), u8 adc, 7 pad}; PRIM 0x7c (strip, Gouraud, textured, fog, blend: the tag
             template at VU 40 from that packet), no registers.
    batch  : u64 texture id (TEX0 of the A+D block in force, 0 = none), u16 GS PRIM (effective: type
             | IIP<<3 | TME<<4 | FGE<<5 | ABE<<6 | AA1<<7 | FST<<8 | CTXT<<9 | FIX<<10), u8 colour
             mode (0 RGBA, 1 index into the node's colour table), u8 register count, u32 vertex count,
             registers {u8 GS register, 7 pad, u64 value} (this batch's own A+D writes), vertices
             {f32 x, y, z, f32 s, t, u32 colour, u8 adc, 3 pad} (28 bytes)
"""
import argparse
import hashlib
import os
import struct
import sys
import time
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tta_textures as tex  # noqa: E402  (SCP container parser)

MESH_TYPES = ('ter ', 'Cobj', 'scbm')
VERSION = 2

# ----------------------------------------------------------------------------------------
# LLObj node tree (0x2173d8 / 0x217520)


def walk_nodes(p):
    if p[:4] != b'OBF ' or p[8:12] != b'HEAD':
        raise ValueError('not an OBF container')
    count = struct.unpack_from('<h', p, 0x10)[0]
    slots = struct.unpack_from('<I', p, 0x18)[0]
    cur = 0x80 + slots * 16
    nodes = []
    stack = [1]  # children still to read at each depth (root: one node)
    while stack:
        if stack[-1] == 0:
            stack.pop()
            continue
        stack[-1] -= 1
        rec = cur + 8
        if p[cur:cur + 4] != b'ELHE':
            raise ValueError('record chunk 0x%x is %r' % (cur, p[cur:cur + 4]))
        cur += 0x100
        nch, inl = struct.unpack_from('<hh', p, rec)
        gsz = struct.unpack_from('<I', p, rec + 4)[0]
        n = dict(num=len(nodes), rec=rec, depth=len(stack) - 1, children=nch, gsz=gsz,
                 stype=struct.unpack_from('<b', p, rec + 0x88)[0], inst=p[rec + 0x8b],
                 param=struct.unpack_from('<h', p, rec + 0x8c)[0], geo=None, geo_end=None)
        if inl:
            cur += 8 + struct.unpack_from('<I', p, cur + 4)[0]
        if gsz:
            size = struct.unpack_from('<I', p, cur + 4)[0]
            n['geo'], n['geo_end'] = cur + 0x80, cur + 8 + size
            cur += 8 + size
        nodes.append(n)
        if nch:
            stack.append(nch)
    if len(nodes) != count or cur != len(p):
        raise ValueError('tree: %d nodes (HEAD %d), end 0x%x of 0x%x' % (len(nodes), count, cur, len(p)))
    return nodes


# ----------------------------------------------------------------------------------------
# VIF1 model: just enough to replay the UNPACKs of one batch into VU1 data memory


UNPACK_SIZE = {0: 4, 1: 2, 2: 1, 4: 8, 5: 4, 6: 2, 8: 12, 9: 6, 10: 3, 12: 16, 13: 8, 14: 4, 15: 2}
UNPACK_NAME = {0: 'S-32', 1: 'S-16', 2: 'S-8', 4: 'V2-32', 5: 'V2-16', 6: 'V2-8', 8: 'V3-32', 9: 'V3-16',
               10: 'V3-8', 12: 'V4-32', 13: 'V4-16', 14: 'V4-8', 15: 'V4-5'}


class Unpack:
    __slots__ = ('uid', 'vt', 'addr', 'num', 'usn', 'masked')

    def __init__(self, uid, vt, addr, num, usn, masked):
        self.uid, self.vt, self.addr, self.num, self.usn, self.masked = uid, vt, addr, num, usn, masked


def _elements(vt, usn, data, cnt):
    """cnt vectors of the unpack as lists of u32."""
    vn, vl = (vt >> 2) & 3, vt & 3
    comps = vn + 1
    out = []
    if vt == 15:
        for i in range(cnt):
            v = struct.unpack_from('<H', data, i * 2)[0]
            out.append([(v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3, (v >> 15) << 7])
        return out
    if vl == 0:
        vals = struct.unpack_from('<%dI' % (cnt * comps), data)
    elif vl == 1:
        vals = struct.unpack_from('<%d%s' % (cnt * comps, 'H' if usn else 'h'), data)
        vals = [v & 0xffffffff for v in vals]
    else:
        vals = struct.unpack_from('<%d%s' % (cnt * comps, 'B' if usn else 'b'), data)
        vals = [v & 0xffffffff for v in vals]
    for i in range(cnt):
        e = list(vals[i * comps:(i + 1) * comps])
        if comps == 1:
            e = e * 4
        else:
            e += [0] * (4 - comps)  # V2/V3: missing fields are left as written (here: 0) unless masked
        out.append(e)
    return out


class Batch:
    """One MSCNT worth of VU1 data memory (TOPS-relative), plus the GS state seen in DIRECTs."""

    def __init__(self):
        self.mem = {}      # qword addr -> [x, y, z, w] u32
        self.writer = {}   # qword addr -> Unpack


def vif_batches(p, a, e, warn):
    """Replay a VIF1 stream; yield (Batch, direct_writes) per MSCNT (direct writes: (reg, value))."""
    cl = wl = 1
    mask = 0
    mode = 0
    row = [0, 0, 0, 0]
    col = [0, 0, 0, 0]
    b = Batch()
    direct = []
    uid = 0
    while a + 4 <= e:
        w = struct.unpack_from('<I', p, a)[0]
        cmd, num, imm = (w >> 24) & 0x7f, (w >> 16) & 0xff, w & 0xffff
        a += 4
        if cmd >= 0x60:
            vt = cmd & 0xf
            if vt not in UNPACK_SIZE:
                raise ValueError('UNPACK format 0x%x at 0x%x' % (vt, a - 4))
            n = num or 256
            cnt = n if wl <= cl else cl * (n // wl) + min(n % wl, cl)
            size = (UNPACK_SIZE[vt] * cnt + 3) & ~3
            if wl > cl:
                warn('STCYCL filling write %d/%d' % (cl, wl))
            usn, masked = bool(imm & 0x4000), bool(cmd & 0x10)
            u = Unpack(uid, vt, imm & 0x3ff, n, usn, masked)
            uid += 1
            for i, v in enumerate(_elements(vt, usn, p[a:a + size], cnt)):
                addr = (u.addr + (i // wl) * cl + i % wl if wl < cl else u.addr + i) & 0x3ff  # skipping write
                old = b.mem.get(addr, [0, 0, 0, 0])
                cyc = min(i % max(wl, 1), 3)  # the write cycle selects the mask row and the column
                m = (mask >> (8 * cyc)) & 0xff if masked else 0
                outv = []
                for f in range(4):
                    sel = (m >> (2 * f)) & 3 if masked else 0
                    if sel == 0:
                        x = v[f]
                        if mode == 1:
                            x = (x + row[f]) & 0xffffffff
                        elif mode == 2:
                            x = (x + row[f]) & 0xffffffff
                            row[f] = x
                    elif sel == 1:
                        x = row[f]
                    elif sel == 2:
                        x = col[cyc]
                    else:
                        x = old[f]
                    outv.append(x)
                b.mem[addr] = outv
                b.writer[addr] = u
            a += size
        elif cmd == 0x01:
            cl, wl = imm & 0xff, imm >> 8
        elif cmd == 0x05:
            mode = imm & 3
        elif cmd == 0x20:
            mask = struct.unpack_from('<I', p, a)[0]
            a += 4
        elif cmd == 0x30:
            row = list(struct.unpack_from('<4I', p, a))
            a += 16
        elif cmd == 0x31:
            col = list(struct.unpack_from('<4I', p, a))
            a += 16
        elif cmd in (0x50, 0x51):
            n = imm or 65536
            direct += gif_ad(p, a, a + n * 16)
            a += n * 16
        elif cmd in (0x14, 0x15, 0x17):
            b.end = a  # stream offset just after the MSCNT
            yield b, direct
            b, direct = Batch(), []
        elif cmd == 0x4a:
            a = (a + 7) & ~7
            a += (num or 256) * 8
        elif cmd in (0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x10, 0x11, 0x13):
            pass
        else:
            raise ValueError('VIF code 0x%08x at 0x%x' % (w, a - 4))
    if b.mem:
        warn('UNPACKs after the last MSCNT')


def gif_ad(p, a, e):
    """A+D register writes of PACKED GIF packets in [a, e)."""
    out = []
    while a + 16 <= e:
        lo, hi = struct.unpack_from('<QQ', p, a)
        nloop, eop = lo & 0x7fff, lo >> 15 & 1
        nreg = (lo >> 60) or 16
        flg = lo >> 58 & 3
        a += 16
        if flg == 0 and nreg == 1 and (hi & 0xf) == 0xe:
            for i in range(nloop):
                v, r = struct.unpack_from('<QQ', p, a + i * 16)
                out.append((r & 0xff, v))
            a += nloop * 16
        else:
            a += nloop * nreg * 16 if flg == 0 else 0
        if eop:
            break
    return out


# ----------------------------------------------------------------------------------------
# batch interpretation (what the VU1 programs read)


def f32(u):
    return struct.unpack('<f', struct.pack('<I', u & 0xffffffff))[0]


def s32(u):
    return u - (1 << 32) if u & 0x80000000 else u


class GsState:
    def __init__(self):
        self.tex0 = 0
        self.prmode = 0
        self.prmodecont = 1

    def apply(self, writes):
        for r, v in writes:
            if r in (0x06, 0x07):
                self.tex0 = v
            elif r == 0x1b:
                self.prmode = v & 0x7ff
            elif r == 0x1a:
                self.prmodecont = v & 1


def effective_prim(tag_prim, gs):
    if gs.prmodecont:
        return tag_prim & 0x7ff
    return (tag_prim & 7) | (gs.prmode & 0x7f8)


def read_tags(b, warn, at=0):
    """The next PRIM tag from VU address at (TOPS-relative), after any A+D blocks.
    Yields one (ad_writes, prim, nloop, data_addr, eop)."""
    pending = []
    for _ in range(64):
        if at not in b.mem:
            warn('no GIF tag at VU %d' % at)
            return
        x, y, z, w = b.mem[at]
        nloop, eop = x & 0x7fff, x >> 15 & 1
        if (y >> 28) == 1 and (z & 0xf) == 0xe and not (y >> 26) & 3:  # A+D block
            for i in range(nloop):
                q = b.mem.get(at + 1 + i)
                if q is None:
                    warn('A+D data missing')
                    break
                pending.append((q[2] & 0xff, q[0] | (q[1] << 32)))
            at += 1 + nloop
            if eop:
                return
            continue
        prim = (y >> 15) & 0x7ff if (y >> 14) & 1 else 0
        yield pending, prim, nloop, at + 1, eop
        pending = []
        return


def arrays_after(b, start, n):
    """The vertex arrays after a PRIM tag: runs of n qwords written by one UNPACK each."""
    out, at = [], start
    if n <= 0:
        return out, at
    tag_writer = b.writer.get(start - 1)
    if tag_writer is not None and b.writer.get(at) is tag_writer:
        at += 1  # parameter qword unpacked with the tag (types 4/5: UV scroll, colour animation)
    while True:
        u = b.writer.get(at)
        if u is None or u is tag_writer or u.addr != at or u.num != n or u.vt not in (4, 5, 8, 12, 14):
            break
        out.append((u, at))
        at += n
    return out, at


def static_batches(b, gs, warn):
    """Set-0/1 programs: [A+D] PRIM [param] pos[n] st[n] colour[n] (extra[n]); loops to EOP."""
    at = 0
    res = []
    for _ in range(32):
        tags = list(read_tags(b, warn, at))
        if not tags:
            break
        ad, prim, n, start, eop = tags[0]
        gs.apply(ad)
        arrays, end = arrays_after(b, start, n)
        pos = st = colour = None
        for u, base in arrays:
            if pos is None and u.vt in (8, 12):
                pos = (u, base)
            elif st is None and u.vt in (4, 5):
                st = (u, base)
            elif colour is None and u.vt == 14:
                colour = (u, base)
        if pos is None:
            warn('batch without positions (%s)' % ','.join(UNPACK_NAME[u.vt] for u, _ in arrays))
            break
        verts = []
        for i in range(n):
            q = b.mem[pos[1] + i]
            x, y, z = f32(q[0]), f32(q[1]), f32(q[2])
            if st is None:
                s = t = 0.0
            elif st[0].vt == 5:
                tq = b.mem[st[1] + i]
                s, t = s32(tq[0]) / 4096.0, s32(tq[1]) / 4096.0
            else:
                tq = b.mem[st[1] + i]
                s, t = f32(tq[0]), f32(tq[1])
            if colour is None:
                rgba, adc = 0x80808080, 0
            else:
                c = b.mem[colour[1] + i]
                rgba = (c[0] & 0xff) | (c[1] & 0xff) << 8 | (c[2] & 0xff) << 16 | (c[3] & 0xff) << 24
                adc = c[0] & 1
            verts.append((x, y, z, s, t, rgba, adc))
        res.append(dict(tex=gs.tex0, prim=effective_prim(prim, gs), cmode=0, regs=ad, verts=verts,
                        layout=' '.join(UNPACK_NAME[u.vt] for u, _ in arrays)))
        if eop:
            break
        at = end
    return res


def terrain_batches(b, gs, warn):
    """VU1 program 0x4c8 (set 8e45395d): [A+D] PRIM tag, then (x y z)(s t index|ADC) per vertex."""
    res = []
    for ad, prim, n, start, eop in read_tags(b, warn):
        gs.apply(ad)
        u = b.writer.get(start)
        if u is None or u.vt != 8 or any(b.writer.get(start + i) is not u for i in range(2 * n)):
            warn('terrain batch layout')
            continue
        verts = []
        for i in range(n):
            q, r = b.mem[start + 2 * i], b.mem[start + 2 * i + 1]
            idx = r[2] & 0xffff
            verts.append((f32(q[0]), f32(q[1]), f32(q[2]), f32(r[0]), f32(r[1]), idx & 0x7fff, idx >> 15))
        res.append(dict(tex=gs.tex0, prim=effective_prim(prim, gs), cmode=1, regs=ad, verts=verts,
                        layout='V3-32x2'))
    return res


# ----------------------------------------------------------------------------------------
# a resource


def water_batches(p, g, e, warn):
    """Type 19 (ShaderObject_Water, create 0x221a20; per-frame work in the water driver 0x2246e8 -> 0x2236a0,
    sine table 0x403788 built by the module init 0x221928): not a VIF packet but an EE list of EDGES that the
    driver extrudes into animated wave/foam quads. Geometry: u16 edge count, u16 pad, per edge 2 vertices of
    60 bytes {f32 x, y, z, f32 s, t, u8 r, g, b, a (GS units), f32 dx, dy, dz (extrusion vector), f32 ds, dt
    (the matching s/t change), f32 k0, k1, k2 (small integers: per-vertex phase/colour offsets, not
    decoded), f32 f (0 or +-57: not decoded)}, then u64 texture id, u8 subdivisions along / across (+8,
    +9), u8 x2, u16, u16, u32 (wave parameters, not decoded).
    Converted at rest: per edge one quad (p0, p1, p0 + d0, p1 + d1, with s, t + ds, dt), drawn as a
    4-vertex strip (first two ADC); PRIM 0x5c (strip, Gouraud, textured, blend) is inferred."""
    rows = struct.unpack_from('<H', p, g)[0]
    end = g + 4 + rows * 2 * 60
    if not rows or end + 8 > e:
        warn('water geometry size')
        return []
    verts = []
    for k in range(rows):
        quad = []
        for side in range(2):
            o = g + 4 + (2 * k + side) * 60
            x, y, z, s_, t = struct.unpack_from('<5f', p, o)
            rgba = struct.unpack_from('<I', p, o + 20)[0]
            dx, dy, dz, ds, dt = struct.unpack_from('<5f', p, o + 24)
            quad.append(((x, y, z, s_, t), (x + dx, y + dy, z + dz, s_ + ds, t + dt), rgba))
        (a0, a1, ca), (b0, b1, cb) = quad
        verts += [a0 + (ca, 1), b0 + (cb, 1), a1 + (ca, 0), b1 + (cb, 0)]
    tex_id = struct.unpack_from('<Q', p, end)[0]
    return [dict(tex=tex_id, prim=0x5c, cmode=0, regs=[], verts=verts, layout='water')]


def convert_resource(p, warn):
    nodes = walk_nodes(p)
    groups = {}
    for n in nodes:
        n['kind'], n['batches'], n['master'], n['t21'] = 2, [], None, None
        if not n['geo']:
            continue
        g, e = n['geo'], n['geo_end']
        st = n['stype']
        if st == 21:
            m = struct.unpack_from('<16f', p, g)
            inst, grp = struct.unpack_from('<HH', p, g + 0x40)
            ncol, qwc, w4c = struct.unpack_from('<III', p, g + 0x44)
            cols = struct.unpack_from('<256I', p, g + 0x50)
            if ncol != 256:
                warn('type 21 colour count %d' % ncol)
            n['t21'] = (m, inst, grp, w4c, cols)
            if inst == 1:
                gs = GsState()
                pk = g + 0x50 + ncol * 4
                for b, direct in vif_batches(p, pk, pk + qwc * 16, warn):
                    gs.apply(direct)
                    n['batches'] += terrain_batches(b, gs, warn)
                n['kind'] = 0
                groups[grp] = n
        elif st == 17:
            n['sections'] = skin_sections(p, n, warn)
            n['batches'] = [bt for _, bts in n['sections'] for bt in bts]
            n['kind'] = 3
        elif st == 19:
            n['batches'] = water_batches(p, g, e, warn)
            n['kind'] = 0
        else:
            gs = GsState()
            for b, direct in vif_batches(p, g, e, warn):
                gs.apply(direct)
                n['batches'] += static_batches(b, gs, warn)
            n['kind'] = 0
    for n in nodes:
        if n['t21'] and n['t21'][1] != 1:
            master = groups.get(n['t21'][2])
            if master is None:
                warn('type 21 group %d without a master in this resource' % n['t21'][2])
            else:
                n['kind'], n['master'] = 1, master['num']
    return nodes


# ----------------------------------------------------------------------------------------
# type 17 WeightedSkin (VU1 set 8df1e059, batch program 0x378 / 0xb10, skin build 0x16e0)

NAME_HASH = None  # (char map, weights) of 0x26ce88, read from the executable


def load_name_hash(game):
    """0x26ce88: material name -> 64-bit texture id (chars 0-5 -> low word, 6-11 -> high word,
    each the sum of map[c] * weight[i]; map at 0x378898, weights 1, 40, 40^2 ... at 0x3789c0)."""
    global NAME_HASH
    import glob
    for path in sorted(glob.glob(os.path.join(game, 'SLUS_*'))):
        with open(path, 'rb') as f:
            e = f.read()
        base = 0x110000 - 0x1000  # the executable's single PT_LOAD segment
        NAME_HASH = (e[0x378898 - base:0x378898 - base + 256], struct.unpack_from('<6I', e, 0x3789c0 - base))
        return


def name_id(name):
    cmap, w = NAME_HASH
    lo = sum(cmap[c] * w[i] for i, c in enumerate(name[:6])) & 0xffffffff
    hi = sum(cmap[c] * w[i] for i, c in enumerate(name[6:12])) & 0xffffffff
    return lo | hi << 32


def skin_chunks(p, s, e):
    """The part's byte stream (0x21d9f8): per DMA chunk (VIF qword count, material index or None,
    [(skeleton bone index, VU palette offset)])."""
    out, o = [], s
    while o < e:
        b0 = p[o]
        o += 1
        cnt, mat, ents = b0 & 0x7f, None, []
        if b0 & 0x80:
            b1 = p[o]
            o += 1
            if b1 & 0x80:
                mat = p[o]
                o += 1
            if b1 & 0x40:
                cnt |= p[o] << 7
                o += 1
            for _ in range(b1 & 0x1f):
                if b1 & 0x20:
                    ents.append(((p[o] << 8) | p[o + 1], p[o + 2]))
                    o += 3
                else:
                    ents.append((p[o], p[o + 1]))
                    o += 2
        out.append((cnt, mat, ents))
    if o != e:
        raise ValueError('skin byte stream ends at +%d of %d' % (o - s, e - s))
    return out


def _s(x, bits=16):
    x &= (1 << bits) - 1
    return x - (1 << bits) if x >> (bits - 1) else x


SKIN_STATS = Counter()


def skin_batch(b, palette, cache, prev, scale, tex, warn):
    """One MSCNT: q0 = (output count, vertex end | 0x8000, ..), q1.y = vertex start | 0x8000; vertices
    are 3 qwords {normal x y z (x1024), w}, {position x y z (/1024 x geometry scale), w = blended-matrix
    cache slot (| flag)}, {VU scratch}; normal.w: 0x8000 | record address = blend the record's bones
    (and keep the result in the cache slot), 0x8000 alone = as the previous vertex, < 0x8000 = the
    matrix cached at that slot (possibly by an earlier batch: the cache persists in VU memory). The VU sees
    w ^ 0x8000 (MTIR / ILW of the unpacked float 8224 + w/1024), so in the raw data: normal.w 0x8000 = no
    rebuild, 0x8000 | n = blend record n, n < 0x8000 = load slot n (VU 211 + n, 7 qwords); a blend is
    stored to slot position.w only when position.w lacks bit 15; influence records are 2 V4-8
    qwords {4 palette offsets (< 0 unused), 4 weights * 127}; output entries from TOP+3, one per GS
    vertex: {s, t (/1024), source vertex address | 0x8000, output slot * 3 | 0x8000 = drawn} in [3, vertex start);
    entries [3, q1.z) are one per vertex in order, [q1.z, q1.w) repeat vertices of this batch, and
    [q1.w, vertex start) name vertices of the previous batch (still in VU memory). prev: that batch's
    vertices; returns (batch, vertices) or (None, None)."""
    m = b.mem
    pscale = scale / 1024.0  # (row 8224.0 + v/1024) - VU35.x 8224.0, then * VU33.w = BONE+0x18
    if 0 not in m or 1 not in m:
        warn('skin batch without header')
        return None, None
    nout = m[0][0] & 0x7fff
    vstart, vend, pfirst = m[1][1] & 0x3ff, m[0][1] & 0x3ff, m[1][3] & 0x3ff
    if pfirst == 0:  # q1.w = 0x8000: no entry names the previous batch
        pfirst = vstart
    if m[0][0] & 0x8000 or vend < vstart or vstart > 0x80 or not 3 <= pfirst <= vstart:
        warn('skin batch layout (not the 0x378 program)')
        return None, None
    verts = {}
    infl = ((0, 0, 0, 0), (1.0, 0.0, 0.0, 0.0))
    for a in range(vstart, vend, 3):
        if a not in m or a + 1 not in m:
            warn('skin vertex missing')
            return None, None
        nq, pq = m[a], m[a + 1]
        ptr, cslot = nq[3] & 0xffff, pq[3] & 0x7fff
        if not ptr & 0x8000:
            if ptr not in cache:
                warn('skin matrix cache slot never filled')
                return None, None
            infl = cache[ptr]
        elif ptr & 0x7fff:
            ptr &= 0x7fff
            rec, wts = m.get(ptr & 0x3ff), m.get((ptr + 1) & 0x3ff)
            if rec is None or wts is None:
                warn('skin influence record missing')
                return None, None
            bones, ws = [], []
            for k in range(4):
                off = _s(rec[k], 8)
                if off < 0:
                    bones.append(0)
                    ws.append(0.0)
                    continue
                if off not in palette:
                    warn('palette offset without bone')
                bones.append(palette.get(off, 0))
                ws.append(_s(wts[k], 8) / 127.0)
            tot = sum(ws)
            if tot > 0:
                # The records' weights are rounded independently (64 + 64 = 128 of 127): the VU blends with that
                # sum (a 1/127 too large blended matrix, +0.8% of the vertex's model-space distance from the
                # origin), which tears lips and eyelids apart in close-ups; the converted weights sum to 1.
                ws = [w / tot for w in ws]
            infl = (tuple(bones), tuple(ws))
            if not pq[3] & 0x8000:
                # 0x16e0 stores the blended matrix only when position.w, as the VU reads it (ILW of the
                # float 8224 + w/1024, i.e. w ^ 0x8000), is negative: raw w without bit 15. A raw w with bit 15
                # keeps the slot as it is (another vertex's matrix may still be cached there).
                cache[cslot] = infl
            SKIN_STATS['influences %d' % sum(1 for x in ws if x)] += 1
        nx, ny, nz = (_s(x) / 1024.0 for x in nq[:3])
        ln = (nx * nx + ny * ny + nz * nz) ** 0.5 or 1.0
        verts[a] = ((_s(pq[0]) * pscale, _s(pq[1]) * pscale, _s(pq[2]) * pscale),
                    (nx / ln, ny / ln, nz / ln), infl)
    out = [None] * nout
    for e in range(3, vstart):  # entries fill [3, vertex start); the last ones may repeat (padding)
        q = m.get(e)
        if q is None:
            warn('skin output entry missing')
            return None, None
        src, slot = q[2] & 0x3ff, (q[3] & 0x7fff) // 3
        if slot >= nout:
            continue  # padding entry
        vv = (verts if e < pfirst else prev or {}).get(src)
        if vv is None:
            warn('skin output entry out of range')
            return None, None
        pos, nrm, (bones, ws) = vv
        out[slot] = pos + nrm + (_s(q[0]) / 1024.0, _s(q[1]) / 1024.0, 0x80808080) + bones + ws + (
            0 if q[3] & 0x8000 else 1,)
    if any(v is None for v in out):
        warn('skin output slot not filled')
        return None, None
    return dict(tex=tex, prim=0x7c, cmode=0, regs=[], verts=out, skin=True), verts


def shadow_batch(b, palette, cache, scale, warn):
    """A shadow-volume part's batch (the type-18 pass, set 5's program 0x178; the VU sees every 16-bit field
    ^ 0x8000, the row offset of the unpack): q0 = (vertex count, strip length, face count, edge-entry count),
    q1 = (influence records, strip start, faces start, edge entries start); vertices at q2, 2 qwords each
    {x, y, z (/1024 x scale), w: 0 = keep the current influence, else take the next record (2 V4-8 qwords
    {4 palette offsets, 4 weights * 127}, as the skin's)}; the strip: per entry .w = a vertex address, bit 15 =
    no triangle ending there (the first two included); faces: (.x, .y, .z) vertex addresses. The edge entries
    ({v0, face, face, v1}: which strip/list triangles share an edge, for the silhouette) are not needed for
    closed prisms. Returns the triangles as a skinned list batch (prim 0x8003: a shadow volume, not drawn
    as a surface), every vertex (pos, normal 0, st 0, rgba, bones, weights, adc 0)."""
    m = b.mem
    def vu(x):
        return (x ^ 0x8000) & 0xffff
    if 0 not in m or 1 not in m:
        return None
    nverts, nstrip, nfaces = vu(m[0][0]), vu(m[0][1]), vu(m[0][2])
    rec, strip0, faces0 = vu(m[1][0]), vu(m[1][1]), vu(m[1][2])
    pscale = scale / 1024.0
    verts, infl = {}, ((0, 0, 0, 0), (1.0, 0.0, 0.0, 0.0))
    for k in range(nverts):
        a = 2 + 2 * k
        q = m.get(a)
        if q is None:
            warn('shadow vertex missing')
            return None
        wv = _s(vu(q[3]))
        if wv > 0:  # 0x12d0: the matrix cached at VU 159 + w (stored by this or an earlier batch)
            if 159 + wv not in cache:
                warn('shadow matrix cache slot never filled')
                return None
            infl = cache[159 + wv]
        elif wv < 0:  # 0xe30: blend the next record; 0x1288 caches it at VU 160 + (-w - 2) when -w - 2 >= 0
            r, w = m.get(rec), m.get(rec + 1)
            rec += 2
            if r is None or w is None:
                warn('shadow influence record missing')
                return None
            bones, ws = [], []
            for j in range(4):
                off = _s(r[j], 8)
                if off < 0:
                    bones.append(0)
                    ws.append(0.0)
                    continue
                bones.append(palette.get(off, 0))
                ws.append(_s(w[j], 8) / 127.0)
            tot = sum(ws) or 1.0
            infl = (tuple(bones), tuple(x / tot for x in ws))
            if -wv - 2 >= 0:
                cache[158 - wv] = infl
        verts[a] = ((_s(q[0]) * pscale, _s(q[1]) * pscale, _s(q[2]) * pscale), infl)
    tris = []
    ring = []
    for e in range(strip0, strip0 + nstrip):
        q = m.get(e)
        if q is None:
            warn('shadow strip entry missing')
            return None
        a = vu(q[3])
        ring = (ring + [a & 0x3ff])[-3:]
        if not a & 0x8000 and len(ring) == 3:
            tris.append(tuple(ring))
    for e in range(faces0, faces0 + nfaces):
        q = m.get(e)
        if q is None:
            warn('shadow face missing')
            return None
        tris.append(tuple(vu(q[j]) & 0x3ff for j in range(3)))
    out = []
    for t in tris:
        if any(a not in verts for a in t):
            warn('shadow triangle names no vertex')
            return None
        for a in t:
            pos, (bones, ws) = verts[a]
            out.append(pos + (0.0, 0.0, 0.0, 0.0, 0.0, 0x80808080) + bones + ws + (0,))
    SKIN_STATS['shadow triangles'] += len(tris)
    return dict(tex=0, prim=0x8003, cmode=0, regs=[], verts=out, skin=True)


def skin_sections(p, n, warn):
    """[(section index, [batches])] of a type-17 geometry (header: see scbm_summary)."""
    B = n['geo']
    if p[B + 8:B + 12] != b'ENOB':
        warn('type 17 without BONE header')
        return []
    scale = struct.unpack_from('<f', p, B + 0x18)[0]
    cnt, toff = struct.unpack_from('<II', p, B + 0x20)
    res = []
    for si, o in enumerate(struct.unpack_from('<%dI' % cnt, p, B + toff)):
        if p[B + o] != 1:
            continue
        nmat, arr, blen, woff, vlen, voff = struct.unpack_from('<6I', p, B + o + 4)
        names = [p[B + so:B + so + 16].split(bytes(1))[0] for so in struct.unpack_from('<%dI' % nmat, p, B + arr)]
        ids = [name_id(x) if NAME_HASH else 0 for x in names]
        starts, q = [], 0
        for c in skin_chunks(p, B + woff, B + woff + blen):
            starts.append((B + voff + q * 16, c))
            q += c[0]
        palette, cache, prev, mat, k, batches = {}, {}, None, None, 0, []
        shadow_cache = {}
        for b, _ in vif_batches(p, B + voff, B + voff + vlen, warn):
            while k < len(starts) and starts[k][0] < b.end:
                _, (_, m_, ents) = starts[k]
                if m_ is not None:
                    mat = m_
                for idx, slot in ents:
                    if idx > 255:
                        warn('skeleton bone index > 255')
                    palette[slot] = idx
                    SKIN_STATS['max bone'] = max(SKIN_STATS['max bone'], idx)
                k += 1
            tex = ids[mat] if mat is not None and mat < len(ids) else 0
            if 0 in b.mem and b.mem[0][0] & 0x8000:  # the shadow-volume layout (raw bit 15: VU count < 0x8000)
                bt = shadow_batch(b, palette, shadow_cache, scale, warn)
                if bt:
                    batches.append(bt)
                continue
            bt, prev = skin_batch(b, palette, cache, prev, scale, tex, warn)
            if bt:
                batches.append(bt)
        res.append((si, batches))
    return res


def scbm_summary(p, n):
    """Structure of a type-17 (WeightedSkin) geometry, as ShaderObjectCommon 0x21cfd0 relocates it.

    B = geometry: +8 'BONE' (bytes 'ENOB'), +0x10 version 4, +0x18 f32, +0x20 section count,
    +0x24 offset of the section offset table (offsets relative to B). Section byte 0 = kind:
    1 = mesh part {+4 sub-part count, +8 offset of sub-part offsets (name strings), +0xc ?,
    +0x10 offset of a packed byte stream (bone influences), +0x14 VIF length, +0x18 VIF stream offset,
    +0x1c ?}; 3 = names {+4 offset (< 129) of 16-byte names}. The VIF stream per MSCNT: V4-8 header
    block, V4-16 with STMOD 1 (index/flag words), V4-16 with STCYCL 3/2 (vertex data), drawn by the
    8df1e059 programs 0x10/0x190/0x1f8 with an EE-built bone palette (0x225ba0)."""
    B = n['geo']
    if p[B + 8:B + 12] != b'ENOB':
        return ['  node %d: no BONE header' % n['num']]
    cnt, toff = struct.unpack_from('<II', p, B + 0x20)
    out = ['  node %d rec 0x%x: %d sections, f32 %g' % (n['num'], n['rec'], cnt, struct.unpack_from('<f', p, B + 0x18)[0])]
    for o in struct.unpack_from('<%dI' % cnt, p, B + toff):
        kind = p[B + o]
        if kind == 1:
            nsub, arr, x0c, woff, vlen, voff, x1c = struct.unpack_from('<7I', p, B + o + 4)
            names = []
            for so in struct.unpack_from('<%dI' % nsub, p, B + arr):
                names.append(p[B + so:B + so + 32].split(b'\0')[0].decode('latin1'))
            sigs = Counter()
            nb = 0
            for b, _ in vif_batches(p, B + voff, min(B + voff + vlen, n['geo_end']), lambda m: None):
                nb += 1
                us = sorted({id(u): u for u in b.writer.values()}.values(), key=lambda u: u.uid)
                sigs[' '.join(UNPACK_NAME[u.vt] for u in us)] += 1
            out.append('    part @0x%x: subparts %s, weights @0x%x, VIF @0x%x len 0x%x, %d batches, %s' % (
                o, names, woff, voff, vlen, nb, '; '.join('%dx %s' % (c, k) for k, c in sigs.most_common(3))))
        else:
            out.append('    section @0x%x kind %d' % (o, kind))
    return out


def pack_resource(rtype, idx, payload_size, nodes):
    out = bytearray(b'TTAM' + struct.pack('<II', VERSION, len(nodes)) + rtype.encode('latin1') +
                    struct.pack('<II', idx, payload_size))
    for n in nodes:
        nv = sum(len(bt['verts']) for bt in n['batches'])
        out += struct.pack('<IbBHIII', n['rec'], n['stype'], n['kind'], n['num'], len(n['batches']), nv,
                           0xffffffff if n['master'] is None else n['master'])
        if n['stype'] == 21:
            if n['t21']:
                m, inst, grp, w4c, cols = n['t21']
            else:
                m, inst, grp, w4c, cols = (0.0,) * 16, 0, 0, 0, (0,) * 256
            out += struct.pack('<16fHHI256I', *m, inst, grp, w4c, *cols)
        if n['kind'] == 3:
            out += struct.pack('<I', len(n['sections']))
            for si, bts in n['sections']:
                out += struct.pack('<II', si, len(bts))
                for bt in bts:
                    pack_batch(out, bt)
            continue
        for bt in n['batches']:
            pack_batch(out, bt)
    return bytes(out)


def pack_batch(out, bt):
    out += struct.pack('<QHBBI', bt['tex'], bt['prim'], bt['cmode'], len(bt['regs']), len(bt['verts']))
    for r, v in bt['regs']:
        out += struct.pack('<B7xQ', r, v)
    if bt.get('skin'):
        for v in bt['verts']:
            out += struct.pack('<8fI4B4fB7x', *v)
    else:
        for v in bt['verts']:
            out += struct.pack('<5fIB3x', *v)


# ----------------------------------------------------------------------------------------
# debug output


def triangles(bt):
    """(i, j, k) of the batch's drawn triangles (strip/list/fan with ADC)."""
    vs, typ = bt['verts'], bt['prim'] & 7
    tris = []
    for i in range(2, len(vs)):
        if vs[i][-1]:
            continue
        if typ == 3:
            if i % 3 == 2:
                tris.append((i - 2, i - 1, i))
        elif typ == 5:
            tris.append((0, i - 1, i))
        else:
            tris.append((i - 2, i - 1, i) if i % 2 == 0 else (i - 1, i - 2, i))
    return tris


def node_world(nodes, n):
    """(batches, transform) to draw node n: its own or its master's batches; type 21 matrix."""
    src = nodes[n['master']] if n['kind'] == 1 else n
    mat = n['t21'][0] if n['t21'] else None
    return src['batches'], mat


def xform(m, v):
    if m is None:
        return v[0], v[1], v[2]
    x, y, z = v[0], v[1], v[2]
    return (x * m[0] + y * m[4] + z * m[8] + m[12], x * m[1] + y * m[5] + z * m[9] + m[13],
            x * m[2] + y * m[6] + z * m[10] + m[14])


def write_obj(path, nodes):
    with open(path, 'w', newline='\n') as f:
        base = 1
        for n in nodes:
            batches, m = node_world(nodes, n)
            for bi, bt in enumerate(batches):
                f.write('g node%d_b%d tex_%016x\n' % (n['num'], bi, bt['tex']))
                for v in bt['verts']:
                    x, y, z = xform(m, v)
                    st = (v[6], v[7]) if bt.get('skin') else (v[3], v[4])
                    f.write('v %.4f %.4f %.4f\nvt %.5f %.5f\n' % (x, y, z, st[0], 1.0 - st[1]))
                for i, j, k in triangles(bt):
                    f.write('f %d/%d %d/%d %d/%d\n' % (base + i, base + i, base + j, base + j, base + k, base + k))
                base += len(bt['verts'])


def write_png(path, nodes, size=1024):
    """Top-down (x, z) render of the triangles, shaded by height (y). Needs Pillow."""
    from PIL import Image, ImageDraw
    tris = []
    for n in nodes:
        batches, m = node_world(nodes, n)
        for bt in batches:
            pts = [xform(m, v) for v in bt['verts']]
            tris += [(pts[i], pts[j], pts[k]) for i, j, k in triangles(bt)]
    if not tris:
        return False
    xs = [p[0] for t in tris for p in t]
    zs = [p[2] for t in tris for p in t]
    ys = [p[1] for t in tris for p in t]
    x0, x1, z0, z1, y0, y1 = min(xs), max(xs), min(zs), max(zs), min(ys), max(ys)
    sc = (size - 8) / max(x1 - x0, z1 - z0, 1e-6)
    im = Image.new('RGB', (size, size), (0, 0, 40))
    dr = ImageDraw.Draw(im)
    for t in sorted(tris, key=lambda t: t[0][1] + t[1][1] + t[2][1]):
        h = (sum(p[1] for p in t) / 3 - y0) / max(y1 - y0, 1e-6)
        c = (int(40 + 200 * h), int(90 + 150 * h), int(40 + 60 * h))
        dr.polygon([(4 + (p[0] - x0) * sc, 4 + (p[2] - z0) * sc) for p in t], fill=c)
    im.save(path)
    return True


# ----------------------------------------------------------------------------------------
# driver


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('game')
    ap.add_argument('out')
    ap.add_argument('--only', nargs='*', help='only .SCP files whose path contains one of these')
    ap.add_argument('--obj', nargs='*', default=[],
                    help='also write <out>/obj/<name>.obj for output names containing one of these')
    ap.add_argument('--png', action='store_true', help='with --obj: also a top-down PNG (needs Pillow)')
    ap.add_argument('--textures', help='texture index.txt to check the batch texture ids against '
                    '(default: <game>/../build/bozo/textures/index.txt if present)')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    load_name_hash(args.game)
    t0 = time.time()
    seen = {}                      # (type, idx) -> {sha1: name}
    index, failures, warnings = [], [], Counter()
    stats = defaultdict(Counter)
    tex_ids = Counter()
    scbm_lines = []
    total = 0
    for scp in tex.find_scps(args.game):
        if not scp.upper().endswith('.SCP'):
            continue
        if args.only and not any(s.lower() in scp.replace('\\', '/').lower() for s in args.only):
            continue
        rel = os.path.relpath(scp, args.game).replace('\\', '/')
        with open(scp, 'rb') as f:
            data = f.read()
        nres = 0
        for r in tex.scp_resources(data):
            if r['type'] not in MESH_TYPES:
                continue
            where = '%s/(%s,%d)' % (rel, r['type'], r['idx'])
            try:
                p = tex.resource_payload(r)
            except Exception as ex:  # noqa: BLE001
                failures.append('%s: payload: %s' % (where, ex))
                continue
            nres += 1
            digest = hashlib.sha1(p).digest()
            variants = seen.setdefault((r['type'], r['idx']), {})
            if digest in variants:
                name, lines, _ = variants[digest]
                index += [where + l for l in lines]
                stats[r['type']]['dup resources'] += 1
                continue
            local = Counter()
            try:
                nodes = convert_resource(p, lambda m: local.update([m]))
            except Exception as ex:  # noqa: BLE001
                failures.append('%s: %s' % (where, ex))
                continue
            for m, c in local.items():
                warnings['%s: %s' % (r['type'], m)] += c
            stem = '%s_%d' % (r['type'].strip(), r['idx'])
            name = stem + '.mesh' if not variants else '%s_v%d.mesh' % (stem, len(variants))
            blob = pack_resource(r['type'], r['idx'], len(p), nodes)
            with open(os.path.join(args.out, name), 'wb') as f:
                f.write(blob)
            total += len(blob)
            lines = []
            st = stats[r['type']]
            st['resources'] += 1
            for n in nodes:
                if not n['geo']:
                    continue
                nb = len(n['batches'])
                nv = sum(len(b['verts']) for b in n['batches'])
                st['nodes kind %d' % n['kind']] += 1
                st['shader %d' % n['stype']] += 1
                st['batches'] += nb
                st['vertices'] += nv
                for b in n['batches']:
                    tex_ids[b['tex']] += 1
                lines.append('  %d  0x%x  %d  %d  %d  %d  %s%s' % (
                    len(p), n['rec'], n['stype'], n['num'], nb, nv, name,
                    '' if n['kind'] != 1 else '  ->node%d' % n['master']))
            if r['type'] == 'scbm':
                for n in nodes:
                    if n['geo'] and n['stype'] == 17:
                        try:
                            scbm_lines.append(where)
                            scbm_lines += scbm_summary(p, n)
                        except Exception as ex:  # noqa: BLE001
                            scbm_lines.append('  node %d: %s' % (n['num'], ex))
            variants[digest] = (name, lines, len(p))
            index += [where + l for l in lines]
            if any(s and s.lower() in name.lower() for s in args.obj):
                od = os.path.join(args.out, 'obj')
                os.makedirs(od, exist_ok=True)
                write_obj(os.path.join(od, name[:-5] + '.obj'), nodes)
                if args.png:
                    write_png(os.path.join(od, name[:-5] + '.png'), nodes)
        print('%-28s %4d mesh resources  (%.1fs)' % (rel, nres, time.time() - t0), flush=True)
    keys = Counter(tuple(l.split()[:2]) for l in index)
    with open(os.path.join(args.out, 'index.txt'), 'w', newline='\n') as f:
        f.write('# file/(type,index)  payload_size  record_offset  shader_type  node#  batches  vertices  outfile  '
                '[->master node# for type-21 instances]\n')
        f.write('\n'.join(index) + '\n')
    with open(os.path.join(args.out, 'conflicts.txt'), 'w', newline='\n') as f:
        for (t, i), v in sorted(seen.items()):
            if len(v) > 1:
                f.write('(%s,%d): %d different payloads: %s\n' % (t, i, len(v), ' '.join('%s(size %d)' % (x[0], x[2]) for x in v.values())))
        f.write('\n'.join('FAIL ' + x for x in failures) + '\n')
        f.write('\n'.join('WARN %6d %s' % (c, m) for m, c in warnings.most_common()) + '\n')
    with open(os.path.join(args.out, 'scbm.txt'), 'w', newline='\n') as f:
        f.write('# type 17 (WeightedSkin) geometry structure, not converted to vertices\n')
        f.write('\n'.join(scbm_lines) + '\n')
    tpath = args.textures or os.path.join(args.game, '..', 'build', 'bozo', 'textures', 'index.txt')
    known = None
    if os.path.exists(tpath):
        with open(tpath) as f:
            known = {int(l.split()[0], 16) for l in f if l[:1] != '#' and l.strip()}
    for t, st in sorted(stats.items()):
        print(t, dict(st))
    print('skin:', dict(SKIN_STATS))
    nz = {k: v for k, v in tex_ids.items() if k}
    msg = 'texture ids: %d distinct non-zero, batches without texture %d' % (len(nz), tex_ids.get(0, 0))
    if known is not None:
        miss = [k for k in nz if k not in known]
        msg += ', not in the texture index: %d ids (%d batches)' % (len(miss), sum(nz[k] for k in miss))
    print(msg)
    print('variants: %d keys with >1 payload; failures %d; warnings %d kinds; output %.1f MiB; %.1fs' % (
        sum(1 for v in seen.values() if len(v) > 1), len(failures), len(warnings), total / 1048576.0,
        time.time() - t0))


if __name__ == '__main__':
    main()
