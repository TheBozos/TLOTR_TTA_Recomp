"""tta_vu0_gen.py <SLUS_210.27> <out.cpp>: The Third Age's VU0 microprograms as native C++ for BozoEngine.

The game starts them with VCALLMS; BozoEngine has no VU0 micro mode, so each entry becomes a function on the calling
context's VU0 state (Registry::vu0Program). The microcode is read from the player's ELF (two images the game uploads
with VIF MPG commands: A at file offset 0x249a84, pairs 0..329; B at 0x239d94, pairs 336..501) and translated pair by
pair on BozoEngine's VU0 operations (bozo/ops_vu0.h, the same arithmetic as the macro instructions).

Execution model, one pair a cycle:
- in a pair the lower instruction reads its operands before the upper instruction writes (both results are committed
  at the pair's end); a pair whose two halves write the same register is refused;
- Q: DIV and SQRT land 7 cycles after issue, RSQRT 13, or at WAITQ; a read before that sees the old Q;
- FMAND and FMEQ see the MAC flags as they were 4 pairs earlier (the FMAC pipeline's delay);
- a branch's next pair is its delay slot; the E bit ends the program after the next pair;
- LOI (the upper's I bit) loads I from the lower word, which the upper of that pair already uses.
FMAC interlock stalls are not modelled (they do not change results, only the cycle count Q and the flags are timed on).
Any instruction outside what TTA's two images use is refused, so a different image cannot be translated silently.
"""
import struct
import sys

IMAGES = [(0x249A84, "A"), (0x239D94, "B")]
ENTRIES = [0x0, 0x38, 0x60, 0xD0, 0x178, 0x230, 0x498, 0x500, 0x690, 0x7D0, 0x870, 0x8F0, 0x9C0,
           0xA80, 0xB00, 0xBE0, 0xC28, 0xCC8, 0xD20, 0xD38, 0xDB0, 0xF10, 0xF50]
FIELD = "xyzw"


def load(path):
    d = open(path, "rb").read()
    mem = {}
    for offset, _ in IMAGES:
        p = offset
        for _ in range(8):
            code = struct.unpack_from("<I", d, p)[0]
            cmd, num, imm = (code >> 24) & 0x7F, (code >> 16) & 0xFF, code & 0xFFFF
            if cmd == 0x4A:
                n = num or 256
                p += 4
                p = (p + 7) & ~7
                for k in range(n):
                    mem[imm + k] = struct.unpack_from("<II", d, p + 8 * k)
                p += n * 8
            elif cmd == 0:
                p += 4
            else:
                break
    return mem


def dest_of(w):
    return (w >> 21) & 0xF


def vf(r):
    return f"vf[{r}]"


class Pair:
    """One pair's C++: reads into temporaries, then commits."""

    def __init__(self, pc, lower, upper):
        self.pc, self.lo, self.up = pc, lower, upper
        self.ubody, self.lbody, self.commit = [], [], []
        self.uwrites, self.lwrites = set(), set()
        self.branch = None      # (condition C++, target pc)
        self.end = bool(upper >> 30 & 1)
        self.reads_mac = False
        self.q_issue = None     # latency
        self.waitq = False
        self.reads_q = False


