#!/usr/bin/env bash
# The movies' decoder: a minimal static FFmpeg (LGPL-2.1-or-later) with only what libmpeg uses, libavcodec's MPEG-2
# video decoder and parser, and libavutil, built into build/ffmpeg (include/, lib/), which CMakeLists.txt hands to
# BozoEngine as BOZO_FFMPEG_ROOT. The executables then need no FFmpeg DLL (MSYS2's FFmpeg pulls ~80, 121 MB). The same
# release and configuration as the TMB build kit's.
#   scripts/build_ffmpeg.sh                 download the release (checked against its SHA-256) and build
#   TTA_FFMPEG_TARBALL=<ffmpeg-9.0.2.tar.xz> scripts/build_ffmpeg.sh   from a copy already on disk
# Needs a POSIX shell and GNU make (MSYS2: pacman -S make), the C compiler and curl. ~1 minute.
# Windows: FFmpeg's configure and make in MSYS2's own UCRT64 shell. From another one (Git Bash: its own MSYS runtime
# and tools), MSYS2's make cannot start the compiler (error 127): the script runs itself again there.
if [ -x /c/msys64/usr/bin/bash.exe ] && [ "${MSYSTEM:-}" != UCRT64 ]; then
    script="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
    exec /c/msys64/usr/bin/env MSYSTEM=UCRT64 CHERE_INVOKING=1 TTA_FFMPEG_TARBALL="${TTA_FFMPEG_TARBALL:-}" \
        /c/msys64/usr/bin/bash -l "$script" "$@"
fi
source "$(dirname "$0")/common.sh"

VERSION=9.0.2
URL="https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
PREFIX="$ROOT/build/ffmpeg"
WORK="$ROOT/build/ffmpeg-build"

command -v make > /dev/null || { echo "build_ffmpeg: GNU make is missing (MSYS2: pacman -S make)"; exit 1; }

mkdir -p "$WORK/tmp"
export TMPDIR="$WORK/tmp" # configure's own scratch files: a POSIX path (Windows' TMP has backslashes)
tarball="${TTA_FFMPEG_TARBALL:-$WORK/ffmpeg-$VERSION.tar.xz}"
if [ ! -f "$tarball" ]; then
    echo "build_ffmpeg: downloading $URL"
    curl -fL --retry 3 -o "$tarball.part" "$URL"
    mv "$tarball.part" "$tarball"
fi
echo "$SHA256  $tarball" | sha256sum -c --quiet - || { echo "build_ffmpeg: $tarball does not match FFmpeg $VERSION's SHA-256"; exit 1; }

src="$WORK/ffmpeg-$VERSION"
rm -rf "${src:?}"
tar -xf "$tarball" -C "$WORK"

target=()
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) target=(--target-os=mingw32 --enable-w32threads) ;; esac
(
    cd "$src"
    ./configure --prefix="$PREFIX" --arch=x86_64 "${target[@]}" --cc=gcc --cxx=g++ --pkg-config=false \
        --enable-static --disable-shared --disable-programs --disable-doc --disable-debug --disable-everything \
        --disable-autodetect --disable-network --disable-avdevice --disable-avformat --disable-avfilter \
        --disable-swresample --disable-swscale --enable-avcodec --enable-avutil \
        --enable-decoder=mpeg2video --enable-parser=mpegvideo --disable-x86asm > "$WORK/configure.log" 2>&1 ||
        { tail -20 "$WORK/configure.log"; exit 1; }
    make -j "$JOBS" > "$WORK/make.log" 2>&1 || { tail -30 "$WORK/make.log"; exit 1; }
    make install > "$WORK/install.log" 2>&1 || { tail -20 "$WORK/install.log"; exit 1; }
)
cp "$src/COPYING.LGPLv2.1" "$PREFIX/"
echo "build_ffmpeg: FFmpeg $VERSION (mpeg2video only, static) in $PREFIX"
