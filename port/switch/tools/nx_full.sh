export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64 PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:/mingw64/bin:$PATH
S="$(cygpath -u "$NX_SRC")"
cmake -G Ninja -S "$S" -B /work/nx/b -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release -DSVR_SDK_ONLY=OFF -Wno-dev 2>&1 | grep -E "CMake Error|Error at|Generating done" | cut -c1-170 | head -8
cd /work/nx/b
ninja -k 0 -j4 svr2011 > /work/nx/full.log 2>&1; echo "ninja exit $?"
grep -c "^\[" /work/nx/full.log
grep -E "error:|Error |undefined reference" /work/nx/full.log | sed -E 's#^.*/ld.exe: ##; s#^C:/msys64/work/nx/##' | grep -v "^ninja" | sed -E 's/.*(undefined reference to .*)/\1/' | cut -c1-150 | sort | uniq -c | sort -rn > /work/nx/full_err.txt
wc -l /work/nx/full_err.txt; head -70 /work/nx/full_err.txt
ls -la /work/nx/b/svr2011* 2>/dev/null | cut -c24-