def upper_insn(p):
    w = p.up
    op, dest = w & 0x3F, dest_of(w)
    ft, fs, fd, bc = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 3
    if op < 0x3C:
        if op <= 0x1B and op not in (0x1C, 0x1D, 0x1E, 0x1F):
            kind = ["add", "sub", "madd", "msub", "max", "mini", "mul"][op >> 2]
            t = f"bc({vf(ft)}, {bc})"
        elif op in (0x1C, 0x20, 0x21, 0x24, 0x25):
            kind = {0x1C: "mul", 0x20: "add", 0x21: "madd", 0x24: "sub", 0x25: "msub"}[op]
            t = "broadcast(q)"
            p.reads_q = True
        elif op in (0x1D, 0x1E, 0x1F, 0x22, 0x23, 0x26, 0x27):
            kind = {0x1D: "max", 0x1E: "mul", 0x1F: "mini", 0x22: "add", 0x23: "madd", 0x26: "sub", 0x27: "msub"}[op]
            t = "broadcast(ctx->vu0_i_bits)"
        elif op in (0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2F):
            kind = {0x28: "add", 0x29: "madd", 0x2A: "mul", 0x2B: "max", 0x2C: "sub", 0x2D: "msub", 0x2F: "mini"}[op]
            t = vf(ft)
        elif op == 0x2E:
            if fd:
                p.ubody.append(f"Vec u = {vf(fd)}; opmsub(ctx, u, acc, {vf(fs)}, {vf(ft)});")
                p.uwrites.add(fd)
                p.commit.append(f"{vf(fd)} = u;")
            else:
                p.ubody.append(f"{{ Vec u{{}}; opmsub(ctx, u, acc, {vf(fs)}, {vf(ft)}); }}")
            return
        else:
            raise ValueError(f"pair 0x{p.pc:x}: upper op 0x{op:x}")
        if not fd:
            # A write to vf0 is discarded; the flags are still set (an FMAC result).
            target = "Vec u{};"
        else:
            target = f"Vec u = {vf(fd)};"
        if kind in ("max", "mini"):
            p.ubody.append(f"{target} {kind}(u, {vf(fs)}, {t}, {dest});")
        elif kind in ("madd", "msub"):
            p.ubody.append(f"{target} {kind}(ctx, u, acc, {vf(fs)}, {t}, {dest});")
        else:
            p.ubody.append(f"{target} {kind}(ctx, u, {vf(fs)}, {t}, {dest});")
        if fd:
            p.uwrites.add(fd)
            p.commit.append(f"{vf(fd)} = u;")
        return
    sp = ((w >> 4) & 0x7C) | (w & 3)
    if sp == 0x2F or sp == 0x2B and False:
        return  # NOP
    if sp in range(0x00, 0x10) or sp in (0x18, 0x19, 0x1A, 0x1B):
        group = sp >> 2
        kind = {0: "add", 1: "sub", 2: "madd", 3: "msub", 6: "mul"}[group]
        t = f"bc({vf(ft)}, {sp & 3})"
    elif sp in (0x1C, 0x20, 0x21, 0x24, 0x25):
        kind = {0x1C: "mul", 0x20: "add", 0x21: "madd", 0x24: "sub", 0x25: "msub"}[sp]
        t = "broadcast(q)"
        p.reads_q = True
    elif sp in (0x1E, 0x22, 0x23, 0x26, 0x27):
        kind = {0x1E: "mul", 0x22: "add", 0x23: "madd", 0x26: "sub", 0x27: "msub"}[sp]
        t = "broadcast(ctx->vu0_i_bits)"
    elif sp in (0x28, 0x29, 0x2A, 0x2C, 0x2D):
        kind = {0x28: "add", 0x29: "madd", 0x2A: "mul", 0x2C: "sub", 0x2D: "msub"}[sp]
        t = vf(ft)
    elif sp == 0x2E:
        p.ubody.append(f"Vec a = acc; opmula(ctx, a, {vf(fs)}, {vf(ft)});")
        p.commit.append("acc = a;")
        return
    elif sp in (0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17):
        frac = [0, 4, 12, 15][sp & 3]
        fn = "itof" if sp < 0x14 else "ftoi"
        p.ubody.append(f"Vec u = {vf(ft)}; {fn}(u, {vf(fs)}, {dest}, {frac});")
        if ft:
            p.uwrites.add(ft)
            p.commit.append(f"{vf(ft)} = u;")
        return
    elif sp == 0x1D:
        p.ubody.append(f"Vec u = {vf(ft)}; abs(u, {vf(fs)}, {dest});")
        if ft:
            p.uwrites.add(ft)
            p.commit.append(f"{vf(ft)} = u;")
        return
    else:
        raise ValueError(f"pair 0x{p.pc:x}: special upper 0x{sp:x}")
    # The accumulator forms.
    if kind in ("madd", "msub"):
        p.ubody.append(f"Vec a = acc; {kind}(ctx, a, acc, {vf(fs)}, {t}, {dest});")
    else:
        p.ubody.append(f"Vec a = acc; {kind}(ctx, a, {vf(fs)}, {t}, {dest});")
    p.commit.append("acc = a;")


