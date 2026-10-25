#define _GNU_SOURCE
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* growable string                                                     */
/* ------------------------------------------------------------------ */

void sb_init(strbuf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void sb_free(strbuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void sb_reserve(strbuf *b, size_t extra) {
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void sb_putc(strbuf *b, char c) {
    sb_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void sb_putn(strbuf *b, const char *s, size_t n) {
    sb_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void sb_puts(strbuf *b, const char *s) {
    sb_putn(b, s, strlen(s));
}

void sb_printf(strbuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    sb_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

char *sb_take(strbuf *b) {
    if (!b->data) return xstrdup("");
    char *p = b->data;
    b->data = NULL;
    b->len = b->cap = 0;
    return p;
}

/* ------------------------------------------------------------------ */
/* allocation                                                          */
/* ------------------------------------------------------------------ */

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "x68k: out of memory (%zu bytes)\n", n);
        abort();
    }
    return p;
}

void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) {
        fprintf(stderr, "x68k: out of memory (%zu x %zu bytes)\n", n, sz);
        abort();
    }
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "x68k: out of memory (%zu bytes)\n", n);
        abort();
    }
    return q;
}

char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *xstrndup(const char *s, size_t n) {
    size_t l = strnlen(s, n);
    char *p = xmalloc(l + 1);
    memcpy(p, s, l);
    p[l] = '\0';
    return p;
}

/* ------------------------------------------------------------------ */
/* paths                                                               */
/* ------------------------------------------------------------------ */

bool path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

bool path_is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int mkdir_p(const char *path, mode_t mode) {
    if (!path || !*path) return -1;
    char tmp[X68K_PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(tmp, path, len + 1);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

char *path_join(const char *a, const char *b) {
    if (!a || !*a) return xstrdup(b ? b : "");
    if (!b || !*b) return xstrdup(a);
    if (b[0] == '/') return xstrdup(b);
    size_t la = strlen(a);
    bool slash = a[la - 1] != '/';
    size_t n = la + (slash ? 1 : 0) + strlen(b) + 1;
    char *p = xmalloc(n);
    snprintf(p, n, "%s%s%s", a, slash ? "/" : "", b);
    return p;
}

char *path_dirname(const char * path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return xstrdup(".");
    if (slash == path) return xstrdup("/");
    return xstrndup(path, (size_t)(slash - path));
}

const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

char *path_absolute(const char *path) {
    if (!path || !*path) return xstrdup("");
    if (path[0] == '/') return xstrdup(path);
    char cwd[X68K_PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return xstrdup(path);
    return path_join(cwd, path);
}

char *path_real(const char *path) {
    char buf[X68K_PATH_MAX];
    if (!realpath(path, buf)) return NULL;
    return xstrdup(buf);
}

bool path_has_ext(const char *path, const char *ext) {
    if (!path || !ext) return false;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot || !dot[1]) return false;
    return strcasecmp(dot + 1, ext) == 0;
}

char *file_extension(const char *path) {
    if (!path) return NULL;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot || !dot[1]) return NULL;
    char *e = xstrdup(dot + 1);
    for (char *p = e; *p; p++) *p = (char)tolower((unsigned char)*p);
    return e;
}

char *find_file_ci(const char *dir, const char *name) {
    if (!dir || !name) return NULL;
    char *direct = path_join(dir, name);
    if (path_exists(direct)) return direct;
    free(direct);

    DIR *d = opendir(dir);
    if (!d) return NULL;
    struct dirent *de;
    char *found = NULL;
    while ((de = readdir(d)) != NULL) {
        if (strcasecmp(de->d_name, name) == 0) {
            found = path_join(dir, de->d_name);
            break;
        }
    }
    closedir(d);
    return found;
}

/* ------------------------------------------------------------------ */
/* env / time                                                          */
/* ------------------------------------------------------------------ */

char *env_home_dir(void) {
    const char *h = getenv("HOME");
    if (h && *h) return xstrdup(h);
    return xstrdup(".");
}

char *env_dup(const char *name) {
    const char *v = getenv(name);
    return (v && *v) ? xstrdup(v) : NULL;
}

int64_t now_ms_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t now_ms_realtime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void time_stamp(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, n, "%Y%m%d-%H%M%S", &tmv);
}

