"""tta_rpc_names.py <target.elf> <target symbol map> <target function list> <reference.elf>...

Evidence for naming a stripped executable's SIF RPC clients (libcdvd, libmc, libpad, ...): for every function that calls
sceSifCallRpc, the server id its client structure was bound to (sceSifBindRpc) and the function number it passes, in
the target and in reference executables that keep their symbols. A target function whose (server, number) pair names
exactly one reference function is printed as a candidate with that name, for a person to confirm by reading the code
(recomp/tta_symbols_reviewed.txt). It never writes the map itself.

The scan is a small MIPS reader: within one function it tracks constants in registers (lui/ori/addiu/daddu/move) and
reads a0/a1 at each `jal` to sceSifBindRpc or sceSifCallRpc. Client structures are matched by address.
"""
import struct
import sys
from collections import defaultdict


class Elf:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        phoff, = struct.unpack_from("<I", d, 0x1C)
        phnum, = struct.unpack_from("<H", d, 0x2C)
        self.segments = []
        for i in range(phnum):
            t, off, va, _pa, fs, _ms, _fl, _al = struct.unpack_from("<8I", d, phoff + 32 * i)
            if t == 1:
                self.segments.append((va, off, fs))
        self.symbols = {}  # name -> (address, size)
        shoff, = struct.unpack_from("<I", d, 0x20)
        shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
        sections = [struct.unpack_from("<10I", d, shoff + shentsize * i) for i in range(shnum)] if shoff else []
        for sh in sections:
            if sh[1] == 2:  # SHT_SYMTAB
                strtab = sections[sh[6]]
                for j in range(sh[5] // 16):
                    name_off, value, size, info, _other, _shndx = struct.unpack_from("<IIIBBH", d, sh[4] + 16 * j)
                    if info & 0xF == 2 and value:
                        s = strtab[4] + name_off
                        name = d[s:d.index(b"\0", s)].decode("latin-1")
                        self.symbols[name] = (value, size)

    def word(self, va):
        for start, off, size in self.segments:
            if start <= va < start + size:
                return struct.unpack_from("<I", self.data, off + va - start)[0]
        return None


def scan(elf, start, end, bind, call):
    """(client, server) binds and (client, number) calls inside [start, end)."""
    regs = {0: 0}
    binds, calls = [], []
    pending = None  # a jal's target, acted on after its delay slot
    a = start
    while a < end:
        w = elf.word(a)
        if w is None:
            break
        op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        rd, funct = (w >> 11) & 31, w & 63
        x, y = regs.get(rs), regs.get(rt)
        if op == 0x0F:  # lui
            regs[rt] = (imm << 16) & 0xFFFFFFFF
        elif op == 0x0D:  # ori
            regs[rt] = (x | imm) if x is not None else None
        elif op in (0x09, 0x19):  # addiu, daddiu
            regs[rt] = ((x + simm) & 0xFFFFFFFF) if x is not None else None
        elif op == 0 and funct in (0x21, 0x2D, 0x25):  # addu, daddu, or (move)
            regs[rd] = (((x + y) & 0xFFFFFFFF) if funct != 0x25 else x | y) if x is not None and y is not None else None
        elif op == 0x03:  # jal
            pending = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            a += 4
            continue
        elif op in (0x23, 0x37, 0x20, 0x21, 0x24, 0x25) and rt:  # loads clobber their target
            regs[rt] = None
        if pending is not None:
            if pending == bind:
                binds.append((regs.get(4), regs.get(5)))
            elif pending == call:
                calls.append((regs.get(4), regs.get(5)))
            for r in (2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31):
                regs.pop(r, None)
            pending = None
        a += 4
    return binds, calls


def pairs(elf, functions, bind, call):
    """{function name or address: [(server, number)]} for the functions that call sceSifCallRpc."""
    client_server = {}
    found = {}
    for name, (start, end) in functions.items():
        b, c = scan(elf, start, end, bind, call)
        for client, server in b:
            if client is not None and server is not None:
                client_server[client] = server
        if c:
            found[name] = c
    out = {}
    for name, c in found.items():
        out[name] = sorted({(client_server.get(client), number) for client, number in c}, key=str)
    return out


def main():
    if len(sys.argv) < 5:
        print(__doc__, file=sys.stderr)
        return 2
    target = Elf(sys.argv[1])
    names = {}
    for line in open(sys.argv[2]):
        line = line.split("#")[0].split()
        if len(line) == 2:
            names[line[0]] = int(line[1], 16)
    functions = {}
    for line in open(sys.argv[3]):
        f = line.split()
        if len(f) >= 3 and f[0] == "function":
            functions[int(f[1], 16)] = (int(f[1], 16), int(f[2], 16))
    by_address = {a: n for n, a in names.items()}
    t_pairs = pairs(target, functions, names["sceSifBindRpc"], names["sceSifCallRpc"])

    ref_index = defaultdict(set)  # (server, number) -> reference names
    for path in sys.argv[4:]:
        ref = Elf(path)
        funcs = {n: (a, a + s) for n, (a, s) in ref.symbols.items() if s}
        for name, ps in pairs(ref, funcs, ref.symbols["sceSifBindRpc"][0], ref.symbols["sceSifCallRpc"][0]).items():
            for p in ps:
                if p[0] is not None and p[1] is not None:
                    ref_index[p].add(name)

    for address in sorted(t_pairs):
        ps = t_pairs[address]
        known = by_address.get(address, "")
        cands = set()
        for p in ps:
            cands |= ref_index.get(p, set())
        shown = " ".join(f"{hex(s) if s is not None else '?'}/{hex(n) if n is not None else '?'}" for s, n in ps)
        print(f"0x{address:x} {known or '-':24s} {shown:28s} {' '.join(sorted(cands)) if cands else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
