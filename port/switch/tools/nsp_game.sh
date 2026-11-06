export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64 PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:/mingw64/bin:$PATH
SRC="$(cygpath -u "$NX_SRC")"
rm -rf /work/hbl; mkdir -p /work/hbl; tar -xzf "$(cygpath -u "$HBL_TGZ")" -C /work/hbl --strip-components=1; cd /work/hbl; bash /hbl_patch.sh; make -j4 > make.log 2>&1; grep -E "error|undefined" make.log | cut -c1-140 | head -5; ls hbl.elf
rm -rf /work/nx/pk; mkdir -p /work/nx/pk/exefs /work/nx/pk/control /work/nx/pk/romfs; cd /work/nx/pk
elf2nso /work/hbl/hbl.elf exefs/main
sed 's/svr_nsptest/svr2011/' "$SRC/tests/vktest/nsp.json" > game.json; npdmtool game.json exefs/main.npdm 2>/dev/null
nacptool --create "WWE SmackDown vs. Raw 2011" "SvR 2011 port" "$NX_VER" control/control.nacp --titleid=0500565220110000
cp "$SRC/icon.jpg" control/icon_AmericanEnglish.dat
cp /work/nx/b/svr2011.nro romfs/svr2011.nro
# The compact shaders (uniform-buffer constants), until the SD card's pack has them.
mkdir -p romfs/game/native_shaders; cp "$SVR_SPVC"/*.spvc romfs/game/native_shaders/; ls romfs/game/native_shaders | wc -l
/work/hbp/hacbrewpack.exe -k 'C:\Users\Ranger\.switch\prod.keys' --titleid 0500565220110000 --nologo --exefsdir exefs --romfsdir romfs --controldir control --nspdir nsp 2>&1 | sed -E 's/[0-9A-Fa-f]{32,}/[hidden]/g' | grep -E "Created NSP:|rror" | cut -c1-90
ls -la nsp | tail -1 | cut -c24-
