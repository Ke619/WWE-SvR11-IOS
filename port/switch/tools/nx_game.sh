export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64 PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:/mingw64/bin:$PATH
S="$(cygpath -u "$NX_SRC")"
cmake -G Ninja -S "$S" -B /work/nx/b -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release -DSVR_SDK_ONLY=OFF -Wno-dev 2>&1 | grep -E "CMake Error|Error at|not found|Generating done" | cut -c1-170 | head -12
cd /work/nx/b
ninja -t targets all > /work/nx/targets.txt
T=$(grep -E "^CMakeFiles/svr2011\.dir/.*\.o:" /work/nx/targets.txt | cut -d: -f1)
echo "game objects: $(echo "$T" | wc -l)"
ninja -k 0 -j4 $T > /work/nx/game.log 2>&1; echo "ninja exit $?"
grep -c "^\[" /work/nx/game.log
grep -E "error:|Error " /work/nx/game.log | sed -E 's#^C:/msys64/work/nx/rex/##; s#^.*modest-wilson-a5b648/##' | grep -v "^ninja" | cut -c1-170 | sort -u > /work/nx/game_err.txt
wc -l /work/nx/game_err.txt; cut -d: -f1 /work/nx/game_err.txt | sort | uniq -c | sort -rn | head -50
