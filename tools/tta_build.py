#!/usr/bin/env python3
# Adapted from The Bozos TMB_Recomp (MIT); see launcher/LICENSE for original attribution.
"""Prepare The Lord of the Rings: The Third Age (USA, SLUS-21027) from the player's own disc.

The TMB / DC launcher and build-kit workflow, adapted for TTA: verify the executable; extract what the build reads
(the ELF, EA's sound driver modules, the .SCP archives the textures and meshes come from); configure the pinned native
engine; recompile the game's code and its sound driver; convert textures and meshes offline; compile and stage a
relocatable game folder (tta.exe finds its ELF, disc image, symbol map, textures and meshes beside it).
No game data or generated game code is distributed. ISO and BIN/CUE use the engine's disc reader.

  python tools/tta_build.py --disc "Lord of the Rings, The - The Third Age (USA).iso" --out D:/TTA
  kit/python/python.exe -I -X utf8 source/tools/tta_build.py --disc IMAGE --out build --game game

The bundled kit builds offline without MSYS2, git or an installed Python. --check validates a disc
without building. PROGRESS/PERCENT/SKIP/DONE/ERROR/RESULT lines use the same launcher protocol as TMB.
"""
import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


def configure_gcc_paths(env, compiler_bin):
    """Keep MinGW's linker paths on the ASCII alias, even if GCC resolves its executable's junction.

    GCC_EXEC_PREFIX also selects startup objects; LIBRARY_PATH selects the native static libraries.
    See https://gcc.gnu.org/onlinedocs/gcc/Environment-Variables.html .
    """
    prefix = os.path.dirname(os.path.abspath(compiler_bin))
    gcc = os.path.join(prefix, 'lib', 'gcc')
    versions = sorted(p for p in glob.glob(os.path.join(gcc, '*', '*')) if os.path.isdir(p))
    if os.name != 'nt' or not versions:
        return
    env['GCC_EXEC_PREFIX'] = gcc.replace('\\', '/') + '/'
    libraries = versions + [os.path.join(prefix, 'x86_64-w64-mingw32', 'lib'), os.path.join(prefix, 'lib')]
    env['LIBRARY_PATH'] = os.pathsep.join(p.replace('\\', '/') for p in libraries if os.path.isdir(p))
ENGINE = os.path.join(ROOT, 'BozoEngine')  # --engine replaces it
ps2_disc = None  # BozoEngine's tools/native_pack/ps2_disc.py, imported by load_engine()

ELF_NAME = 'SLUS_210.27'
ELF_SHA1 = 'd88ded370592114169fa18f0ff73b9f610a94bd0'
# Other releases of the game by their boot executable (none recorded yet: any other disc is "not TTA (USA)").
OTHER_RELEASES = {}
# Disc inputs the build reads: the ELF, EA's sound driver modules (compiled to C++ by tools/tta_bozo_audio.py) and the
# .SCP archives tools/tta_textures.py and tools/tta_meshes.py convert. The game reads everything else from disc.iso.
DISC_FILES = ('SLUS_210.27', 'SYSTEM.CNF', 'MODULES/LIBSD.IRX', 'MODULES/AUDIOPF.IRX', 'DATA/LOADONCE.SCP')
DISC_PATTERNS = (re.compile(r'^DATA/GAME/[^/]+\.SCP$'), re.compile(r'^DATA/GAME/[^/]+/[^/]+\.SCP$'))
DISC_HASHES = {'SLUS_210.27': ELF_SHA1}
STEPS = ['verify', 'extract', 'configure', 'tools', 'recompile', 'packs', 'compile', 'stage']
# FetchContent sources (BozoEngine's runtime/CMakeLists.txt): <name>-src in the kit's deps/.
FETCH_NAMES = ['sdl3', 'imgui']
GAME_EXE = 'tta'
EXE = '.exe' if os.name == 'nt' else ''
KIT_MANIFEST = 'kit_manifest.json'


class BuildError(Exception):
    pass


def emit(*parts):
    print(' '.join(str(p) for p in parts), flush=True)


def load_engine(path=None):
    """BozoEngine's tree (--engine, else the submodule) and its disc reader."""
    global ENGINE, ps2_disc
    if path:
        ENGINE = os.path.abspath(path)
    tools = os.path.join(ENGINE, 'tools', 'native_pack')
    if not os.path.isfile(os.path.join(tools, 'ps2_disc.py')):
        raise SystemExit('ERROR verify BozoEngine is missing (%s): the release is incomplete, or in a git checkout run '
                         '"git submodule update --init BozoEngine"' % ENGINE)
    sys.path.insert(0, tools)
    import ps2_disc as module  # noqa: E402
    ps2_disc = module


def sha1_file(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def fingerprint(*items):
    return hashlib.sha1(json.dumps(items, sort_keys=True, default=str).encode()).hexdigest()[:16]


def file_stamp(path):
    try:
        st = os.stat(path)
        return [os.path.normcase(os.path.abspath(path)), st.st_size, st.st_mtime_ns]
    except OSError:
        return [path, None]


def tree_stamp(folder, exts=None):
    """Sizes and mtimes of a source tree (cheap; changes when the source is updated)."""
    out = []
    for dirpath, dirnames, filenames in os.walk(folder):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith('.') and d not in ('build', '__pycache__'))
        for name in sorted(filenames):
            if exts and not name.lower().endswith(exts):
                continue
            p = os.path.join(dirpath, name)
            st = os.stat(p)
            out.append((os.path.relpath(p, folder), st.st_size, st.st_mtime_ns))
    return fingerprint(out)


SOURCE_EXTS = ('.cpp', '.h', '.hpp', '.inl', '.c', '.txt', '.cmake', '.rc', '.ico', '.manifest', '.in', '.py')


# ---- PE imports (the DLLs the game needs, found in the toolchain's bin) ----

