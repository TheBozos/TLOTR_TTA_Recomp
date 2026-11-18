#!/usr/bin/env python3
"""tta_vu0_check.py <SLUS_210.27> <record file> [--entry 0xdb0] [--trace N]: an independent VU0 micro-mode
interpreter for The Third Age's VU0 microprograms, used to check tools/tta_vu0_gen.py's translation.

Written from the VU instruction-set semantics (it shares no translation code with tta_vu0_gen.py; only the image
offsets are the same facts). Execution model, one pair a cycle (FMAC stalls are not modelled; the generator does
not model them either):
- both halves of a pair read the registers as they were before the pair; results are committed at its end;
- I bit: the lower word is loaded into I; the upper instruction of the same pair still reads the OLD I (an LOI is
  visible from the next pair on). This is what TTA's code relies on: e.g. 0xdd8 loads 0.159218 next to an upper
  that does not use I, and 0xde0's ADDAi consumes it; with "same pair" semantics that constant would be dead;
- Q: DIV / SQRT land 7 cycles after issue, RSQRT 13 (the generator's latencies), or at WAITQ; reads before that
  see the old Q;
- FMEQ / FMAND / FMOR see the MAC flags as they were after the pair 4 pairs earlier;
- a branch's next pair is its delay slot; the E bit ends the program after the next pair.
Arithmetic: float32 with round to nearest for the adder (as BozoEngine's addf), multiplier the same; results with
exponent 255 are clamped to the largest magnitude, denormals flushed to zero. Exactness to the bit is not the goal;
differences above 1e-4 relative are reported.

Record file (written by the port's VU0 recorder): per call u32 entry, then the state before and after, each
vf[32] (4 x u32), acc (4 x u32), vi[16] (u32), u32 q, i, p, r, status, mac, clip, cmsar0 (1252 bytes per record).
"""
import argparse
import struct
import sys

IMAGES = [0x249A84, 0x239D94]  # file offsets of the two VU0 microcode images (VIF MPG packets) in the ELF


def load_code(path):
    d = open(path, 'rb').read()
    mem = {}
    for off in IMAGES:
        p = off
        for _ in range(16):
            w = struct.unpack_from('<I', d, p)[0]
            cmd, num, imm = (w >> 24) & 0x7f, (w >> 16) & 0xff, w & 0xffff
            if cmd == 0x4a:
                p = (p + 4 + 7) & ~7
                for k in range(num or 256):
                    mem[imm + k] = struct.unpack_from('<II', d, p + 8 * k)
                p += (num or 256) * 8
            elif cmd == 0:
                p += 4
            else:
                break
    return mem


# ---- float32 helpers -------------------------------------------------------------------------------------------
MAXF = 0x7fffffff  # PS2 floats have no inf: the largest magnitude


def f(u):
    u &= 0xffffffff
    if (u >> 23) & 0xff == 0:
        return -0.0 if u >> 31 else 0.0
    if (u >> 23) & 0xff == 0xff:  # PS2 has no inf/nan: an exponent-255 value is a large ordinary number
        e = 255 - 127
        m = 1 + (u & 0x7fffff) / 8388608.0
        v = m * 2.0 ** e
        return -v if u >> 31 else v
    return struct.unpack('<f', struct.pack('<I', u))[0]


def u32(x):
    try:
        b = struct.unpack('<I', struct.pack('<f', x))[0]
    except OverflowError:
        return (MAXF | 0x80000000) if x < 0 else MAXF
    if (b >> 23) & 0xff == 0xff:
        return (MAXF | (b & 0x80000000))
    if (b >> 23) & 0xff == 0:
        return b & 0x80000000
    return b


def fadd(a, b):
    return u32(f(a) + f(b))


def fsub(a, b):
    return u32(f(a) - f(b))


def fmul(a, b):
    return u32(f(a) * f(b))


FIELDS = 'xyzw'


class State:
    def __init__(self, vf, acc, vi, q, i, mac):
        self.vf = [list(v) for v in vf]
        self.acc = list(acc)
        self.vi = list(vi)
        self.q, self.i, self.mac = q, i, mac


