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
  * type 17 (scbm WeightedSkin), type 19 (water): not converted (kind 2); the structure of every
    type-17 geometry is listed in <out>/scbm.txt (see scbm_summary).

Keys: (type, index, record offset) is unique within one .SCP but NOT across the disc: every level
SCP reuses the same 'ter '/'scbm' indices for different payloads. Identical payloads share one file;
differing ones are <type>_<index>_v<n>.mesh (conflicts.txt), so the runtime needs more than (type, index):
(type, index, unpacked payload size, record offset) still collides for 51 nodes (small 'ter '
resources whose packets differ); adding the record's bounding sphere (record +0x58, 16 bytes, read
through runtime node+0) makes every key unique on the disc. index.txt carries the payload size.

Output: one file per resource <type>_<index>[_v<n>].mesh and index.txt. File layout (little endian):
    header : 'TTAM' u32 version (1), u32 node count, char[4] type, u32 index, u32 payload size
    node   : u32 record offset, i8 shader type, u8 kind (0 own batches, 1 type-21 instance of
             another node's batches, 2 not converted), u16 node#, u32 batch count, u32 vertex count,
             u32 master node# (kind 1, else 0xffffffff),
             if shader type 21: 16 f32 matrix, u16 instance#, u16 group, u32 +0x4c word, 256 u32 colours
             then the batches
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
VERSION = 1

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
            if cl != 1 or wl != 1:
                warn('STCYCL %d/%d' % (cl, wl))
            usn, masked = bool(imm & 0x4000), bool(cmd & 0x10)
            u = Unpack(uid, vt, imm & 0x3ff, n, usn, masked)
            uid += 1
            for i, v in enumerate(_elements(vt, usn, p[a:a + size], cnt)):
                addr = (u.addr + i) & 0x3ff
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
        elif st in (17, 19):
            pass  # WeightedSkin / water: not a plain VIF stream here
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
        for bt in n['batches']:
            out += struct.pack('<QHBBI', bt['tex'], bt['prim'], bt['cmode'], len(bt['regs']), len(bt['verts']))
            for r, v in bt['regs']:
                out += struct.pack('<B7xQ', r, v)
            for v in bt['verts']:
                out += struct.pack('<5fIB3x', *v)
    return bytes(out)


# ----------------------------------------------------------------------------------------
# debug output


def triangles(bt):
    """(i, j, k) of the batch's drawn triangles (strip/list/fan with ADC)."""
    vs, typ = bt['verts'], bt['prim'] & 7
    tris = []
    for i in range(2, len(vs)):
        if vs[i][6]:
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
                    f.write('v %.4f %.4f %.4f\nvt %.5f %.5f\n' % (x, y, z, v[3], 1.0 - v[4]))
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
