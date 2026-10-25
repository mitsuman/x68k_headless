#ifndef X68K_LOG_H
#define X68K_LOG_H
#include <stdarg.h>

void xlog_set_quiet(int quiet);
void xlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void xlogv(const char *fmt, va_list ap);

#endif
