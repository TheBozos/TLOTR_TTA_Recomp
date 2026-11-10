#!/usr/bin/env python3
"""Offline texture converter for The Lord of the Rings: The Third Age (SLUS-21027).

Reads the user's extracted disc (never committed) and writes every texture of every
texture group ('txfe', 'txfs', 'txs2', 'txs3', 'txs4', 'txfg', 'txfz', 'txfi', 'txfx')
as linear RGBA, keyed by the game's 64-bit texture id (texture record +0x00).

    python tools/tta_textures.py <game dir> <out dir> [--sheet] [--only SUBSTR ...]

All GS-layout work (writing the group's pixel block into a model of GS local memory as
PSMCT32, then reading each texture back with its own PSM / TBP0 / TBW / CLUT) happens
here, offline, so the runtime never sees a GS transfer block.

Container (.SCP): a stream of little-endian chunks whose 4CCs are stored as u32
(so 'SHOC' reads "COHS" on disc):
    CTRL  16-byte header, no payload walked (its size field covers the SYNC after it)
    SYNC  8 bytes
    SHOC / SONO  {tag, u32 size incl. header, 8 bytes 0} + one sub-chunk:
        SHDR  u16 version, u16 flags, u32 type 4CC, u32 index, u32 unpacked size  -> starts a resource
        SDAT  4CC + 0x2c bytes (uninitialised) + raw payload bytes
        Ldat  4CC + u32 compressed length + zlib bytes. Consecutive Ldat chunks of one resource
              are ONE zlib stream split into pieces (a new stream starts after the previous ends).
    STOC, SWVR, PADD, FILL  skipped by their size; a FILL with only 4 bytes left before a
        0x800 sector boundary has no size and pads to the boundary.

Texture group payload ('TXF ' container, 4CCs in reading order, u32 sizes):
    'TXF ' u32 0x212
    'HEAD' 8:  u16 blocks (image size, 256-byte units), u16 n_tex, u16 n_clut, u8 w/64, u8 h/32
    'LOCA' 0x18: zero on disc (the runtime's pointers: tex, clut, pixels, VRAM base, ...)
    'TXHE' n_tex*0x50: texture records (payload +0x40 = group + 0x40 in memory)
    'CLHE' n_clut*0xc: CLUT records
    'TXDA' blocks*256: the pixel block, uploaded as one PSMCT32 image (w x h, DBW = w/64)

Output file <id:016x>.rgba: 'TTAX', u16 width, u16 height, u32 flags (bit0: some alpha
!= 0x80), u32 reserved (0), then width*height RGBA bytes (R first). Alpha is the GS
value as stored (0x80 = opaque, up to 0xff possible); the runtime rescales. Level 0 only
(records carry up to 4 mip levels at MIPTBP1/2; they are not exported).

The same id appears in many groups. Identical pixels share one file. When pixels differ
(another palette quantisation, an alpha-masked colour, or a lower-resolution LOD group),
the first occurrence is <id>.rgba and later variants are <id>_v<n>.rgba; index.txt
(one line per group record, last column = output file) gives the (group, record) lookup,
conflicts.txt lists every differing variant.
"""
import argparse
import glob
import hashlib
import os
import struct
import sys
import time
import zlib
from operator import itemgetter

TEX_TYPES = ('txfe', 'txfs', 'txs2', 'txs3', 'txs4', 'txfg', 'txfz', 'txfi', 'txfx', 'elf')

# ----------------------------------------------------------------------------------------
# .SCP chunk stream


def _t4(b):
    return b[::-1].decode('latin1')


