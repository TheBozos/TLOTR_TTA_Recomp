"""tta_bozo_audio.py <game dir> <out dir> <BozoEngine dir>: The Third Age's sound libraries as native C++.

Relocates LIBSD.IRX and AUDIOPF.IRX (EA's sound driver) from the player's disc with BozoEngine's offline IRX tools
(tools/native_irx, docs/native-irx.md), translates them to <name>_iop.cpp, and writes tta_audio_modules.h with the
Image records runtime/tta_audio.cpp registers. Everything it writes is derived from the disc: build tree only.
"""
import pathlib
import subprocess
import sys

# Load bases: LIBSD (0x5980 bytes) then AUDIOPF (0x1496c), both under the compiled libraries' heap at 0x80000.
# Entries: functions the module only reaches through a pointer (thread bodies, callbacks), which the translator's
# call-graph walk cannot see; each was named by a "no compiled IRX target" failure and read in the module's code.
MODULES = [("LIBSD", 0x45A00, []), ("AUDIOPF", 0x4B400, [0x4B400, 0x4C69C])]


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    game, out, engine = (pathlib.Path(a) for a in sys.argv[1:])
    out.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(engine / "tools" / "native_irx"))
    from relocate import relocate
    from recompile import Module

    lines = ['#pragma once', '#include "bozo/native/compiled_libraries.h"', '#include <iterator>',
             'inline void addTtaAudioImages(bozo::native::irx::CompiledLibraries &host)', '{']
    for name, base, entries in MODULES:
        info = relocate(game / "MODULES" / f"{name}.IRX", base, out / name)
        exports, imports = Module(str(out / f"{name}.json")).tables()
        subprocess.run([sys.executable, "-I", str(engine / "tools" / "native_irx" / "recompile.py"),
                        str(out / f"{name}.json"), "--out", str(out)]
                       + (["--entry", ",".join(f"{e:x}" for e in entries)] if entries else []), check=True)
        data = (out / f"{name}.bin").read_bytes()
        lines += ['    {', '        bozo::native::irx::Image image;',
                  f'        image.name = "{info["name"]}";', f'        image.hash = 0x{info["hash"]}ull;',
                  f'        image.base = 0x{base:x};', f'        image.entry = 0x{info["entry"]:x};',
                  f'        image.gp = 0x{info["gp"]:x};', '        static const uint8_t bytes[] = {']
        lines += ['            ' + ' '.join(f'0x{v:02x},' for v in data[i:i + 32]) for i in range(0, len(data), 32)]
        lines += ['        };', '        image.bytes.assign(std::begin(bytes), std::end(bytes));']
        for label, entries in (("imports", imports), ("exports", exports)):
            lines += [f'        image.{label} = {{']
            lines += [f'            {{0x{address:x}, "{library}", {ordinal}}},' for address, (library, ordinal) in entries.items()]
            lines += ['        };']
        lines += [f'        image.nativeLibsd = {"true" if name == "LIBSD" else "false"};',
                  '        host.addImage(std::move(image));', '    }']
    lines += ['}']
    (out / "tta_audio_modules.h").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
