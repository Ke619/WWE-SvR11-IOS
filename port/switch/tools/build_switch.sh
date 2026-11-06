#!/bin/bash
# Builds the Switch program from a checkout of this repository. Steps and what they need: port/docs/SWITCH_BUILD.md.
#   build_switch.sh sources|codegen|build|nsp|all
# Run from MSYS2's bash. Settings, in the environment:
#   NX_WORK    work folder under /work, with no spaces (default /work/nx)
#   NX_DRIVER  folder with mesa-switch's lib and include (default $NX_WORK/nvk-sdk)
#   NX_XEX     your own default.xex (codegen)
#   NX_REXGLUE the PC SDK's rexglue.exe (codegen)
#   NX_SPVC    folder with the .spvc shaders (nsp)
#   NX_KEYS    your own prod.keys (nsp). Its contents are never printed.
#   NX_VER     version in the NSP (default 0.1.0)
#   NX_LIVE    ON only for a release: Report a problem may use the public server (default OFF)
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="$(cd "$HERE/../.." && pwd)"
WORK="${NX_WORK:-/work/nx}"
DRIVER="${NX_DRIVER:-$WORK/nvk-sdk}"
TITLE=0500565220110000

# The sources this port was built against.
REX_URL=https://github.com/rexglue/rexglue-sdk
REX_COMMIT=c94f5eb
PLUME_URL=https://github.com/renderbag/plume.git
PLUME_COMMIT=d723793
HBL_URL=https://github.com/switchbrew/nx-hbloader
HBL_COMMIT=82b95122c5ae
HBP_URL=https://github.com/XITRIX/hacBrewPack
HBP_COMMIT=745b16e

export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64
export PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:/mingw64/bin:$PATH