def scp_resources(data):
    """Yield dicts {type, idx, usz, ver, flags, off, parts=[(kind, bytes)]} in file order."""
    o, n, cur = 0, len(data), None
    while o + 8 <= n:
        tag = _t4(data[o:o + 4])
        sz = struct.unpack_from('<I', data, o + 4)[0]
        if tag == 'CTRL':
            o += 16
            continue
        if tag == 'SYNC':
            o += 8
            continue
        if tag == 'FILL' and (sz < 8 or o + sz > n):
            o = (o + 4 + 0x7ff) & ~0x7ff
            continue
        if sz < 8 or o + sz > n:
            raise ValueError('bad chunk %r at 0x%x (size 0x%x)' % (tag, o, sz))
        if tag in ('SHOC', 'SONO'):
            sub = _t4(data[o + 16:o + 20])
            if sub == 'SHDR':
                if cur:
                    yield cur
                ver, fl = struct.unpack_from('<HH', data, o + 20)
                typ = _t4(data[o + 24:o + 28])
                idx, usz = struct.unpack_from('<II', data, o + 28)
                cur = dict(type=typ, idx=idx, usz=usz, ver=ver, flags=fl, off=o, parts=[])
            elif cur is not None:
                if sub == 'SDAT':
                    cur['parts'].append(('S', data[o + 0x40:o + sz]))
                elif sub == 'Ldat':
                    cl = struct.unpack_from('<I', data, o + 20)[0]
                    cur['parts'].append(('L', data[o + 24:o + 24 + cl]))
        o += sz
    if cur:
        yield cur


def resource_payload(r):
    out, z = bytearray(), None
    for kind, p in r['parts']:
        if kind == 'L':
            if z is None or z.eof:
                z = zlib.decompressobj()
            out += z.decompress(p)
        else:
            out += p
    if len(out) < r['usz']:
        raise ValueError('payload 0x%x < unpacked size 0x%x' % (len(out), r['usz']))
    return bytes(out[:r['usz']])


# ----------------------------------------------------------------------------------------
# GS local memory model (layouts per the GS User's Manual: pages of 32 blocks of 256 bytes,
# blocks of 4 columns of 64 bytes). Written from the documented arrangement.

# PSMCT32: page 64x32, block 8x8; PSMT8 uses the same block arrangement (page 128x64, block 16x16)
BLOCK32 = ((0, 1, 4, 5, 16, 17, 20, 21),
           (2, 3, 6, 7, 18, 19, 22, 23),
           (8, 9, 12, 13, 24, 25, 28, 29),
           (10, 11, 14, 15, 26, 27, 30, 31))
# PSMCT16: page 64x64, block 16x8; PSMT4 uses the same arrangement (page 128x128, block 32x16)
BLOCK16 = ((0, 2, 8, 10), (1, 3, 9, 11), (4, 6, 12, 14), (5, 7, 13, 15),
           (16, 18, 24, 26), (17, 19, 25, 27), (20, 22, 28, 30), (21, 23, 29, 31))
BLOCK16S = ((0, 2, 16, 18), (1, 3, 17, 19), (8, 10, 24, 26), (9, 11, 25, 27),
            (4, 6, 20, 22), (5, 7, 21, 23), (12, 14, 28, 30), (13, 15, 29, 31))
# word order inside a PSMCT32 column (8x2 pixels)
COL32 = ((0, 1, 4, 5, 8, 9, 12, 13), (2, 3, 6, 7, 10, 11, 14, 15))

# (page w, page h, block w, block h, block table, units per byte) ; units: 1 = byte, 2 = nibble
# Addresses below are in "units": words for CT32, halfwords for CT16, bytes for T8, nibbles for T4.


def _in_page_ct32(x, y):  # word index inside a page (2048 words)
    blk = BLOCK32[y >> 3][x >> 3]
    return blk * 64 + ((y & 7) >> 1) * 16 + COL32[y & 1][x & 7]


def _in_page_ct16(x, y, table):  # halfword index inside a page (4096)
    blk = table[y >> 3][x >> 4]
    w = COL32[y & 1][x & 7]
    return blk * 128 + ((y & 7) >> 1) * 32 + w * 2 + ((x >> 3) & 1)


def _in_page_t8(x, y):  # byte index inside a page (8192)
    blk = BLOCK32[y >> 4][x >> 4]
    col = (y & 15) >> 2
    swap = ((y >> 1) & 1) ^ ((y >> 2) & 1)
    w = COL32[y & 1][((x & 7) + 4 * swap) & 7]
    k = ((y >> 1) & 1) + ((x >> 3) & 1) * 2
    return blk * 256 + col * 64 + w * 4 + k


