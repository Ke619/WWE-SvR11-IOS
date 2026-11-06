# Starts our NRO once. When it exits, the process leaves straight away: returning
# through the loader's unload path ended in "the software was closed".
f=/work/hbl/source/main.c
sed -i 's|"sdmc:/hbmenu.nro"|"sdmc:/switch/svr_loaded.nro"|' "$f"
n=$(grep -n '^void loadNro(void)' "$f" | cut -d: -f1)
[ -n "$n" ] || { echo "PATCH POINT NOT FOUND"; exit 1; }
n=$((n+1))
sed -i "${n}a\    static bool s_started;\n    if (s_started \&\& g_nextNroPath[0] == 0)\n        svcExitProcess();\n    s_started = true;\n" "$f"
sed -n "$((n-1)),$((n+6))p" "$f"
