# The whole game in one NSP: program and game files in its romfs.
export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64 PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:/mingw64/bin:$PATH
SRC="$(cygpath -u "$NX_SRC")"
WK=/e/SvR2011SwitchWork
mkdir -p "$WK/pk/exefs" "$WK/pk/control" "$WK/pk/romfs/game"
cd "$WK/pk"
elf2nso /work/hbl/hbl.elf exefs/main
sed 's/svr_nsptest/svr2011/' "$SRC/tests/vktest/nsp.json" > "$WK/game.json"; npdmtool "$WK/game.json" exefs/main.npdm 2>/dev/null
nacptool --create "WWE SmackDown vs. Raw 2011" "SvR 2011 port" "$NX_VER" control/control.nacp --titleid=0500565220110000
cp "$SRC/icon.jpg" control/icon_AmericanEnglish.dat
cp /work/nx/b/svr2011.nro romfs/svr2011.nro
rm -rf hacbrewpack_nca hacbrewpack_temp hacbrewpack_backup nsp
/work/hbp/hacbrewpack.exe -k 'C:\Users\Ranger\.switch\prod.keys' --titleid 0500565220110000 --nologo --exefsdir exefs --romfsdir romfs --controldir control --nspdir nsp 2>&1 | sed -E 's/[0-9A-Fa-f]{32,}/[hidden]/g' | grep -E "Created NSP:|rror|ailed" | cut -c1-100
ls -la nsp | tail -1 | cut -c24-