def _in_page_t4(x, y):  # nibble index inside a page (16384)
    blk = BLOCK16[y >> 4][x >> 5]
    col = (y & 15) >> 2
    swap = ((y >> 1) & 1) ^ ((y >> 2) & 1)
    w = COL32[y & 1][((x & 7) + 4 * swap) & 7]
    k = ((y >> 1) & 1) + ((x >> 3) & 3) * 2
    return blk * 512 + col * 128 + w * 8 + k


# psm -> (page w, page h, unit fn, units per page, unit kind)
PSM_INFO = {
    0x00: (64, 32, _in_page_ct32, 'ct32'),
    0x01: (64, 32, _in_page_ct32, 'ct24'),
    0x1b: (64, 32, _in_page_ct32, 't8h'),
    0x24: (64, 32, _in_page_ct32, 't4hl'),
    0x2c: (64, 32, _in_page_ct32, 't4hh'),
    0x02: (64, 64, lambda x, y: _in_page_ct16(x, y, BLOCK16), 'ct16'),
    0x0a: (64, 64, lambda x, y: _in_page_ct16(x, y, BLOCK16S), 'ct16'),
    0x13: (128, 64, _in_page_t8, 't8'),
    0x14: (128, 128, _in_page_t4, 't4'),
}
PSM_NAME = {0x00: 'CT32', 0x01: 'CT24', 0x02: 'CT16', 0x0a: 'CT16S', 0x13: 'T8', 0x14: 'T4',
            0x1b: 'T8H', 0x24: 'T4HL', 0x2c: 'T4HH'}

_tile_cache = {}


def _tile(psm, w, h):
    """Row-major unit indices (relative to the page start) for a w x h crop of one page."""
    key = (psm, w, h)
    t = _tile_cache.get(key)
    if t is None:
        fn = PSM_INFO[psm][2]
        t = [fn(x, y) for y in range(h) for x in range(w)]
        _tile_cache[key] = t
    return t


