export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64 PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:/mingw64/bin:$PATH
S="$(cygpath -u "$NX_SRC")"
cmake -G Ninja -S "$S" -B /work/nx/b -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release -DSVR_SDK_ONLY=${NX_SDK_ONLY:-OFF} -Wno-dev 2>&1 | grep -E "Error|error|not found|Generating done" | cut -c1-140 | head -8
cd /work/nx/b; ninja -k 0 -j4 "$@" > /work/nx/build.log 2>&1; echo "ninja exit $?"; grep -c "^\[" /work/nx/build.log; grep -E "error|Error " /work/nx/build.log | sed -E 's#C:/msys64/work/nx/rex/##; s#/work/nx/rex/##' | grep -v "^ninja" | cut -c1-165 | sort | uniq | head -${NX_LINES:-60}
