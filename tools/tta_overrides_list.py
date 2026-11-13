"""tta_overrides_list.py <runtime dir> <out.txt>: every guest address the port overrides.

Scans runtime/*.cpp for registerFunction(0x...) and writes one "override 0xADDR" line per address, the --overrides
list bozo-externals reads (BozoEngine tools/externals). An address the port registers must be an override in the generated code, or a
direct call would skip the port's function.
"""
import pathlib
import re
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    runtime, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    pattern = re.compile(r"registerFunction\(\s*(0x[0-9A-Fa-f]+)")
    addresses = set()
    for source in sorted(runtime.glob("*.cpp")):
        addresses.update(int(a, 16) for a in pattern.findall(source.read_text(encoding="utf-8")))
    out.write_text("".join(f"override 0x{a:x}\n" for a in sorted(addresses)), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
