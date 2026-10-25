/*
 * util.h - small utilities shared by the x68k headless frontend
 */
#ifndef X68K_UTIL_H
#define X68K_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define X68K_PATH_MAX 4096

/* ---- growable string buffer ---- */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} strbuf;

void  sb_init(strbuf *b);
void  sb_free(strbuf *b);
void  sb_reserve(strbuf *b, size_t extra);
void  sb_putc(strbuf *b, char c);
void  sb_putn(strbuf *b, const char *s, size_t n);
void  sb_puts(strbuf *b, const char *s);
void  sb_printf(strbuf *b, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
char *sb_take(strbuf *b); /* returns malloc'd buffer, resets b */

/* ---- allocation (aborts on OOM) ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* ---- paths ---- */
bool path_exists(const char *path);
bool path_is_dir(const char *path);
int  mkdir_p(const char *path, mode_t mode);
char *path_join(const char *a, const char *b);       /* malloc'd */
char *path_dirname(const char *path);                /* malloc'd */
const char *path_basename(const char *path);
char *path_absolute(const char *path);               /* malloc'd, relative to cwd */
char *path_real(const char *path);                   /* malloc'd or NULL */
bool path_has_ext(const char *path, const char *ext); /* case-insensitive, ext without dot */
char *find_file_ci(const char *dir, const char *name); /* malloc'd full path or NULL */
char *file_extension(const char *path);              /* malloc'd lowercase ext without dot */

/* ---- environment / home ---- */
char *env_home_dir(void);                  /* $HOME or "." (malloc'd) */
char *env_dup(const char *name);           /* getenv copy or NULL */

/* ---- time ---- */
int64_t now_ms_monotonic(void);
int64_t now_ms_realtime(void);
void    time_stamp(char *buf, size_t n);   /* local "YYYYmmdd-HHMMSS" */

/* ---- small helpers ---- */
bool  parse_duration(const char *s, double *seconds, bool *is_frames, long long *frames);
bool  parse_int(const char *s, long long *out);
bool  parse_size_arg(const char *s, unsigned *w, unsigned *h);
uint64_t fnv1a64(const void *data, size_t len);
uint64_t fnv1a64_update(uint64_t h, const void *data, size_t len);
char *str_trim(char *s);
char *tail_file(const char *path, size_t max_bytes);
const char *bool_str(bool v);
double clamp_double(double v, double lo, double hi);

#endif
