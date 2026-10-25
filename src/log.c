#include "log.h"

#include <stdio.h>
#include <time.h>

static int g_quiet;

void xlog_set_quiet(int quiet) { g_quiet = quiet; }

void xlogv(const char *fmt, va_list ap) {
    if (g_quiet) return;
    char ts[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    fprintf(stderr, "[%s] ", ts);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void xlog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    xlogv(fmt, ap);
    va_end(ap);
}
