# The game's loader, from nx-hbloader (ISC). The program is in the NSP (romfs:/svr2011.nro);
# a newer one at sdmc:/SvR2011/update/svr2011.nro wins, which is how updates arrive.
# When the game exits the process leaves at once: returning through the loader's
# unload path ended in "the software was closed".
f=/work/hbl/source/main.c
sed -i 's|"sdmc:/hbmenu.nro"|"sdmc:/SvR2011/update/svr2011.nro"|' "$f"
n=$(grep -n '^void loadNro(void)' "$f" | cut -d: -f1)
[ -n "$n" ] || { echo "PATCH POINT NOT FOUND"; exit 1; }
n=$((n+1))
sed -i "${n}a\    static bool s_started;\n    if (s_started \&\& g_nextNroPath[0] == 0)\n        svcExitProcess();\n    s_started = true;\n" "$f"
sed -i 's|^    int fd = open(g_nextNroPath, O_RDONLY);|    romfsMountFromCurrentProcess("romfs");\n    int fd = open(g_nextNroPath, O_RDONLY);\n    if (fd < 0)\n        fd = open("romfs:/svr2011.nro", O_RDONLY);|' "$f"
sed -i 's|^    fsdevUnmountAll();|    fsdevUnmountAll();\n    romfsUnmount("romfs");|' "$f"
sed -i -E 's/(g_innerheap\[)0x[0-9a-fA-F]+(\])/\10x40000\2/' "$f"
grep -c "romfs" "$f"; grep -n "g_innerheap\[" "$f" | head -1 | cut -c1-70