def lower_insn(p):
    w = p.lo
    if p.up >> 31 & 1:  # I bit: the lower word is an immediate for I, which this pair's upper uses
        p.lbody.append(f"ctx->vu0_i_bits = 0x{w:08x}u;")
        return "loi"
    op = (w >> 25) & 0x7F
    it, is_, imm11 = (w >> 16) & 31, (w >> 11) & 31, w & 0x7FF
    simm11 = imm11 - 0x800 if imm11 & 0x400 else imm11
    if op == 0x40:
        f = w & 0x3F
        if f >= 0x3C:
            sp = ((w >> 4) & 0x7C) | (w & 3)
            ft, fs, dest = it, is_, dest_of(w)
            if sp == 0x30:  # MOVE (NOP when vf0 to vf0)
                if ft == 0:
                    return
                p.lbody.append(f"Vec l = {vf(ft)}; move(l, {vf(fs)}, {dest});")
                p.lwrites.add(ft)
                p.commit.insert(0, f"{vf(ft)} = l;")
                return
            if sp == 0x31:
                if ft == 0:
                    return
                p.lbody.append(f"Vec l = {vf(ft)}; mr32(l, {vf(fs)}, {dest});")
                p.lwrites.add(ft)
                p.commit.insert(0, f"{vf(ft)} = l;")
                return
            if sp in (0x38, 0x39, 0x3A):
                fsf, ftf = (w >> 21) & 3, (w >> 23) & 3
                if sp == 0x38:
                    expr = f"div(ctx, {vf(fs)}.u[{fsf}], {vf(ft)}.u[{ftf}])"
                    p.q_issue = 7
                elif sp == 0x39:
                    expr = f"sqrt(ctx, {vf(ft)}.u[{ftf}])"
                    p.q_issue = 7
                else:
                    expr = f"rsqrt(ctx, {vf(fs)}.u[{fsf}], {vf(ft)}.u[{ftf}])"
                    p.q_issue = 13
                p.lbody.append(f"const uint32_t qNext = {expr};")
                return
            if sp == 0x3B:
                p.waitq = True
                return
            if sp == 0x3D:  # MFIR: the integer register, sign-extended, into the fields
                p.lbody.append(f"Vec l = {vf(ft)}; for (int k = 0; k < 4; ++k) if (fieldOn({dest}, k)) "
                               f"l.u[k] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(vi[{fs}])));")
                if ft:
                    p.lwrites.add(ft)
                    p.commit.insert(0, f"{vf(ft)} = l;")
                return
            raise ValueError(f"pair 0x{p.pc:x}: lower special 0x{sp:x}")
        id_ = (w >> 6) & 31
        if f in (0x30, 0x31, 0x34, 0x35):
            expr = {0x30: f"vi[{is_}] + vi[{it}]", 0x31: f"vi[{is_}] - vi[{it}]",
                    0x34: f"vi[{is_}] & vi[{it}]", 0x35: f"vi[{is_}] | vi[{it}]"}[f]
            if id_:
                p.lbody.append(f"const uint32_t iNext = ({expr}) & 0xFFFFu;")
                p.commit.insert(0, f"vi[{id_}] = iNext;")
            return
        if f == 0x32:  # IADDI: it = is + imm5
            imm5 = (w >> 6) & 31
            simm5 = imm5 - 32 if imm5 & 16 else imm5
            if it:
                p.lbody.append(f"const uint32_t iNext = (vi[{is_}] + {simm5}) & 0xFFFFu;")
                p.commit.insert(0, f"vi[{it}] = iNext;")
            return
        raise ValueError(f"pair 0x{p.pc:x}: lower 0x40 funct 0x{f:x}")
    if op in (0x08, 0x09):  # IADDIU, ISUBIU
        imm15 = ((w >> 10) & 0x7800) | (w & 0x7FF)
        if it:
            sign = "+" if op == 0x08 else "-"
            p.lbody.append(f"const uint32_t iNext = (vi[{is_}] {sign} {imm15}u) & 0xFFFFu;")
            p.commit.insert(0, f"vi[{it}] = iNext;")
        return
    if op in (0x28, 0x29):  # IBEQ, IBNE
        cond = f"vi[{it}] {'==' if op == 0x28 else '!='} vi[{is_}]"
        p.branch = (cond, p.pc + 8 + simm11 * 8)
        return
    if op in (0x18, 0x1A):  # FMEQ, FMAND: against the MAC flags 4 pairs back
        p.reads_mac = True
        expr = f"(macSeen == (vi[{is_}] & 0xFFFFu)) ? 1u : 0u" if op == 0x18 else f"vi[{is_}] & macSeen"
        if it:
            p.lbody.append(f"const uint32_t iNext = ({expr}) & 0xFFFFu;")
            p.commit.insert(0, f"vi[{it}] = iNext;")
        return
    if op == 0x20:  # B
        p.branch = ("true", p.pc + 8 + simm11 * 8)
        return
    raise ValueError(f"pair 0x{p.pc:x}: lower op 0x{op:x}")


def translate(mem, entry):
    """The pairs reachable from entry (byte address), each as a Pair."""
    pairs, todo = {}, [entry]
    # The pair after an E bit is the program's last: the walk stops after it.
    finals = {idx * 8 + 8 for idx, (lo, up) in mem.items() if up >> 30 & 1}
    while todo:
        pc = todo.pop()
        while pc not in pairs:
            idx = pc // 8
            if idx not in mem:
                raise ValueError(f"entry 0x{entry:x}: pc 0x{pc:x} outside the images")
            lo, up = mem[idx]
            p = Pair(pc, lo, up)
            upper_insn(p)
            lower_insn(p)
            if p.uwrites & p.lwrites:
                raise ValueError(f"pair 0x{pc:x}: upper and lower write the same register")
            pairs[pc] = p
            if p.branch:
                todo.append(p.branch[1])
                todo.append(pc + 16)  # past the delay slot
            if p.end:
                todo.append(pc + 8)   # the pair after the E bit runs
                break
            if pc in finals:
                break
            pc += 8
    return pairs