/* ------------------------------------------------------------------ */
/* parsing / misc                                                      */
/* ------------------------------------------------------------------ */

bool parse_int(const char *s, long long *out) {
    if (!s || !*s) return false;
    errno = 0;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (errno || !end || *end != '\0') return false;
    *out = v;
    return true;
}

bool parse_duration(const char *s, double *seconds, bool *is_frames, long long *frames) {
    if (!s || !*s) return false;
    char buf[64];
    if (strlen(s) >= sizeof(buf)) return false;
    strcpy(buf, s);
    char *p = str_trim(buf);
    if (!*p) return false;

    /* strip a trailing unit; bare numbers mean seconds */
    size_t len = strlen(p);
    char *unit = NULL;
    for (size_t i = 0; i < len; i++) {
        if (!(isdigit((unsigned char)p[i]) || p[i] == '.' || p[i] == '+' || p[i] == '-')) {
            unit = p + i;
            break;
        }
    }
    double mult = 1.0;
    bool fr = false;
    if (unit) {
        char *u = unit;
        while (*u == ' ' || *u == '\t') u++;
        /* trim trailing whitespace */
        char *e = u + strlen(u);
        while (e > u && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
        char unitbuf[16];
        snprintf(unitbuf, sizeof(unitbuf), "%s", u);
        for (char *q = unitbuf; *q; q++) *q = (char)tolower((unsigned char)*q);
        *unit = '\0';
        u = unitbuf;
        if (!strcmp(u, "s") || !strcmp(u, "sec") || !strcmp(u, "secs") || !strcmp(u, "second") || !strcmp(u, "seconds")) {
            mult = 1.0;
        } else if (!strcmp(u, "ms") || !strcmp(u, "msec") || !strcmp(u, "millisecond") || !strcmp(u, "milliseconds")) {
            mult = 0.001;
        } else if (!strcmp(u, "m") || !strcmp(u, "min") || !strcmp(u, "mins") || !strcmp(u, "minute") || !strcmp(u, "minutes")) {
            mult = 60.0;
        } else if (!strcmp(u, "f") || !strcmp(u, "fr") || !strcmp(u, "frame") || !strcmp(u, "frames")) {
            fr = true;
        } else {
            return false;
        }
    }

    errno = 0;
    char *end = NULL;
    double v = strtod(p, &end);
    if (errno || !end || *end != '\0' || v <= 0 || !(v < 1e9)) return false;
    if (fr) {
        double n = v;
        if (n < 1.0 || n > 300000.0) return false;
        long long nf = (long long)(n + 0.5);
        if (nf < 1) return false;
        *is_frames = true;
        *frames = nf;
        *seconds = 0.0;
    } else {
        *is_frames = false;
        *frames = 0;
        *seconds = v * mult;
        if (*seconds <= 0 || *seconds > 3600.0) return false;
    }
    return true;
}

bool parse_size_arg(const char *s, unsigned *w, unsigned *h) {
    if (!s) return false;
    unsigned a, b;
    char extra;
    if (sscanf(s, "%ux%u%c", &a, &b, &extra) != 2) return false;
    if (a < 16 || b < 16 || a > 4096 || b > 4096) return false;
    if ((a & 1) || (b & 1)) return false; /* yuv420p requires even dimensions */
    *w = a;
    *h = b;
    return true;
}

uint64_t fnv1a64_update(uint64_t h, const void *data, size_t len) {
    const unsigned char *p = data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

uint64_t fnv1a64(const void *data, size_t len) {
    return fnv1a64_update(0xcbf29ce484222325ULL, data, len);
}

char *str_trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
    return s;
}

char *tail_file(const char *path, size_t max_bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    size_t want = (size_t)sz;
    if (want > max_bytes) want = max_bytes;
    if (fseek(f, (long)((size_t)sz - want), SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = xmalloc(want + 1);
    size_t got = fread(buf, 1, want, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

const char *bool_str(bool v) {
    return v ? "true" : "false";
}

double clamp_double(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