def dest_fields(w):
    d = (w >> 21) & 15
    return [k for k in range(4) if d & (8 >> k)]


class Pair:
    """Decoded effects of one pair, computed from the state before it."""


def run(mem, entry, st, opts, trace=None, max_pairs=4000):
    vf, vi = st.vf, st.vi
    vf[0] = [0, 0, 0, 0x3f800000]
    q, qpend, qready, qwait = st.q, 0, 0, False
    i_reg = st.i
    mac = st.mac
    machist = [mac] * 4  # MAC after each of the last pairs (ring)
    pc, cycle = entry, 0
    branch_to, delay, end_after = None, None, False
    for _ in range(max_pairs):
        if qwait and cycle >= qready:
            q, qwait = qpend, False
        lo, up = mem[pc // 8]
        mac_seen = machist[cycle & 3]
        wr_vf, wr_vi, wr_acc = [], [], None
        new_mac = None
        new_i = None
        q_issue = None
        waitq = False
        br = None
        ibit, ebit = up >> 31 & 1, up >> 30 & 1
        # ---------------- upper ----------------
        op = up & 0x3f
        ft, fs, fd = (up >> 16) & 31, (up >> 11) & 31, (up >> 6) & 31
        flds = dest_fields(up)

        def bcv(reg, k):
            return [vf[reg][k]] * 4

        def fm(kind, a, b, accv=None, to_acc=False, dreg=None):
            r = list(st_acc_cur) if to_acc else list(vf[dreg])
            m = 0
            for k in flds:
                if kind == 'add':
                    v = fadd(a[k], b[k])
                elif kind == 'sub':
                    v = fsub(a[k], b[k])
                elif kind == 'mul':
                    v = fmul(a[k], b[k])
                elif kind == 'madd':
                    v = fadd(accv[k], fmul(a[k], b[k]))
                elif kind == 'msub':
                    v = fsub(accv[k], fmul(a[k], b[k]))
                r[k] = v
                bit = 3 - k
                if v & 0x7fffffff == 0:
                    m |= 1 << bit
                if v & 0x80000000:
                    m |= 1 << (bit + 4)
            return r, m

        st_acc_cur = st.acc
        accv = st.acc
        iv = [lo if (ibit and opts.get('i_same_pair')) else i_reg] * 4
        qv = [q] * 4
        if op < 0x3c:
            group = None
            if op < 0x1c:
                kind = ['add', 'sub', 'madd', 'msub', 'max', 'mini', 'mul'][op >> 2]
                tv = bcv(ft, op & 3)
            else:
                tab = {0x1c: ('mul', 'q'), 0x1d: ('max', 'i'), 0x1e: ('mul', 'i'), 0x1f: ('mini', 'i'),
                       0x20: ('add', 'q'), 0x21: ('madd', 'q'), 0x22: ('add', 'i'), 0x23: ('madd', 'i'),
                       0x24: ('sub', 'q'), 0x25: ('msub', 'q'), 0x26: ('sub', 'i'), 0x27: ('msub', 'i'),
                       0x28: ('add', 'v'), 0x29: ('madd', 'v'), 0x2a: ('mul', 'v'), 0x2b: ('max', 'v'),
                       0x2c: ('sub', 'v'), 0x2d: ('msub', 'v'), 0x2f: ('mini', 'v')}
                if op == 0x2e:  # OPMSUB
                    s, t = vf[fs], vf[ft]
                    r = list(vf[fd])
                    pr = [fmul(s[1], t[2]), fmul(s[2], t[0]), fmul(s[0], t[1])]
                    m = 0
                    for k in range(3):
                        r[k] = fsub(accv[k], pr[k])
                        if r[k] & 0x7fffffff == 0:
                            m |= 1 << (3 - k)
                        if r[k] & 0x80000000:
                            m |= 1 << (7 - k)
                    wr_vf.append((fd, r))
                    new_mac = m
                    kind = None
                else:
                    kind, src = tab[op]
                    tv = {'q': qv, 'i': iv, 'v': vf[ft]}[src]
            if kind in ('max', 'mini'):
                r = list(vf[fd])
                for k in flds:
                    a, b = f(vf[fs][k]), f(tv[k])
                    r[k] = vf[fs][k] if (a >= b) == (kind == 'max') else tv[k]
                wr_vf.append((fd, r))
            elif kind:
                r, m = fm(kind, vf[fs], tv, accv=accv, dreg=fd)
                wr_vf.append((fd, r))
                new_mac = m
        else:
            sp = ((up >> 4) & 0x7c) | (up & 3)
            if sp < 0x10 or 0x18 <= sp < 0x1c:
                kind = ['add', 'sub', 'madd', 'msub', None, None, 'mul'][sp >> 2]
                r, m = fm(kind, vf[fs], bcv(ft, sp & 3), accv=accv, to_acc=True)
                wr_acc, new_mac = r, m
            elif 0x10 <= sp < 0x18:
                frac = [0, 4, 12, 15][sp & 3]
                r = list(vf[ft])
                for k in flds:
                    if sp < 0x14:
                        x = vf[fs][k]
                        x = x - (1 << 32) if x & 0x80000000 else x
                        r[k] = u32(float(x) / (1 << frac))
                    else:
                        x = f(vf[fs][k]) * (1 << frac)
                        x = max(-2147483648, min(2147483647, int(x)))
                        r[k] = x & 0xffffffff
                wr_vf.append((ft, r))
            elif sp == 0x1d:  # ABS
                r = list(vf[ft])
                for k in flds:
                    r[k] = vf[fs][k] & 0x7fffffff
                wr_vf.append((ft, r))
            elif sp in (0x1c, 0x1e, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2c, 0x2d):
                tab = {0x1c: ('mul', 'q'), 0x1e: ('mul', 'i'), 0x20: ('add', 'q'), 0x21: ('madd', 'q'),
                       0x22: ('add', 'i'), 0x23: ('madd', 'i'), 0x24: ('sub', 'q'), 0x25: ('msub', 'q'),
                       0x26: ('sub', 'i'), 0x27: ('msub', 'i'), 0x28: ('add', 'v'), 0x29: ('madd', 'v'),
                       0x2a: ('mul', 'v'), 0x2c: ('sub', 'v'), 0x2d: ('msub', 'v')}
                kind, src = tab[sp]
                tv = {'q': qv, 'i': iv, 'v': vf[ft]}[src]
                r, m = fm(kind, vf[fs], tv, accv=accv, to_acc=True)
                wr_acc, new_mac = r, m
            elif sp == 0x2e:  # OPMULA
                s, t = vf[fs], vf[ft]
                r = list(st.acc)
                r[0], r[1], r[2] = fmul(s[1], t[2]), fmul(s[2], t[0]), fmul(s[0], t[1])
                m = 0
                for k in range(3):
                    if r[k] & 0x7fffffff == 0:
                        m |= 1 << (3 - k)
                    if r[k] & 0x80000000:
                        m |= 1 << (7 - k)
                wr_acc, new_mac = r, m
            elif sp == 0x2f:
                pass
            else:
                raise ValueError('pc 0x%x: upper special 0x%x' % (pc, sp))
        # ---------------- lower ----------------
        if ibit:
            new_i = lo
        else:
            lop = (lo >> 25) & 0x7f
            it, is_ = (lo >> 16) & 31, (lo >> 11) & 31
            i11 = lo & 0x7ff
            s11 = i11 - 0x800 if i11 & 0x400 else i11
            lfl = dest_fields(lo)
            if lop == 0x40:
                fn = lo & 0x3f
                if fn < 0x3c:
                    idr = (lo >> 6) & 31
                    if fn == 0x30:
                        wr_vi.append((idr, vi[is_] + vi[it]))
                    elif fn == 0x31:
                        wr_vi.append((idr, vi[is_] - vi[it]))
                    elif fn == 0x32:
                        i5 = (lo >> 6) & 31
                        wr_vi.append((it, vi[is_] + (i5 - 32 if i5 & 16 else i5)))
                    elif fn == 0x34:
                        wr_vi.append((idr, vi[is_] & vi[it]))
                    elif fn == 0x35:
                        wr_vi.append((idr, vi[is_] | vi[it]))
                    else:
                        raise ValueError('pc 0x%x: lower 0x40/%x' % (pc, fn))
                else:
                    sp = ((lo >> 4) & 0x7c) | (lo & 3)
                    fsf, ftf = (lo >> 21) & 3, (lo >> 23) & 3
                    if sp == 0x30:  # MOVE
                        r = list(vf[it])
                        for k in lfl:
                            r[k] = vf[is_][k]
                        wr_vf.append((it, r))
                    elif sp == 0x31:  # MR32
                        r = list(vf[it])
                        src = vf[is_][1:] + vf[is_][:1]
                        for k in lfl:
                            r[k] = src[k]
                        wr_vf.append((it, r))
                    elif sp == 0x38:  # DIV
                        b = f(vf[it][ftf])
                        a = f(vf[is_][fsf])
                        q_issue = (u32(a / b) if b else (MAXF | ((vf[is_][fsf] ^ vf[it][ftf]) & 0x80000000)), 7)
                    elif sp == 0x39:  # SQRT
                        q_issue = (u32(abs(f(vf[it][ftf])) ** 0.5), 7)
                    elif sp == 0x3a:  # RSQRT
                        b = abs(f(vf[it][ftf])) ** 0.5
                        a = f(vf[is_][fsf])
                        q_issue = (u32(a / b) if b else (MAXF | (vf[is_][fsf] & 0x80000000)), 13)
                    elif sp == 0x3b:
                        waitq = True
                    elif sp == 0x3c:  # MTIR
                        wr_vi.append((it, vf[is_][fsf] & 0xffff))
                    elif sp == 0x3d:  # MFIR
                        r = list(vf[it])
                        x = vi[is_] & 0xffff
                        x = x - 0x10000 if x & 0x8000 else x
                        for k in lfl:
                            r[k] = x & 0xffffffff
                        wr_vf.append((it, r))
                    else:
                        raise ValueError('pc 0x%x: lower special 0x%x' % (pc, sp))
            elif lop in (0x08, 0x09):
                imm = ((lo >> 10) & 0x7800) | i11
                wr_vi.append((it, vi[is_] + imm if lop == 8 else vi[is_] - imm))
            elif lop in (0x18, 0x1a, 0x1b):  # FMEQ, FMAND, FMOR
                m = mac_seen & 0xffff
                v = {0x18: int(m == (vi[is_] & 0xffff)), 0x1a: vi[is_] & m, 0x1b: vi[is_] | m}[lop]
                wr_vi.append((it, v))
            elif lop == 0x20:
                br = pc + 8 + s11 * 8
            elif lop in (0x28, 0x29):
                c = (vi[it] & 0xffff) == (vi[is_] & 0xffff)
                if c == (lop == 0x28):
                    br = pc + 8 + s11 * 8
            elif lop in (0x2c, 0x2d, 0x2e, 0x2f):
                x = vi[is_] & 0xffff
                x = x - 0x10000 if x & 0x8000 else x
                c = {0x2c: x < 0, 0x2d: x > 0, 0x2e: x <= 0, 0x2f: x >= 0}[lop]
                if c:
                    br = pc + 8 + s11 * 8
            else:
                raise ValueError('pc 0x%x: lower op 0x%x' % (pc, lop))
        if trace is not None:
            trace.append((pc, cycle, list(wr_vf), wr_acc, q, i_reg, mac_seen))
        # ---------------- commit ----------------
        for r, v in wr_vf:
            if r:
                vf[r] = v
        for r, v in wr_vi:
            if r:
                vi[r] = v & 0xffff
        if wr_acc is not None:
            st.acc = wr_acc
        if new_mac is not None:
            mac = new_mac
        if new_i is not None:
            i_reg = new_i
        if q_issue is not None:
            qpend, qready, qwait = q_issue[0], cycle + q_issue[1], True
        if waitq and qwait:
            q, qwait = qpend, False
        machist[cycle & 3] = mac
        cycle += 1
        if end_after:
            break
        if ebit:
            end_after = True
        if delay is not None:
            pc, delay = delay, None
            if br is not None:
                delay = br
            continue
        if br is not None:
            delay = br
        pc += 8
    if qwait:
        q = qpend
    st.q, st.i, st.mac = q, i_reg, mac
    return st


REC = 4 + 2 * (32 * 16 + 16 + 64 + 32)


def parse_state(b, o):
    vf = [list(struct.unpack_from('<4I', b, o + 16 * k)) for k in range(32)]
    o += 512
    acc = list(struct.unpack_from('<4I', b, o))
    o += 16
    vi = list(struct.unpack_from('<16I', b, o))
    o += 64
    q, i, p, r, status, mac, clip, cmsar = struct.unpack_from('<8I', b, o)
    return dict(vf=vf, acc=acc, vi=vi, q=q, i=i, mac=mac, status=status)


def close(a, b):
    fa, fb = f(a), f(b)
    if a == b:
        return True
    return abs(fa - fb) <= 1e-4 * max(abs(fa), abs(fb), 1e-6)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('elf')
    ap.add_argument('records')
    ap.add_argument('--entry', type=lambda x: int(x, 0), default=None)
    ap.add_argument('--trace', type=int, default=-1, help='print the pair trace of this record')
    ap.add_argument('--i-same-pair', action='store_true',
                    help='model the old generator: an LOI is already seen by the upper of its own pair')
    args = ap.parse_args()
    mem = load_code(args.elf)
    data = open(args.records, 'rb').read()
    diff = {}
    n = len(data) // REC
    for k in range(n):
        o = k * REC
        entry = struct.unpack_from('<I', data, o)[0]
        if args.entry is not None and entry != args.entry:
            continue
        before = parse_state(data, o + 4)
        after = parse_state(data, o + 4 + 624)
        st = State(before['vf'], before['acc'], before['vi'], before['q'], before['i'], before['mac'])
        tr = [] if k == args.trace else None
        run(mem, entry, st, {'i_same_pair': args.i_same_pair}, tr)
        if tr is not None:
            for pc, cyc, wv, wa, q, i, ms in tr:
                print('%04x c%-3d q=%-12.6g I=%-10.6g mac4=%04x %s' % (pc, cyc, f(q), f(i), ms,
                      ' '.join('vf%d=(%s)' % (r, ','.join('%.6g' % f(x) for x in v)) for r, v in wv if r)))
        bad = []
        for r in range(1, 32):
            for c in range(4):
                if not close(st.vf[r][c], after['vf'][r][c]):
                    bad.append('vf%d%s' % (r, FIELDS[c]))
        for c in range(4):
            if not close(st.acc[c], after['acc'][c]):
                bad.append('acc%s' % FIELDS[c])
        for r in range(1, 16):
            if (st.vi[r] & 0xffff) != (after['vi'][r] & 0xffff):
                bad.append('vi%d' % r)
        if not close(st.q, after['q']):
            bad.append('q')
        for b in bad:
            diff.setdefault(b, []).append(k)
        if k < 6 or bad and k < 12:
            v20 = [f(x) for x in st.vf[20]]
            g20 = [f(x) for x in after['vf'][20]]
            print('rec %d entry 0x%x: %s | ref vf20 %s |q|^2 %.4f ; recorded %s |q|^2 %.4f' % (
                k, entry, ' '.join(bad) or 'match', ['%.4f' % x for x in v20], sum(x * x for x in v20),
                ['%.4f' % x for x in g20], sum(x * x for x in g20)))
    print('%d records' % n)
    for b, ks in sorted(diff.items(), key=lambda x: -len(x[1])):
        print('  %-6s differs in %d records (first %s)' % (b, len(ks), ks[:5]))


if __name__ == '__main__':
    main()
