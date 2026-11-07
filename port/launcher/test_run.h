/* A launcher test run (--capture, --report-send) never writes to a server off this PC (svr2011_launcher.c). */
#ifndef SVR2011_TEST_RUN_H
#define SVR2011_TEST_RUN_H

#include <wchar.h>

/* 1: a test run's write to this host is refused (logged in launcher_test_guard.log); SVR2011_ALLOW_LIVE=1 lets
   it through. 0 outside test runs and for 127.0.0.1 / localhost / ::1. */
int test_run_refuses(const wchar_t *host, const char *what);
/* The same for a server address (a URL, or host[:port]). */
int test_run_refuses_url(const wchar_t *server, const char *what);

#endif
