# Starts our NRO once; when it exits, the process leaves instead of restarting it.
f=/work/hbl/source/main.c
sed -i 's|"sdmc:/hbmenu.nro"|"sdmc:/switch/svr_loaded.nro"|' "$f"
n=$(grep -n 'g_nextNroPath\[0\] == ' "$f" | head -1 | cut -d: -f1)
[ -n "$n" ] || { echo "PATCH POINT NOT FOUND"; exit 1; }
sed -i "${n}i\    static bool s_started;\n    if (s_started \&\& g_nextNroPath[0] == 0)\n        svcExitProcess(); /* the game has exited: don't start it again */\n    s_started = true;\n" "$f"
grep -c s_started "$f"
