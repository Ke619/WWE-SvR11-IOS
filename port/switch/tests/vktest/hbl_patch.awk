/^    if \(g_nextNroPath\[0\] == '\0'\)/ && !done {
    print "    static bool s_started;";
    print "    if (s_started && g_nextNroPath[0] == '\0')";
    print "        svcExitProcess(); /* the game has exited: leave, don't start it again */";
    print "    s_started = true;";
    print "";
    done = 1;
}
{ print }