class GSMem:
    """GS local memory holding one group's pixel block, written as PSMCT32 at DBP 0."""

    def __init__(self, pixels, width, height):
        dbw = width // 64
        pages_x, pages_y = width // 64, height // 32
        npages = pages_x * pages_y
        self.words = [0] * (npages * 2048 + 2048 * 4)  # slack for reads past the end
        src = memoryview(pixels).cast('I') if len(pixels) % 4 == 0 else None
        perm = _tile(0, 64, 32)
        for py in range(pages_y):
            for px in range(pages_x):
                page_base = (py * dbw + px) * 2048
                # gather the page's 64x32 source rectangle row by row
                rows = []
                for yy in range(32):
                    s = ((py * 32 + yy) * width + px * 64)
                    rows.extend(src[s:s + 64])
                for i, v in zip(perm, rows):
                    self.words[page_base + i] = v
        self.nwords = len(self.words)
        self._bytes = None
        self._nibbles = None

    def bytes(self):
        if self._bytes is None:
            import array
            a = array.array('I', self.words)
            if sys.byteorder != 'little':
                a.byteswap()
            self._bytes = a.tobytes()
        return self._bytes

    def nibbles(self):
        if self._nibbles is None:
            b = self.bytes()
            lo = b.translate(bytes(i & 15 for i in range(256)))
            hi = b.translate(bytes(i >> 4 for i in range(256)))
            n = bytearray(len(b) * 2)
            n[0::2] = lo
            n[1::2] = hi
            self._nibbles = bytes(n)
        return self._nibbles

    def read_units(self, psm, tbp, tbw, w, h):
        """Return a flat list (row-major) of unit values for a w x h image at tbp/tbw."""
        pw, ph, _, kind = PSM_INFO[psm]
        if kind in ('ct32', 'ct24', 't8h', 't4hl', 't4hh'):
            src, upp, ubp = self.words, 2048, 64        # units per page / per block
        elif kind == 'ct16':
            src, upp, ubp = None, 4096, 128
        elif kind == 't8':
            src, upp, ubp = self.bytes(), 8192, 256
        else:
            src, upp, ubp = self.nibbles(), 16384, 512
        if kind == 'ct16':
            b = self.bytes()
            src = memoryview(b).cast('H')
        pages_per_row = max(1, tbw * 64 // pw)
        out = [None] * (w * h)
        tw, th = min(w, pw), min(h, ph)
        tile = _tile(psm, tw, th)
        limit = len(src)
        for pr in range((h + ph - 1) // ph):
            for pc in range((w + pw - 1) // pw):
                base = (tbp + (pr * pages_per_row + pc) * 32) * ubp
                if base + upp > limit:
                    vals = [src[(base + i) % limit] for i in tile]
                else:
                    vals = itemgetter(*tile)(src[base:base + upp]) if len(tile) > 1 else (src[base + tile[0]],)
                for y in range(th):
                    d = (pr * ph + y) * w + pc * pw
                    out[d:d + tw] = vals[y * tw:(y + 1) * tw]
        return out, kind


def _ct32_rgba(v):
    return struct.pack('<I', v)


def read_clut(mem, cbp, n_entries, csa=0):
    """CSM1 CT32 CLUT -> list of 4-byte RGBA entries (index order)."""
    if n_entries == 256:
        img, _ = mem.read_units(0x00, cbp, 1, 16, 16)
        pal = []
        for i in range(256):
            p = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)
            pal.append(struct.pack('<I', img[p]))
        return pal
    # 16 entries: an 8x2 CT32 rectangle at CBP (CSA is 0 in every group on the disc)
    if csa:
        raise ValueError('CSA %d not supported' % csa)
    img, _ = mem.read_units(0x00, cbp, 1, 8, 2)
    return [struct.pack('<I', v) for v in img]


# ----------------------------------------------------------------------------------------
# texture groups


def parse_group(p):
    if p[:4] != b'TXF ':
        raise ValueError('not a TXF group: %r' % p[:4])
    o = 8
    ch = {}
    while o + 8 <= len(p):
        tag = p[o:o + 4].decode('latin1')
        sz = struct.unpack_from('<I', p, o + 4)[0]
        ch[tag] = (o + 8, sz)
        o += 8 + sz
        if tag == 'TXDA':
            break
    ho, _ = ch['HEAD']
    blocks, ntex, nclut, bw64, bh32 = struct.unpack_from('<HHHBB', p, ho)
    to, tsz = ch['TXHE']
    co, csz = ch['CLHE']
    do, dsz = ch['TXDA']
    assert tsz >= ntex * 0x50 and csz >= nclut * 0xc, 'record table sizes'  # chunks padded to 16
    assert dsz == blocks * 256 == bw64 * 64 * bh32 * 32 * 4, 'pixel block size'
    recs = []
    for i in range(ntex):
        r = p[to + i * 0x50:to + (i + 1) * 0x50]
        tid = struct.unpack_from('<Q', r, 0)[0]
        tbp, tbw = struct.unpack_from('<HH', r, 0x10)
        psm, mips, _, lw, lh = r[0x3c], r[0x3d], r[0x3e], r[0x3f], r[0x40]
        clut = struct.unpack_from('<H', r, 0x46)[0]
        recs.append(dict(id=tid, tbp=tbp, tbw=tbw, psm=psm, mips=mips, lw=lw, lh=lh, clut=clut, raw=r))
    cluts = []
    for i in range(nclut):
        c = struct.unpack_from('<6H', p, co + i * 12)
        cluts.append(dict(qwc=c[2], cbp=c[3], n=c[4], csa=c[5], raw=c))
    return dict(blocks=blocks, w=bw64 * 64, h=bh32 * 32, recs=recs, cluts=cluts,
                pixels=p[do:do + dsz])


def decode_texture(mem, rec, cluts):
    w, h = 1 << rec['lw'], 1 << rec['lh']
    psm = rec['psm']
    if psm not in PSM_INFO:
        raise ValueError('unsupported PSM 0x%x' % psm)
    vals, kind = mem.read_units(psm, rec['tbp'], rec['tbw'], w, h)
    if kind in ('ct32', 'ct24'):
        import array
        a = array.array('I', vals)
        if sys.byteorder != 'little':
            a.byteswap()
        rgba = a.tobytes()
        if kind == 'ct24':
            m = bytearray(rgba)
            m[3::4] = b'\x80' * (w * h)
            rgba = bytes(m)
        return w, h, rgba, None
    if kind == 'ct16':
        out = bytearray()
        for v in vals:
            out += bytes(((v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3,
                          0x80 if v & 0x8000 else 0))
        return w, h, bytes(out), None
    if kind == 't8h':
        idx = [v >> 24 for v in vals]
    elif kind == 't4hl':
        idx = [(v >> 24) & 15 for v in vals]
    elif kind == 't4hh':
        idx = [v >> 28 for v in vals]
    else:
        idx = vals
    if rec['clut'] == 0xffff:
        raise ValueError('indexed texture without CLUT')
    c = cluts[rec['clut']]
    n = 256 if kind in ('t8', 't8h') else 16
    pal = read_clut(mem, c['cbp'], n, c['csa'])
    return w, h, b''.join(map(pal.__getitem__, idx)), c


# ----------------------------------------------------------------------------------------
# driver


def write_rgba(path, w, h, rgba):
    flags = 1 if rgba[3::4].strip(b'\x80') else 0
    with open(path, 'wb') as f:
        f.write(b'TTAX' + struct.pack('<HHII', w, h, flags, 0))
        f.write(rgba)


def _difference(out, tid, w, h, rgba):
    """Describe how a colliding texture differs from the one already written for its id."""
    with open(os.path.join(out, '%016x.rgba' % tid), 'rb') as f:
        first = f.read()
    fw, fh = struct.unpack_from('<HH', first, 4)
    if (fw, fh) != (w, h):
        return 'size %dx%d vs %dx%d' % (fw, fh, w, h)
    a, b = first[16:], rgba
    step = max(1, len(a) // 4096) * 4
    diffs = [abs(a[j + k] - b[j + k]) for j in range(0, len(a), step) if a[j + 3] or b[j + 3] for k in range(4)]
    return 'same size, mean |diff| %.1f per channel over visible pixels (sampled)' % (
        sum(diffs) / max(1, len(diffs)))


TXF_MAGIC = b'TXF ' + struct.pack('<I', 0x212)


def find_groups(payload):
    """Yield (offset, bytes) of every 'TXF ' container inside a payload (any alignment)."""
    o = payload.find(TXF_MAGIC)
    while o >= 0:
        e = o + 8
        ok = False
        while e + 8 <= len(payload):
            tag = payload[e:e + 4]
            e += 8 + struct.unpack_from('<I', payload, e + 4)[0]
            if tag == b'TXDA':
                ok = e <= len(payload)
                break
        if ok:
            yield o, payload[o:e]
            o = payload.find(TXF_MAGIC, e)
        else:
            o = payload.find(TXF_MAGIC, o + 4)


def find_scps(game):
    files = sorted(glob.glob(os.path.join(game, 'SLUS_*')))
    for pat in ('DATA/LOADONCE.SCP', 'DATA/GAME/*.SCP', 'DATA/GAME/*/*.SCP'):
        files += sorted(glob.glob(os.path.join(game, pat)))
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('game')
    ap.add_argument('out')
    ap.add_argument('--only', nargs='*', help='only .SCP files whose path contains one of these')
    ap.add_argument('--sheet', action='store_true', help='write PNG contact sheets (needs Pillow)')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    t0 = time.time()
    seen = {}            # id -> (sha1, where)
    conflicts = []
    index_lines = []
    stats = {}
    failures = []
    sheet_items = []
    total_bytes = 0
    for scp in find_scps(args.game):
        if args.only and not any(s.lower() in scp.replace('\\', '/').lower() for s in args.only):
            continue
        rel = os.path.relpath(scp, args.game).replace('\\', '/')
        with open(scp, 'rb') as f:
            data = f.read()
        nres = 0
        if scp.upper().endswith('.SCP'):
            res = scp_resources(data)
        else:  # the executable: one payload, the whole file
            res = [dict(type='elf', idx=0, usz=len(data), parts=[('S', data)])]
        for r in res:
            try:
                payload = resource_payload(r)
            except Exception as e:  # noqa: BLE001
                failures.append('%s/(%s,%d): payload: %s' % (rel, r['type'], r['idx'], e))
                continue
            for goff, gbytes in find_groups(payload):
                nres += 1
                st = stats.setdefault(r['type'], [0, 0, 0])
                st[0] += 1
                where = '%s/(%s,%d)  0x%x' % (rel, r['type'], r['idx'], goff)
                if goff & 15:
                    where += '!unaligned'
                try:
                    g = parse_group(gbytes)
                    mem = GSMem(g['pixels'], g['w'], g['h'])
                except Exception as e:  # noqa: BLE001
                    failures.append('%s: %s' % (where, e))
                    continue
                for i, rec in enumerate(g['recs']):
                    try:
                        w, h, rgba, c = decode_texture(mem, rec, g['cluts'])
                    except Exception as e:  # noqa: BLE001
                        failures.append('%s rec %d: %s' % (where, i, e))
                        st[2] += 1
                        continue
                    st[1] += 1
                    digest = hashlib.sha1(rgba).digest()
                    ctxt = '-' if c is None else '(%d,CT32,%d)' % (c['cbp'], c['csa'])
                    line = '%016x  %s  %d  %s  %d  %d  %d  %d  %s' % (
                        rec['id'], where, i, PSM_NAME.get(rec['psm'], hex(rec['psm'])), w, h,
                        rec['tbp'], rec['tbw'], ctxt)
                    # seen: id -> {sha1: (file name, first location)}; the first variant is <id>.rgba,
                    # differing variants (other quantisation / LOD of the same id) are <id>_v<n>.rgba
                    variants = seen.setdefault(rec['id'], {})
                    if digest in variants:
                        index_lines.append(line + '  ' + variants[digest][0])
                        continue
                    name = '%016x.rgba' % rec['id'] if not variants else '%016x_v%d.rgba' % (rec['id'], len(variants))
                    if variants:
                        first = next(iter(variants.values()))
                        conflicts.append('%016x  first %s  differs in %s rec %d (-> %s): %s' % (
                            rec['id'], first[1], where, i, name, _difference(args.out, rec['id'], w, h, rgba)))
                    variants[digest] = (name, '%s rec %d' % (where, i))
                    index_lines.append(line + '  ' + name)
                    write_rgba(os.path.join(args.out, name), w, h, rgba)
                    total_bytes += 16 + len(rgba)
                    if args.sheet and len(variants) == 1:
                        sheet_items.append(('%016x' % rec['id'], r['type'], w, h, rgba))
        print('%-28s %5d texture groups  (%.1fs)' % (rel, nres, time.time() - t0), flush=True)
    with open(os.path.join(args.out, 'index.txt'), 'w', newline='\n') as f:
        f.write('# id  file/(resource type,index)  group offset in payload  record#  psm  w  h  tbp0_rel  tbw  clut(cbp_rel,cpsm,csa)\n')
        f.write('\n'.join(index_lines) + '\n')
    with open(os.path.join(args.out, 'conflicts.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(conflicts + ['FAIL ' + x for x in failures]) + '\n')
    print('types:', {k: 'groups %d, textures %d, failed %d' % tuple(v) for k, v in sorted(stats.items())})
    print('unique ids %d, id conflicts %d, failures %d, output %.1f MiB, %.1fs' % (
        len(seen), len(conflicts), len(failures), total_bytes / 1048576.0, time.time() - t0))
    if args.sheet and sheet_items:
        write_sheets(args.out, sheet_items)


def write_sheets(out, items, per_sheet=64, cell=128):
    from PIL import Image
    step = max(1, len(items) // (per_sheet * 6))
    picks = items[::step][:per_sheet * 6]
    for s in range(0, len(picks), per_sheet):
        part = picks[s:s + per_sheet]
        cols = 8
        sheet = Image.new('RGB', (cols * cell, ((len(part) + cols - 1) // cols) * cell), (255, 0, 255))
        for i, (_, _, w, h, rgba) in enumerate(part):
            im = Image.frombytes('RGBA', (w, h), rgba)
            a = bytes(min(255, v * 2) for v in range(256))
            im.putalpha(im.getchannel('A').point(list(a)))
            bg = Image.new('RGBA', im.size, (96, 96, 96, 255))
            im = Image.alpha_composite(bg, im).convert('RGB')
            im.thumbnail((cell - 2, cell - 2))
            sheet.paste(im, ((i % cols) * cell + 1, (i // cols) * cell + 1))
        sheet.save(os.path.join(out, 'sheet_%02d.png' % (s // per_sheet)))


if __name__ == '__main__':
    main()