def emit(mem, out_path):
    lines = [
        "// Generated by tools/tta_vu0_gen.py from SLUS_210.27 (the player's disc): do not edit, never commit.",
        '#include "bozo/ops_vu0.h"',
        '#include "bozo/port.h"',
        "",
        "namespace",
        "{",
        "    using namespace bozo::vu0;",
        "",
    ]
    for entry in ENTRIES:
        pairs = translate(mem, entry)
        uses_mac = any(p.reads_mac for p in pairs.values())
        lines += [f"    void program_{entry:03x}(uint8_t *, R5900Context *ctx, Runtime *)", "    {",
                  "        Vec *vf = ctx->vu0_vf;", "        Vec &acc = ctx->vu0_acc;", "        uint32_t *vi = ctx->vi;",
                  "        uint32_t q = ctx->vu0_q_bits, qPending = 0, qReadyAt = 0;", "        bool qWaiting = false;",
                  "        uint32_t cycle = 0;"]
        if uses_mac:
            lines += ["        uint32_t macPipe[4] = {ctx->vu0_mac, ctx->vu0_mac, ctx->vu0_mac, ctx->vu0_mac};"]
        lines += [f"        uint32_t pc = 0x{entry:x}u, branchTo = 0, pendingTarget = 0;",
                  "        bool branchSet = false, pending = false, endAfter = false, stopAfterThis = false;",
                  "        for (;;)", "        {",
                  "            if (qWaiting && cycle >= qReadyAt) { q = qPending; qWaiting = false; }",
                  "            branchSet = false;"]
        if uses_mac:
            lines += ["            const uint32_t macSeen = macPipe[cycle & 3] & 0xFFFFu;"]
        lines += ["            switch (pc)", "            {"]
        for pc in sorted(pairs):
            p = pairs[pc]
            lines.append(f"            case 0x{pc:x}u:")
            lines.append("            {")
            if p.waitq:
                lines.append("                if (qWaiting) { q = qPending; qWaiting = false; }")
            for b in p.lbody:
                lines.append("                " + b)
            if p.ubody:
                lines.append("                {")
                for b in p.ubody:
                    lines.append("                    " + b)
                for c in p.commit:
                    if c.startswith("acc") or "= u;" in c:
                        lines.append("                    " + c)
                lines.append("                }")
            for c in p.commit:
                if not (c.startswith("acc") or "= u;" in c):
                    lines.append("                " + c)
            if p.q_issue:
                lines.append(f"                qPending = qNext; qWaiting = true; qReadyAt = cycle + {p.q_issue};")
            if p.branch:
                cond, target = p.branch
                lines.append(f"                if ({cond}) {{ branchSet = true; branchTo = 0x{target:x}u; }}")
            if p.end:
                lines.append("                endAfter = true;")
            lines.append("                break;")
            lines.append("            }")
        lines += ["            default:", "                return; // not reached: every pair of the program is a case",
                  "            }"]
        if uses_mac:
            lines += ["            macPipe[cycle & 3] = ctx->vu0_mac;"]
        lines += ["            ++cycle;",
                  "            if (stopAfterThis) break;",
                  "            if (endAfter) { endAfter = false; stopAfterThis = true; }",
                  "            // A branch's delay slot (the next pair) runs before the jump.",
                  "            pc = pending ? pendingTarget : pc + 8;",
                  "            pending = branchSet;",
                  "            pendingTarget = branchTo;",
                  "        }",
                  "        if (qWaiting) q = qPending; // the call returns once VU0 is idle: Q has landed",
                  "        ctx->vu0_q_bits = q;",
                  "    }", ""]
    lines += ["}", "", "namespace tta", "{",
              "    // TTA's VU0 microprograms, by entry (Registry::vu0Program).",
              "    void installVu0(bozo::port::Registry &registry)", "    {"]
    for entry in ENTRIES:
        lines.append(f"        registry.vu0Program(0x{entry:x}u, program_{entry:03x});")
    lines += ["    }", "}", ""]
    src = "\n".join(lines)
    # endAfter: declared once per program, set when the pair after an E bit has been queued.
    open(out_path, "w", newline="\n").write(src)


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    emit(load(sys.argv[1]), sys.argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main())