# windres (the game's icon, version and UTF-8 manifest resources) runs its preprocessor through popen, that is
# cmd /c: by default as "<its own folder>\gcc" -E ... "<the .rc>". With a space in the toolchain's path (C:\Users\Jean
# Dupont\..., D:\Games\TTA Recomp\kit) and a second quoted argument, cmd strips the outer quotes and runs
# "D:\Games\TTA" ("preprocessing failed"). The bare name, found on the PATH tta_build puts the toolchain first on, has no
# quotes for cmd to strip.
RC_FLAGS = '--preprocessor=gcc --preprocessor-arg=-E --preprocessor-arg=-xc --preprocessor-arg=-DRC_INVOKED'


def cmake_rc_flags():
    return ['-DCMAKE_RC_FLAGS=' + RC_FLAGS] if os.name == 'nt' else []


def pe_imports(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'MZ':
        return []
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        return []
    nsec, opt_size = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    dd = opt + (112 if magic == 0x20b else 96)
    sections = []
    for i in range(nsec):
        s = pe + 24 + opt_size + 40 * i
        vsize, va, rsize, roff = struct.unpack_from('<IIII', data, s + 8)
        sections.append((va, max(vsize, rsize), roff))

    def off(rva):
        for va, size, roff in sections:
            if va <= rva < va + size:
                return rva - va + roff
        return None

    def cstr(o):
        return data[o:data.index(b'\0', o)].decode('latin-1')

    names = []
    for index, stride, name_at in ((1, 20, 12), (13, 32, 4)):  # import table, delay-load table
        rva = struct.unpack_from('<I', data, dd + 8 * index)[0]
        o = off(rva) if rva else None
        while o is not None and o + stride <= len(data):
            name_rva = struct.unpack_from('<I', data, o + name_at)[0]
            if not name_rva:
                break
            no = off(name_rva)
            if no is not None:
                names.append(cstr(no))
            o += stride
    return names


def runtime_dlls(exe, search_dirs):
    """The DLLs exe loads (recursively) that one of search_dirs has: what must sit next to it."""
    found, queue = {}, [exe]
    while queue:
        for name in pe_imports(queue.pop()):
            key = name.lower()
            if key in found:
                continue
            for d in search_dirs:
                p = os.path.join(d, name)
                if os.path.isfile(p):
                    found[key] = p
                    queue.append(p)
                    break
            else:
                found[key] = None  # a Windows DLL
    return sorted(p for p in found.values() if p)


# ---- the build ----

class Build:
    def __init__(self, args):
        self.args = args
        self.out = os.path.abspath(args.out)
        self.game = os.path.abspath(args.game) if args.game else os.path.join(self.out, 'TTA')
        self.disc_dir = os.path.join(self.out, 'disc')
        self.cmake_build = os.path.join(self.out, 'build')
        self.logs = os.path.join(self.out, 'logs')
        self.state_path = os.path.join(self.out, 'tta_build_state.json')
        os.makedirs(self.logs, exist_ok=True)
        try:
            with open(self.state_path) as f:
                self.state = json.load(f)
        except (OSError, ValueError):
            self.state = {}
        if self.state.get('engine') != 'bozo':
            # A work folder of the fork's build (generated/, its CMake cache): its step records mean nothing here.
            self.state = {'engine': 'bozo'}
        self.toolchain_bin = None
        self.env = dict(os.environ)
        self.step = None
        self.kit = find_kit(args)
        self.ffmpeg_root = None
        if self.kit:
            # The kit's parts unless the command line names others.
            args.toolchain = args.toolchain or os.path.join(self.kit, 'ucrt64')
            args.fetch_cache = args.fetch_cache or os.path.join(self.kit, 'deps')
            self.ffmpeg_root = os.path.join(self.kit, 'ffmpeg')
        if args.debug_info is None:  # a player's build (the kit): no debug information
            args.debug_info = 'no' if self.kit else 'yes'

    def save_state(self):
        tmp = self.state_path + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(self.state, f, indent=1)
        os.replace(tmp, self.state_path)

    # -- helpers

    def log_path(self):
        return os.path.join(self.logs, self.step + '.log')

    def run(self, cmd, cwd=None, env=None, percent=None, append=False):
        """Runs cmd with its output in the step's log; percent(line) -> 0-100 or None for PERCENT lines."""
        with open(self.log_path(), 'a' if append else 'w', encoding='utf-8', errors='replace') as log:
            log.write('$ %s\n' % subprocess.list2cmdline(cmd))
            log.flush()
            try:
                p = subprocess.Popen(cmd, cwd=cwd, env=env or self.env, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, bufsize=1, universal_newlines=True,
                                     encoding='utf-8', errors='replace')
            except OSError as e:
                raise BuildError('cannot start %s: %s' % (cmd[0], e.strerror))
            last = -1
            for line in p.stdout:
                log.write(line)
                if percent:
                    v = percent(line)
                    if v is not None and v != last:
                        last = v
                        emit('PERCENT', self.step, v)
            code = p.wait()
        if code != 0:
            with open(self.log_path(), encoding='utf-8', errors='replace') as f:
                tail = f.readlines()[-25:]
            sys.stderr.write(''.join(tail))
            raise BuildError('%s failed (exit %d); the full output is in %s'
                             % (os.path.basename(cmd[0]), code, self.log_path()))

    def cmake_build_targets(self, *targets, percent=None):
        self.run([self.tool('cmake'), '--build', self.cmake_build, '--target'] + list(targets) +
                 ['-j', str(self.args.jobs)], percent=percent)

    # -- inputs

    def find_toolchain(self):
        if self.toolchain_bin:
            return self.toolchain_bin
        cands = []
        if self.args.toolchain:
            t = os.path.abspath(self.args.toolchain)
            cands.append(t if os.path.isfile(os.path.join(t, 'g++' + EXE)) else os.path.join(t, 'bin'))
        else:
            gxx = shutil.which('g++')
            if gxx:
                cands.append(os.path.dirname(gxx))
            if os.name == 'nt':
                cands.append('C:/msys64/ucrt64/bin')
        for c in cands:
            missing = [t for t in ('g++', 'gcc', 'cmake', 'ninja') if not os.path.isfile(os.path.join(c, t + EXE))]
            if not missing:
                self.toolchain_bin = os.path.normpath(c)
                break
        else:
            raise BuildError('no compiler toolchain found (looked in %s): pass --toolchain <dir> with g++, gcc, cmake '
                             'and ninja in its bin folder (an MSYS2 UCRT64 prefix: C:/msys64/ucrt64)'
                             % (', '.join(cands) or 'PATH'))
        self.env['PATH'] = self.toolchain_bin + os.pathsep + self.env.get('PATH', '')
        configure_gcc_paths(self.env, self.toolchain_bin)
        return self.toolchain_bin

    def tool(self, name):
        return os.path.join(self.find_toolchain(), name + EXE)

    def toolchain_key(self):
        self.find_toolchain()
        return fingerprint(file_stamp(self.tool('gcc')), self.env.get('GCC_EXEC_PREFIX', ''),
                           self.env.get('LIBRARY_PATH', ''))

    def disc_input(self):
        src = os.path.abspath(self.args.disc)
        if not os.path.exists(src):
            raise BuildError('%s does not exist' % src)
        return src

    def elf(self):
        return os.path.join(self.disc_dir, ELF_NAME)

    # -- steps

    def key(self, step):
        a = self.args
        prev = lambda name: self.state.get(name, {}).get('key')
        if step == 'verify':
            src = self.disc_input()
            inputs = [file_stamp(os.path.join(src,n)) for n in DISC_HASHES] if os.path.isdir(src) else file_stamp(src)
            return fingerprint(inputs, DISC_HASHES)
        if step == 'extract': return fingerprint(prev('verify'), DISC_FILES, [p.pattern for p in DISC_PATTERNS], self.game)
        if step == 'configure':
            return fingerprint(self.find_toolchain(), a.build_type, a.debug_info, a.fetch_cache, self.ffmpeg_root,
                               self.toolchain_key(), cmake_rc_flags(), ROOT, ENGINE, sys.executable, prev('extract'),
                               [file_stamp(os.path.join(ROOT,n)) for n in ('CMakeLists.txt','VERSION')],
                               [file_stamp(os.path.join(ENGINE,n)) for n in ('CMakeLists.txt','runtime/CMakeLists.txt')],
                               file_stamp(os.path.join(self.kit,KIT_MANIFEST)) if self.kit else None)
        if step == 'tools': return fingerprint(prev('configure'), tree_stamp(os.path.join(ENGINE,'recompiler'),SOURCE_EXTS),tree_stamp(os.path.join(ENGINE,'tools','externals'),SOURCE_EXTS))
        if step == 'recompile':
            return fingerprint(prev('tools'), *[tree_stamp(os.path.join(ROOT,d),SOURCE_EXTS+('.py','.csv','.patches')) for d in ('runtime','recomp','tools')],
                               tree_stamp(os.path.join(ENGINE,'tools','native_irx'), SOURCE_EXTS))
        if step == 'packs': return fingerprint(prev('configure'), *[file_stamp(os.path.join(ROOT,'tools',n)) for n in ('tta_textures.py','tta_meshes.py')])
        if step == 'compile': return fingerprint(prev('recompile'),prev('packs'),tree_stamp(os.path.join(ROOT,'runtime'),SOURCE_EXTS),tree_stamp(os.path.join(ENGINE,'runtime'),SOURCE_EXTS))
        if step == 'stage': return fingerprint(prev('compile'),self.game)
        raise AssertionError(step)

    def step_verify(self):
        src = self.disc_input()
        disc = None
        try:
            if not os.path.isdir(src):
                try: disc = ps2_disc.Disc(src)
                except ps2_disc.DiscError as e: raise BuildError('%s: %s' % (os.path.basename(src),e))
                boot = disc.boot_elf()
                if not boot or boot.upper()!=ELF_NAME: raise BuildError(self.wrong_disc(boot,src))
            for name,expected in DISC_HASHES.items():
                try: digest = sha1_file(os.path.join(src,name)) if disc is None else hashlib.sha1(disc.read(name)).hexdigest()
                except (OSError,ps2_disc.DiscError): raise BuildError('%s: required disc file %s is missing' % (os.path.basename(src),name))
                if digest!=expected:
                    raise BuildError('%s is not the supported USA executable (SHA-1 %s, expected %s). Use an unmodified SLUS-21027 disc image.' % (name,digest,expected))
        finally:
            if disc is not None: disc.close()
        emit('INFO','verify','USA disc (SLUS-21027), executable SHA-1 ok')

    @staticmethod
    def wrong_disc(boot, src):
        if boot and boot.upper() in OTHER_RELEASES and boot.upper() != ELF_NAME:
            what = OTHER_RELEASES[boot.upper()]
            return ('%s is %s of The Third Age; TLOTR_TTA_Recomp only supports the USA disc (SLUS-21027).'
                    % (os.path.basename(src), what))
        if boot:
            return ('%s boots %s: not The Third Age (USA, SLUS-21027), the only disc TLOTR_TTA_Recomp supports.'
                    % (os.path.basename(src), boot))
        return ('%s: no %s found: not The Third Age\'s USA disc (SLUS-21027), or not a PS2 disc image.'
                % (os.path.basename(src), ELF_NAME))

    def step_extract(self):
        src = self.disc_input()
        image = src
        if os.path.isdir(src):
            image = self.find_folder_image(src)
            if not image:
                raise BuildError('the game reads the disc image itself: no .iso of the USA disc in %s or the folders '
                                 'above it. Pass the .iso (--disc <image>) instead.' % src)
        os.makedirs(self.game, exist_ok=True)
        marker = os.path.join(self.disc_dir, '.extracted')
        if os.path.exists(marker):
            os.remove(marker)
        self.extract_files(src)
        for name, expected in DISC_HASHES.items():
            if sha1_file(os.path.join(self.disc_dir,name)) != expected:
                raise BuildError('extracted %s differs from the verified disc; retry extraction' % name)
        # Mark the verified extraction complete.
        with open(marker, 'w') as f:
            f.write('extracted by tools/tta_build.py\n')
        self.place_image(image)

    def find_folder_image(self, folder):
        """An extracted folder: the .iso it came from, in it or up to three folders above (as the runtime looks)."""
        d = os.path.abspath(folder)
        for _ in range(4):
            for name in sorted(os.listdir(d)):
                if not name.lower().endswith('.iso'):
                    continue
                try:
                    disc = ps2_disc.Disc(os.path.join(d, name))
                except ps2_disc.DiscError:
                    continue
                try:
                    if (disc.boot_elf() or '').upper() == ELF_NAME and \
                            hashlib.sha1(disc.read(ELF_NAME)).hexdigest() == ELF_SHA1:
                        return os.path.join(d, name)
                finally:
                    disc.close()
            parent = os.path.dirname(d)
            if parent == d:
                break
            d = parent
        return None

    def place_image(self, src):
        """The game reads the disc from a 2048-byte-sector image of it, <game>/disc.iso (dc.exe --iso; it also finds
        it walking up from the ELF's folder). A hard link to the player's .iso when it is on the same drive, else a
        copy; a .bin / .cue is converted."""
        dest = os.path.join(self.game, 'disc.iso')
        disc = ps2_disc.Disc(src)
        try:
            img = disc.image
            total = img.size // img.sector_size
            if os.path.exists(dest) and img.sector_size == 2048 and os.path.samefile(src,dest):
                return
            if os.path.exists(dest):
                os.remove(dest)
            if img.sector_size == 2048:
                try:
                    os.link(src, dest)
                    return
                except OSError:
                    pass
            free = shutil.disk_usage(self.game).free
            if free < total * 2048 + (2 << 30):
                raise BuildError('not enough free space on %s for the disc image: %.1f GB free, about %.1f GB needed'
                                 % (self.game, free / 2**30, (total * 2048 + (2 << 30)) / 2**30))
            tmp = dest + '.part'
            with open(tmp, 'wb') as f:
                lba = 0
                while lba < total:
                    n = min(4096, total - lba)
                    f.write(img.read_sectors(lba, n))
                    lba += n
                    emit('PERCENT', 'extract', lba * 100 // total)
            os.replace(tmp, dest)
        finally:
            disc.close()

    @staticmethod
    def wanted(rel):
        rel = rel.replace('\\','/').upper()
        return rel in DISC_FILES or any(p.match(rel) for p in DISC_PATTERNS)

    def extract_files(self, src):
        """Only what the build reads: the ELF and the packs' files (resumable: finished files stay)."""
        os.makedirs(self.disc_dir, exist_ok=True)
        if os.path.isdir(src):
            files = []
            for dirpath, _dirs, names in os.walk(src):
                for n in names:
                    rel = os.path.relpath(os.path.join(dirpath, n), src)
                    if self.wanted(rel):
                        files.append(rel)
            total = sum(os.path.getsize(os.path.join(src, f)) for f in files) or 1
            done, last = 0, -1
            for rel in files:
                s, d = os.path.join(src, rel), os.path.join(self.disc_dir, rel)
                os.makedirs(os.path.dirname(d), exist_ok=True)
                size = os.path.getsize(s)
                if not (os.path.exists(d) and os.path.getsize(d) == size and
                        (rel.upper() not in DISC_HASHES or sha1_file(d) == DISC_HASHES[rel.upper()])):
                    if os.path.exists(d):
                        os.remove(d)
                    try:
                        os.link(s, d)  # same volume: no copy
                    except OSError:
                        shutil.copy2(s, d)
                done += size
                pct = done * 100 // total
                if pct != last:
                    last = pct
                    emit('PERCENT', 'extract', pct)
            return
        disc = ps2_disc.Disc(src)
        try:
            files = [e for e in disc.entries() if not e.is_dir and self.wanted(e.path)]
            total = sum(e.size for e in files) or 1
            free = shutil.disk_usage(self.out).free
            if free < total + (3 << 30):
                raise BuildError('not enough free space on %s: %.1f GB free, about %.1f GB needed (the disc\'s files '
                                 'and the build)' % (self.out, free / 2**30, (total + (3 << 30)) / 2**30))
            done, last = 0, -1
            for e in files:
                dest = os.path.join(self.disc_dir, e.path)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                if not (os.path.exists(dest) and os.path.getsize(dest) == e.size and
                        (e.path.upper() not in DISC_HASHES or sha1_file(dest) == DISC_HASHES[e.path.upper()])):
                    tmp = dest + '.part'
                    with open(tmp, 'wb') as f:
                        for chunk in disc.image.read_extent(e.lba, e.size):
                            f.write(chunk)
                    os.replace(tmp, dest)
                    if e.mtime is not None:
                        os.utime(dest, (e.mtime, e.mtime))
                done += e.size
                pct = done * 100 // total
                if pct != last:
                    last = pct
                    emit('PERCENT', 'extract', pct)
        finally:
            disc.close()
        emit('INFO', 'extract', '%d files, %.0f MB' % (len(files), total / 1e6))

    def fetch_args(self):
        if not self.args.fetch_cache:
            return []  # CMake downloads the pinned archives (SHA-256 checked)
        cache = os.path.abspath(self.args.fetch_cache)
        out = ['-DFETCHCONTENT_FULLY_DISCONNECTED=ON'] if self.kit else []  # never the network
        for n in FETCH_NAMES:
            d = os.path.join(cache, n + '-src')
            if os.path.isdir(d):
                out.append('-DFETCHCONTENT_SOURCE_DIR_%s=%s' % (n.upper(), d.replace('\\', '/')))
            elif self.kit:
                raise BuildError('the build kit has no deps/%s-src: the kit is incomplete or of an older release' % n)
        return out

    def step_configure(self):
        # A build folder configured from another path (the release folder moved or renamed, or reached once through
        # the non-ASCII aliases and once not), or a fork build's: CMake refuses its cache; start that folder afresh.
        cache = os.path.join(self.cmake_build, 'CMakeCache.txt')
        stamp = os.path.join(self.cmake_build, '.tta-toolchain-key')
        toolchain_key = self.toolchain_key()
        try:
            with open(stamp, encoding='utf-8') as f:
                previous_toolchain = f.read().strip()
        except OSError:
            previous_toolchain = ''
        if os.path.isfile(cache):
            with open(cache, encoding='utf-8', errors='replace') as f:
                text = f.read()
            m = re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$', text, re.M)
            moved = m and os.path.normcase(os.path.normpath(m.group(1))) != os.path.normcase(os.path.normpath(ROOT))
            if moved or previous_toolchain != toolchain_key or not re.search(r'^BOZO_ENGINE_DIR:PATH=', text, re.M):
                emit('INFO', 'configure', 'the build folder was configured %s: configured again'
                     % ('from %s' % m.group(1) if moved else 'with another toolchain configuration'))
                os.remove(cache)
                shutil.rmtree(os.path.join(self.cmake_build, 'CMakeFiles'), ignore_errors=True)
        fwd = lambda p: p.replace('\\', '/')  # noqa: E731
        cmd = [self.tool('cmake'), '-S', ROOT, '-B', self.cmake_build, '-G', 'Ninja',
               '-DCMAKE_BUILD_TYPE=' + self.args.build_type,
               '-DBOZO_ENGINE_DIR=' + fwd(ENGINE),
               '-DTTA_GAME_DIR=' + fwd(self.disc_dir), '-DTTA_ISO=' + fwd(os.path.join(self.game,'disc.iso')),
               '-DBOZO_BUILD_TOOLS=OFF', '-DBUILD_TESTING=OFF', '-DTTA_GUI_SUBSYSTEM=ON', '-DTTA_WINDOW=ON',
               '-DPython3_EXECUTABLE=' + fwd(sys.executable),
               '-DCMAKE_C_COMPILER=' + self.tool('gcc'), '-DCMAKE_CXX_COMPILER=' + self.tool('g++'),
               '-DCMAKE_MAKE_PROGRAM=' + self.tool('ninja')] + cmake_rc_flags()
        if self.args.debug_info == 'no' and self.args.build_type != 'Release':
            # The configuration's own flags without -g: the same code (GCC's -g never changes it).
            flags = {'RelWithDebInfo': '-O2 -DNDEBUG', 'Debug': '-O0'}[self.args.build_type]
            cmd += ['-DCMAKE_%s_FLAGS_%s=%s' % (lang, self.args.build_type.upper(), flags) for lang in ('C', 'CXX')]
        if self.ffmpeg_root:
            cmd.append('-DTTA_FFMPEG_ROOT=' + fwd(self.ffmpeg_root))
        else:
            pkg = os.path.join(self.find_toolchain(), 'pkg-config' + EXE)
            if os.path.isfile(pkg):
                cmd.append('-DPKG_CONFIG_EXECUTABLE=' + pkg)
        self.run(cmd + self.fetch_args())
        with open(stamp, 'w', encoding='utf-8') as f:
            f.write(toolchain_key + '\n')

    def step_tools(self):
        self.cmake_build_targets('bozo-recomp', 'bozo-externals', percent=ninja_percent)

    def step_recompile(self):
        self.cmake_build_targets('tta_generate', 'tta_vu0_generate', 'tta_audio_code', percent=ninja_percent)

    def step_packs(self):
        self.cmake_build_targets('tta_textures_generate', 'tta_meshes_generate', percent=texture_percent)

    def step_compile(self):
        self.cmake_build_targets(GAME_EXE, percent=ninja_percent)

    def exe_path(self):
        return os.path.join(self.cmake_build, GAME_EXE + EXE)

    def step_stage(self):
        exe = self.exe_path()
        if not os.path.isfile(exe):
            raise BuildError('no game at %s' % exe)
        os.makedirs(self.game, exist_ok=True)
        staged = [exe]
        dest = os.path.join(self.game, os.path.basename(exe))
        strip = os.path.join(self.find_toolchain(), 'strip' + EXE)
        if os.path.exists(dest):
            os.remove(dest)
        if self.args.debug_info == 'no' and os.path.isfile(strip):
            self.run([strip, '--strip-debug', '-o', dest, exe])
            shutil.copystat(exe, dest)
        else:
            shutil.copy2(exe, dest)
        if os.name == 'nt':
            for dll in runtime_dlls(exe, [os.path.dirname(exe), self.find_toolchain()]):
                shutil.copy2(dll, os.path.join(self.game, os.path.basename(dll)))
                staged.append(dll)
        shutil.copy2(self.elf(), os.path.join(self.game, ELF_NAME))
        shutil.copy2(os.path.join(ROOT, 'recomp', 'tta_symbols.txt'), os.path.join(self.game, 'tta_symbols.txt'))
        for folder in ('textures', 'meshes'):
            self.stage_folder(os.path.join(self.cmake_build, folder), os.path.join(self.game, folder))
        # Remove obsolete dynamic compiler runtimes from a previous build.
        for old in ('libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll'):
            p = os.path.join(self.game, old)
            if os.path.isfile(p) and p not in staged:
                os.remove(p)
        name = os.path.basename(exe)
        if os.name == 'nt':
            lines = ['@echo off', 'rem Made by tools/tta_build.py: The Lord of the Rings: The Third Age, native PC port (BozoEngine).',
                     'cd /d "%~dp0"',
                     'start "" "%%~dp0%s" --elf "%%~dp0%s" --iso "%%~dp0disc.iso" %%*' % (name, ELF_NAME), '']
            with open(os.path.join(self.game, 'Play The Third Age.cmd'), 'w', newline='\r\n') as f:
                f.write('\n'.join(lines))
        else:
            sh = os.path.join(self.game, 'play_tta.sh')
            with open(sh, 'w', newline='\n') as f:
                f.write('#!/bin/sh\ncd "$(dirname "$0")"\nexec ./%s --elf "$PWD/%s" --iso "$PWD/disc.iso" "$@"\n'
                        % (name, ELF_NAME))
            os.chmod(sh, 0o755)
        with open(os.path.join(self.game, 'tta_build.json'), 'w') as f:
            json.dump({'built': time.strftime('%Y-%m-%d %H:%M:%S'), 'engine': 'BozoEngine', 'elf_sha1': ELF_SHA1,
                       'build_type': self.args.build_type, 'debug_info': self.args.debug_info,
                       'toolchain': self.find_toolchain(), 'kit': self.kit, 'exe': name,
                       'files': [os.path.basename(p) for p in staged]}, f, indent=1)
        emit('INFO', 'stage', '%s, %.1f MB, %d DLLs staged' % (name, os.path.getsize(dest) / 1e6, len(staged) - 1))

    def stage_folder(self, src, dest):
        """A converted folder (textures/, meshes/: ~800 MB together) into the game folder: hard links on the same
        drive, else copies; files no longer in src are removed."""
        if not os.path.isfile(os.path.join(src, 'index.txt')):
            raise BuildError('no %s (the packs step did not finish)' % os.path.join(src, 'index.txt'))
        os.makedirs(dest, exist_ok=True)
        names = set(os.listdir(src))
        for old in os.listdir(dest):
            if old not in names:
                os.remove(os.path.join(dest, old))
        for name in names:
            s, d = os.path.join(src, name), os.path.join(dest, name)
            if os.path.exists(d):
                if os.path.samefile(s, d):
                    continue
                os.remove(d)
            try:
                os.link(s, d)
            except OSError:
                shutil.copy2(s, d)

    def write_built_from(self):
        """<game>/built_from.ini: the release stamp (<TLOTR_TTA_Recomp>/release.ini, written by tools/make_release.py) of the
        code this game folder was built from, after every successful run, even one where every step was skipped (a
        release that changed nothing the build reads). The launcher compares it with the source's to offer Rebuild
        after an update. Without a stamp (a git checkout) the commit is git's when git is there, else empty."""
        stamp = os.path.join(ROOT, 'release.ini')
        if os.path.isfile(stamp):
            with open(stamp, encoding='utf-8') as f:
                text = f.read()
        else:
            commit = ''
            try:
                commit = subprocess.run(['git', '-C', ROOT, 'rev-parse', 'HEAD'], stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, universal_newlines=True).stdout.strip()
            except OSError:
                pass
            text = 'version = dev\ncommit = %s\n' % commit
        with open(os.path.join(self.game, 'built_from.ini'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('# The TLOTR_TTA_Recomp code this game folder was built from (tools/tta_build.py writes this file).\n')
            f.write('\n'.join(l for l in text.splitlines() if l and not l.startswith('#')) + '\n')

    def outputs_present(self, step):
        expected = {
            'extract': [self.elf(), os.path.join(self.game,'disc.iso')],
            'configure': [os.path.join(self.cmake_build,'CMakeCache.txt')],
            'recompile': [os.path.join(self.cmake_build,'tta_gen',n) for n in ('tta_table.cpp','tta_functions.h')],
            'packs': [os.path.join(self.cmake_build,n,'index.txt') for n in ('textures','meshes')],
            'compile': [self.exe_path()],
            'stage': [os.path.join(self.game,n) for n in ('tta'+EXE,ELF_NAME,'tta_symbols.txt','textures/index.txt','meshes/index.txt')],
        }
        return all(os.path.isfile(p) for p in expected.get(step, []))

    # -- driver

    def main(self):
        redo_from = STEPS.index(self.args.redo) if self.args.redo else len(STEPS)
        n = len(STEPS)
        dirty = False
        total_start = time.time()
        for i, step in enumerate(STEPS, 1):
            self.step = step
            try:
                key = self.key(step)
                st = self.state.get(step, {})
                if not dirty and i - 1 < redo_from and st.get('done') and st.get('key') == key and self.outputs_present(step):
                    emit('SKIP', step, '%d/%d' % (i, n))
                    if step == self.args.stop_after:
                        emit('INFO', 'stopped', 'after ' + step)
                        return 0
                    continue
                dirty = True  # every later step runs again (Ninja only redoes what changed)
                emit('PROGRESS', step, '%d/%d' % (i, n), STEP_TITLES[step])
                t = time.time()
                self.state[step] = {'done': False}
                self.save_state()
                getattr(self, 'step_' + step)()
                secs = round(time.time() - t, 1)
                self.state[step] = {'done': True, 'key': self.key(step), 'seconds': secs}
                self.save_state()
                emit('DONE', step, '%d/%d' % (i, n), secs)
                if step == self.args.stop_after:
                    emit('INFO', 'stopped', 'after ' + step)
                    return 0
            except BuildError as e:
                emit('ERROR', step, e)
                return 1
            except KeyboardInterrupt:
                emit('ERROR', step, 'interrupted (run again to resume)')
                return 1
            except Exception as e:  # an unexpected failure: still one clear line, details in the log
                with open(os.path.join(self.logs, step + '.log'), 'a') as log:
                    import traceback
                    traceback.print_exc(file=log)
                emit('ERROR', step, '%s: %s (details in %s)' % (type(e).__name__, e, self.log_path()))
                return 1
        self.write_built_from()
        launcher = os.path.join(self.game, 'Play The Third Age.cmd' if os.name == 'nt' else 'play_tta.sh')
        emit('INFO', 'total', '%.1f s' % (time.time() - total_start))
        emit('RESULT', self.game, launcher)
        return 0


def find_kit(args):
    """The build kit: --kit, else <TLOTR_TTA_Recomp>/kit or its parent's kit/ when it has kit_manifest.json."""
    if args.no_kit:
        return None
    if args.kit:
        kit = os.path.abspath(args.kit)
        if not os.path.isfile(os.path.join(kit, KIT_MANIFEST)):
            raise SystemExit('ERROR kit %s has no %s' % (kit, KIT_MANIFEST))
        return kit
    for kit in (os.path.join(ROOT, 'kit'), os.path.join(os.path.dirname(ROOT), 'kit')):
        if os.path.isfile(os.path.join(kit, KIT_MANIFEST)):
            return kit
    return None


STEP_TITLES = {
    'verify': 'Checking the disc',
    'extract': 'Reading the disc\'s files',
    'configure': 'Configuring the build',
    'tools': 'Preparing the recompiler',
    'recompile': 'Recompiling the game\'s code',
    'packs': 'Converting textures and meshes',
    'compile': 'Compiling (this takes a while)',
    'stage': 'Assembling the game folder',
}

NINJA_RE = re.compile(r'^\[(\d+)/(\d+)\]')


def texture_percent(line):
    m = re.search(r"^TEXTURE_PROGRESS (100|[0-9]{1,2})$", line.strip())
    return int(m.group(1)) if m else None


def ninja_percent(line):
    m = NINJA_RE.match(line)
    if m and int(m.group(2)):
        return int(m.group(1)) * 100 // int(m.group(2))
    return None


def check_disc(path):
    """--check: the verify step alone, for a launcher checking the image the player picked."""
    b = Build.__new__(Build)
    b.args = argparse.Namespace(disc=path)
    try:
        b.step_verify()
    except BuildError as e:
        emit('ERROR', 'verify', e)
        return 1
    except Exception as e:  # noqa: BLE001 (an unreadable file: still one line)
        emit('ERROR', 'verify', '%s: %s' % (os.path.basename(path), e))
        return 1
    emit('OK', 'USA disc (SLUS-21027)')
    return 0


def main():
    ap = argparse.ArgumentParser(description='Builds The Lord of the Rings: The Third Age (USA) as a native PC game from your own disc.')
    ap.add_argument('--disc', required=True, help='the disc image (.iso, .bin, .cue) or an extracted disc folder')
    ap.add_argument('--out', default='tta_build', help='the work folder; the game ends up in <out>/TTA')
    ap.add_argument('--game', help='the game folder to stage (default: <out>/TTA)')
    ap.add_argument('--check', action='store_true', help='only check the disc (no work folder): OK <message> or '
                    'ERROR verify <message>')
    ap.add_argument('--engine', help='BozoEngine\'s source tree (default: the BozoEngine submodule)')
    ap.add_argument('--kit', help='the build kit folder (default: <TLOTR_TTA_Recomp>/kit or <its parent>/kit if present)')
    ap.add_argument('--no-kit', action='store_true', help='do not use a build kit even if one is found')
    ap.add_argument('--toolchain', help='compiler folder (with bin/g++, cmake, ninja), default: the kit, PATH, '
                    'C:/msys64/ucrt64')
    ap.add_argument('--fetch-cache', help='folder with the FetchContent sources (sdl3-src, imgui-src) instead of the '
                    'download')
    ap.add_argument('--build-type', default='Release', choices=['Release', 'RelWithDebInfo', 'Debug'])
    ap.add_argument('--debug-info', choices=['yes', 'no'], help='debug information in the game (default: no with '
                    'the build kit, a player build; yes without it)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--redo', choices=STEPS, help='run this step and the ones after it again')
    ap.add_argument('--stop-after', choices=STEPS, help='stop once this step is done')
    args = ap.parse_args()
    load_engine(args.engine)
    if args.check:
        sys.exit(check_disc(args.disc))
    os.makedirs(os.path.abspath(args.out), exist_ok=True)
    if not os.environ.get(ALIAS_ENV):
        remove_stale_sources()
    if needs_ascii_aliases(args):
        sys.exit(run_through_aliases(args))
    sys.exit(Build(args).main())


def remove_stale_sources():
    """A release's source folder lists its files (release_files.txt, tools/make_release.py). A newer release unzipped
    over an older one leaves the files it no longer has; CMake globs some folders (runtime/*.cpp, BozoEngine's
    sources), so they are removed here: regular files under the source folder only, never through a link, never
    Python's caches; then the folders left empty."""
    listing = os.path.join(ROOT, 'release_files.txt')
    if not os.path.isfile(listing):
        return
    with open(listing, encoding='utf-8') as f:
        keep = set(l.strip() for l in f if l.strip())
    removed = 0
    # PS2Recomp was a managed source component in previous releases. It is absent from the new manifest, so deriving
    # traversal roots solely from that manifest would leave the entire retired runtime behind after an update.
    retired = {'PS2Recomp'}
    tops = sorted(set(rel.split('/', 1)[0] for rel in keep if '/' in rel) | retired)
    linked = lambda p: os.path.islink(p) or (hasattr(os.path, 'isjunction') and os.path.isjunction(p))
    for top in tops:
        if top in ('', '.', '..') or os.path.isabs(top) or '\\' in top or ':' in top:
            raise BuildError('invalid source manifest directory: %r' % top)
        base = os.path.abspath(os.path.join(ROOT, top))
        if os.path.normcase(os.path.commonpath([os.path.abspath(ROOT), base])) != os.path.normcase(os.path.abspath(ROOT)):
            raise BuildError('source cleanup escaped its root: %s' % base)
        if linked(base):
            if top in retired:
                if os.path.islink(base):
                    os.unlink(base)
                else:
                    os.rmdir(base) # the retired junction only, never its target
            continue
        visited = []
        # Prune links before descending. A bottom-up walk cannot prevent traversal of a Windows junction.
        for dirpath, dirs, names in os.walk(base, topdown=True, followlinks=False):
            visited.append(dirpath)
            dirs[:] = [d for d in dirs if (d != '__pycache__' or top in retired) and
                       not linked(os.path.join(dirpath, d))]
            for n in names:
                p = os.path.join(dirpath, n)
                rel = os.path.relpath(p, ROOT).replace('\\', '/')
                if rel not in keep and not linked(p) and os.path.isfile(p):
                    os.remove(p)
                    removed += 1
        for dirpath in reversed(visited):
            if not os.listdir(dirpath):
                os.rmdir(dirpath)
    if removed:
        emit('INFO', 'update', '%d files of an earlier release removed from %s' % (removed, ROOT))


# ---- folders whose path is not ASCII (Windows) ----
#
# MSYS2's binutils open files in the ANSI code page: ld cannot open an object or library whose path, read from the
# response file CMake writes in UTF-8, has a non-ASCII character (C:\Users\Jérôme\..., D:\Jeux\Été\...): "cannot find
# ...obj". GCC itself copes; ld does not, and it is not ours to rebuild. So when the code, the work folder, the game
# folder or the kit has such a path, tta_build runs itself again through directory junctions with plain names
# (%ProgramData%\TLOTR_TTA_Recomp\paths\<hash>-<name>, the same each time so CMake's cache stays valid) and removes the
# junctions afterwards (only the links: os.rmdir on a junction never touches its target). A junction needs no
# administrator rights; the disc image is read by Python only and keeps its path.

ALIAS_ENV = 'TTA_BUILD_ALIASED'


def is_ascii(path):
    try:
        path.encode('ascii')
        return True
    except UnicodeEncodeError:
        return False


def needs_ascii_aliases(args):
    if os.name != 'nt' or os.environ.get(ALIAS_ENV):
        return False
    kit = find_kit(args)
    paths = [ROOT, ENGINE, os.path.abspath(args.out), os.path.abspath(args.game) if args.game else '', kit or '',
             os.path.abspath(args.toolchain) if args.toolchain else '']
    return not all(is_ascii(p) for p in paths)


def alias_folder():
    base = os.path.join(os.environ.get('ProgramData') or r'C:\ProgramData', 'TLOTR_TTA_Recomp', 'paths')
    if is_ascii(base):
        return base
    return os.path.join(os.path.splitdrive(os.path.abspath(ROOT))[0] + os.sep, 'TLOTR_TTA_Recomp_paths')


def make_alias(target, name):
    import _winapi
    target = os.path.abspath(target)
    os.makedirs(target, exist_ok=True)
    folder = alias_folder()
    os.makedirs(folder, exist_ok=True)
    link = os.path.join(folder, hashlib.sha1(os.path.normcase(target).encode('utf-8')).hexdigest()[:10] + '-' + name)
    if os.path.lexists(link):
        remove_alias(link)
    _winapi.CreateJunction(target, link)
    return link


def remove_alias(link):
    """Removes a junction this script made: the link only."""
    if os.path.lexists(link) and (os.path.islink(link) or getattr(os.path, 'isjunction', lambda _p: True)(link)):
        os.rmdir(link)


def run_through_aliases(args):
    kit = find_kit(args)
    links = []
    try:
        root = make_alias(ROOT, 'source')
        links.append(root)
        argv = [sys.executable, '-I', '-X', 'utf8', os.path.join(root, 'tools', 'tta_build.py')]
        out = make_alias(args.out, 'build')
        links.append(out)
        game = None
        if args.game:
            game = make_alias(args.game, 'game')
            links.append(game)
        rest = sys.argv[1:]
        # The same command line, the folders replaced by their aliases.
        replace = {'--out': out, '--game': game}
        if args.toolchain:
            t = make_alias(args.toolchain, 'toolchain')
            links.append(t)
            replace['--toolchain'] = t
        if kit:
            k = make_alias(kit, 'kit')
            links.append(k)
            replace['--kit'] = k
        if args.engine:
            e = make_alias(ENGINE, 'engine')
            links.append(e)
            replace['--engine'] = e
        new = []
        i = 0
        while i < len(rest):
            a = rest[i]
            key = a.split('=', 1)[0]
            if key in replace and replace[key]:
                new += [key, replace[key]]
                i += 1 if '=' in a else 2
                continue
            new.append(a)
            i += 1
        if kit and '--kit' not in [n.split('=', 1)[0] for n in new]:
            new += ['--kit', replace['--kit']]
        emit('INFO', 'paths', 'non-ASCII folders: building through %s' % alias_folder())
        env = dict(os.environ)
        env[ALIAS_ENV] = '1'
        sys.stdout.flush()
        # The links disappear at exit. Protocol paths shown to the launcher/CLI must point at the real folders.
        display = [(root, ROOT), (out, os.path.abspath(args.out))]
        if game:
            display.append((game, os.path.abspath(args.game)))
        if kit:
            display.append((replace['--kit'], kit))
        proc = subprocess.Popen(argv + new, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding='utf-8', errors='replace')
        for line in proc.stdout:
            for alias, real in display:
                line = line.replace(alias, real).replace(alias.replace('\\', '/'), real.replace('\\', '/'))
            print(line, end='', flush=True)
        return proc.wait()
    except OSError as e:
        emit('ERROR', 'verify', 'could not make plain-named links to the folders (%s): move the game\'s folder to a '
             'path with plain letters, such as C:\\Games\\TLOTR_TTA_Recomp' % e)
        return 1
    finally:
        for link in links:
            try:
                remove_alias(link)
            except OSError:
                pass


if __name__ == '__main__':
    main()