# devkitA64's compiler is a Windows program: it reads "/work/..." from the root of the current drive, and can't read
# MSYS paths such as /e/... See "Folders" in port/docs/SWITCH_BUILD.md.
case "$WORK" in /work/*) ;; *) echo "NX_WORK must be under /work (see SWITCH_BUILD.md)"; exit 1 ;; esac
case "$PORT" in /work/*) ;; *) echo "Run this through a checkout under /work (see SWITCH_BUILD.md), not $PORT"; exit 1 ;; esac
case "$WORK$PORT" in *" "*) echo "No spaces allowed in these paths: the tools fail on them."; exit 1 ;; esac

fetch_repo() {  # url commit folder
  [ -d "$3/.git" ] && return 0
  git -c core.autocrlf=false clone -q "$1" "$3"
  git -C "$3" config core.autocrlf false
  git -C "$3" checkout -q "$2"
  git -C "$3" submodule update -q --init --recursive --jobs 4
}

fetch_tar() {  # url commit folder
  [ -d "$3" ] && return 0
  mkdir -p "$3"
  git -c core.autocrlf=false clone -q "$1" "$3"
  git -C "$3" checkout -q "$2"
}

# Git on Windows checks symbolic links out as small text files holding the target's path (libmspack has some that
# are source files). They become copies of their targets.
resolve_links() {  # folder
  git -C "$1" ls-files -s | awk -F'	' '$1 ~ /^120000/ {print $2}' | while IFS= read -r f; do
    local path="$1/$f"
    [ -f "$path" ] && [ "$(wc -c < "$path")" -lt 512 ] || continue
    local target="$(dirname "$path")/$(cat "$path")"
    [ -e "$target" ] || continue
    rm "$path"
    cp -r "$target" "$path"
  done
}

apply_once() {  # folder patch
  local mark="$1/.svr2011_$(basename "$2").applied"
  [ -f "$mark" ] && return 0
  git -C "$1" apply --whitespace=nowarn "$2"
  touch "$mark"
}

sources() {
  mkdir -p "$WORK"
  fetch_repo "$REX_URL" "$REX_COMMIT" "$WORK/rex"
  git -C "$WORK/rex" submodule status --recursive | awk '{print $2}' | while IFS= read -r sub; do
    resolve_links "$WORK/rex/$sub"
  done
  apply_once "$WORK/rex" "$PORT/patches/rexglue-sdk-svr2011.patch"
  apply_once "$WORK/rex" "$PORT/patches/rexglue-sdk-switch.patch"
  apply_once "$WORK/rex/thirdparty/FFmpeg" "$PORT/patches/ffmpeg-switch.patch"
  fetch_repo "$PLUME_URL" "$PLUME_COMMIT" "$WORK/plume"
  apply_once "$WORK/plume" "$PORT/patches/plume-svr2011.patch"
  apply_once "$WORK/plume" "$PORT/patches/plume-switch.patch"
  fetch_tar "$HBL_URL" "$HBL_COMMIT" "$WORK/hbl"
  fetch_tar "$HBP_URL" "$HBP_COMMIT" "$WORK/hbp"
  [ -f "$WORK/hbp/hacbrewpack.exe" ] || {
    cd "$WORK/hbp"
    cp config.mk.template config.mk
    make -j4 CC=gcc > make.log 2>&1 || { tail -5 make.log; exit 1; }
  }
  echo "sources: ready in $WORK"
}

codegen() {
  [ -f "$NX_XEX" ] || { echo "Set NX_XEX to your own default.xex"; exit 1; }
  [ -f "$NX_REXGLUE" ] || { echo "Set NX_REXGLUE to the PC SDK's rexglue.exe"; exit 1; }
  local gen="$WORK/gen"
  mkdir -p "$gen/assets"
  cp "$NX_XEX" "$gen/assets/default.xex"
  bash "$HERE/nx_codegen.sh" "$PORT" "$gen" "$NX_REXGLUE"
  rm -rf "$WORK/generated"
  mkdir -p "$WORK/generated"
  cp -r "$gen/generated/default" "$WORK/generated/default"
  echo "codegen: $(ls "$WORK/generated/default" | wc -l) files"
}

build() {
  [ -f "$DRIVER/lib/libvulkan.a" ] || { echo "No driver at $DRIVER (NX_DRIVER)"; exit 1; }
  cmake -G Ninja -S "$PORT/switch" -B "$WORK/b" -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release \
    -DPython3_EXECUTABLE=/mingw64/bin/python -DSVR_SDK_ONLY=OFF -DSVR_PORT="$PORT" -DREXSDK_DIR="$WORK/rex" -DPLUME_DIR="$WORK/plume" \
    -DSVR_GENERATED="$WORK/generated" -DNVK_SDK="$DRIVER" -DSVR_NX_REPORTS_LIVE="${NX_LIVE:-OFF}" -Wno-dev > "$WORK/configure.log" 2>&1 \
    || { tail -20 "$WORK/configure.log"; exit 1; }
  ninja -C "$WORK/b" -j "${NX_JOBS:-4}" svr2011 > "$WORK/build.log" 2>&1 || { grep -E "FAILED|error:|undefined reference" "$WORK/build.log" | head -30; exit 1; }
  cd "$WORK/b"
  nacptool --create "SvR 2011" "SvR 2011 port" "${NX_VER:-0.1.0}" svr2011.nacp
  aarch64-none-elf-strip -o svr2011.stripped.elf svr2011.elf
  elf2nro svr2011.stripped.elf svr2011.nro --nacp=svr2011.nacp > /dev/null
  ls -la svr2011.nro
}

nsp() {
  [ -f "$NX_KEYS" ] || { echo "Set NX_KEYS to your own prod.keys"; exit 1; }
  [ -d "$NX_SPVC" ] || { echo "Set NX_SPVC to the folder with the .spvc shaders"; exit 1; }
  local hbl="$WORK/hbl_build" pk="$WORK/pk"
  rm -rf "$hbl" "$pk"
  cp -r "$WORK/hbl" "$hbl"
  (cd "$hbl" && HBL_DIR="$hbl" bash "$HERE/hbl_patch.sh" > /dev/null && make -j4 > make.log 2>&1) || { tail -5 "$hbl/make.log"; exit 1; }
  mkdir -p "$pk/exefs" "$pk/control" "$pk/romfs/game/native_shaders"
  cd "$pk"
  elf2nso "$hbl/hbl.elf" exefs/main
  sed 's/svr_nsptest/svr2011/' "$PORT/switch/tests/vktest/nsp.json" > game.json
  npdmtool game.json exefs/main.npdm 2> /dev/null
  nacptool --create "WWE SmackDown vs. Raw 2011" "SvR 2011 port" "${NX_VER:-0.1.0}" control/control.nacp --titleid=$TITLE
  cp "$PORT/switch/icon.jpg" control/icon_AmericanEnglish.dat
  cp "$WORK/b/svr2011.nro" romfs/svr2011.nro
  cp "$NX_SPVC"/*.spvc romfs/game/native_shaders/
  # (hacBrewPack prints key material: anything that looks like it is hidden.)
  "$WORK/hbp/hacbrewpack.exe" -k "$(cygpath -w "$NX_KEYS")" --titleid $TITLE --nologo --exefsdir exefs --romfsdir romfs \
    --controldir control --nspdir nsp 2>&1 | sed -E 's/[0-9A-Fa-f]{32,}/[hidden]/g' | grep -E "Created NSP:|rror" | cut -c1-90
  rm -f hacbrewpack_backup/* 2> /dev/null || true
  ls -la "nsp/$TITLE.nsp"
}

case "${1:-all}" in
  sources) sources ;;
  codegen) codegen ;;
  build) build ;;
  nsp) nsp ;;
  all) sources; codegen; build; nsp ;;
  *) echo "sources, codegen, build, nsp or all"; exit 1 ;;
esac
