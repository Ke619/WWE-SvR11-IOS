#!/bin/sh
# Builds the svrmod test CLI (tools-only, no CMake): port/out/modmaker/svrmod.exe
here=$(dirname "$0")
export PATH="/c/Program Files/LLVM/bin:$PATH"
mkdir -p "$here/../out/modmaker/obj"
# ufbx is big: compile it once
[ -f "$here/../out/modmaker/obj/ufbx.o" ] || clang -O2 -c -o "$here/../out/modmaker/obj/ufbx.o" "$here/third_party/ufbx/ufbx.c"
# LZX for WWE '13 files (svrfmt/wwe13): libmspack's decoder from the SDK
# the SDK: recomp/rexglue-sdk in the project (sdk/rexglue-sdk in older layouts)
sdk="$here/../../recomp/rexglue-sdk"
[ -d "$sdk" ] || sdk="$here/../../sdk/rexglue-sdk"
mspack="$sdk/thirdparty/libmspack/libmspack/mspack"
# (a DLL of its own, LGPL: rexmspack.dll beside svrmod.exe; its C runtime built in, as the SDK's build of it)
[ -f "$here/../out/modmaker/rexmspack.dll" ] || clang -O2 -w -D_CRT_SECURE_NO_WARNINGS -shared -fms-runtime-lib=static -I "$mspack" \
  -Wl,/DEF:"$sdk/thirdparty/lgpl-dlls/rexmspack.def" -Wl,/NODEFAULTLIB:libucrt.lib -Wl,/DEFAULTLIB:ucrt.lib \
  -o "$here/../out/modmaker/rexmspack.dll" "$mspack/lzxd.c"
# zip reading (svrfmt/zip_write.cpp unzip_inflate): the launcher's inflate
clang -O2 -w -c -o "$here/../out/modmaker/obj/unzip.o" "$here/../launcher/unzip.c"
clang++ -std=c++20 -O2 -Wall -Wno-unused-function -D_CRT_SECURE_NO_WARNINGS -DSVRFMT_INFLATE \
  -I "$sdk/thirdparty/stb" -I "$here/third_party/ufbx" -I "$mspack" \
  -o "$here/../out/modmaker/svrmod.exe" "$here/svrmod_cli.cpp" "$here"/svrfmt/*.cpp \
  "$here/../out/modmaker/obj/ufbx.o" "$here/../out/modmaker/rexmspack.lib" "$here/../out/modmaker/obj/unzip.o" -lole32 -lwindowscodecs
