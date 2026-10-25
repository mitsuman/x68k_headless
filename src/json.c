#include "json.h"
#include "util.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    J_NULL,
    J_BOOL,
    J_INT,
    J_DOUBLE,
    J_STRING,
    J_ARRAY,
    J_OBJECT
} jtype;

typedef struct jmember {
    char *key;
    json *value;
    struct jmember *next;
} jmember;

struct json {
    jtype type;
    union {
        bool b;
        int64_t i;
        double d;
        char *s;
        struct {
            json **items;
            size_t len, cap;
        } arr;
        struct {
            jmember *head, *tail;
        } obj;
    } u;
};

static json *jnew(jtype t) {
    json *v = xcalloc(1, sizeof(*v));
    v->type = t;
    return v;
}

json *json_null(void) { return jnew(J_NULL); }
json *json_bool(bool b) { json *v = jnew(J_BOOL); v->u.b = b; return v; }
json *json_int(int64_t i) { json *v = jnew(J_INT); v->u.i = i; return v; }
json *json_double(double d) { json *v = jnew(J_DOUBLE); v->u.d = d; return v; }

json *json_string(const char *s) {
    json *v = jnew(J_STRING);
    v->u.s = xstrdup(s ? s : "");
    return v;
}

json *json_object(void) { return jnew(J_OBJECT); }
json *json_array(void) { return jnew(J_ARRAY); }

void json_free(json *v) {
    if (!v) return;
    switch (v->type) {
    case J_STRING:
        free(v->u.s);
        break;
    case J_ARRAY:
        for (size_t i = 0; i < v->u.arr.len; i++) json_free(v->u.arr.items[i]);
        free(v->u.arr.items);
        break;
    case J_OBJECT: {
        jmember *m = v->u.obj.head;
        while (m) {
            jmember *next = m->next;
            free(m->key);
            json_free(m->value);
            free(m);
            m = next;
        }
        break;
    }
    default:
        break;
    }
    free(v);
}

void json_set(json *obj, const char *key, json *v) {
    if (!obj || obj->type != J_OBJECT) {
        json_free(v);
        return;
    }
    /* replace an existing key, preserving position */
    for (jmember *m = obj->u.obj.head; m; m = m->next) {
        if (strcmp(m->key, key) == 0) {
            json_free(m->value);
            m->value = v;
            return;
        }
    }
    jmember *m = xcalloc(1, sizeof(*m));
    m->key = xstrdup(key);
    m->value = v;
    if (obj->u.obj.tail)
        obj->u.obj.tail->next = m;
    else
        obj->u.obj.head = m;
    obj->u.obj.tail = m;
}

void json_set_null(json *obj, const char *key) { json_set(obj, key, json_null()); }
void json_set_bool(json *obj, const char *key, bool v) { json_set(obj, key, json_bool(v)); }
void json_set_int(json *obj, const char *key, int64_t v) { json_set(obj, key, json_int(v)); }
void json_set_double(json *obj, const char *key, double v) { json_set(obj, key, json_double(v)); }
void json_set_str(json *obj, const char *key, const char *v) { json_set(obj, key, json_string(v)); }

void json_set_fmt(json *obj, const char *key, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        json_set_null(obj, key);
        return;
    }
    char *buf = xmalloc((size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    json_set(obj, key, json_string(buf));
    free(buf);
}

void json_add(json *arr, json *v) {
    if (!arr || arr->type != J_ARRAY) {
        json_free(v);
        return;
    }
    if (arr->u.arr.len == arr->u.arr.cap) {
        arr->u.arr.cap = arr->u.arr.cap ? arr->u.arr.cap * 2 : 4;
        arr->u.arr.items = xrealloc(arr->u.arr.items, arr->u.arr.cap * sizeof(json *));
    }
    arr->u.arr.items[arr->u.arr.len++] = v;
}

void json_add_str(json *arr, const char *v) { json_add(arr, json_string(v)); }
void json_add_int(json *arr, int64_t v) { json_add(arr, json_int(v)); }

const char *json_string_value(const json *v) {
    return (v && v->type == J_STRING) ? v->u.s : NULL;
}

json *json_get(const json *obj, const char *key) {
    if (!obj || obj->type != J_OBJECT) return NULL;
    for (jmember *m = obj->u.obj.head; m; m = m->next) {
        if (!strcmp(m->key, key)) return m->value;
    }
    return NULL;
}

bool json_get_bool(const json *obj, const char *key, bool def) {
    json *v = json_get(obj, key);
    return (v && v->type == J_BOOL) ? v->u.b : def;
}

bool json_is_object(const json *v) { return v && v->type == J_OBJECT; }

/* ------------------------------------------------------------------ */

static void dump_string(strbuf *b, const char *s) {
    sb_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\b': sb_puts(b, "\\b"); break;
        case '\f': sb_puts(b, "\\f"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\r': sb_puts(b, "\\r"); break;
        case '\t': sb_puts(b, "\\t"); break;
        default:
            if (*p < 0x20) {
                sb_printf(b, "\\u%04x", *p);
            } else {
                sb_putc(b, (char)*p);
            }
        }
    }
    sb_putc(b, '"');
}

static void dump_number(strbuf *b, const json *v) {
    if (v->type == J_INT) {
        sb_printf(b, "%lld", (long long)v->u.i);
        return;
    }
    double d = v->u.d;
    if (!isfinite(d)) {
        sb_puts(b, "null");
        return;
    }
    /* integral values are printed without a decimal point */
    if (d == (double)(int64_t)d && d >= -9e15 && d <= 9e15) {
        sb_printf(b, "%lld", (long long)d);
        return;
    }
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%.10g", d);
    sb_puts(b, tmp);
}

static void dump_value(strbuf *b, const json *v, bool pretty, int depth) {
    if (!v) {
        sb_puts(b, "null");
        return;
    }
    switch (v->type) {
    case J_NULL:
        sb_puts(b, "null");
        break;
    case J_BOOL:
        sb_puts(b, v->u.b ? "true" : "false");
        break;
    case J_INT:
    case J_DOUBLE:
        dump_number(b, v);
        break;
    case J_STRING:
        dump_string(b, v->u.s);
        break;
    case J_ARRAY:
        if (v->u.arr.len == 0) {
            sb_puts(b, "[]");
            break;
        }
        sb_putc(b, '[');
        for (size_t i = 0; i < v->u.arr.len; i++) {
            if (i) sb_putc(b, ',');
            if (pretty) {
                sb_putc(b, '\n');
                for (int k = 0; k <= depth; k++) sb_puts(b, "  ");
            }
            dump_value(b, v->u.arr.items[i], pretty, depth + 1);
        }
        if (pretty) {
            sb_putc(b, '\n');
            for (int k = 0; k < depth; k++) sb_puts(b, "  ");
        }
        sb_putc(b, ']');
        break;
    case J_OBJECT: {
        if (!v->u.obj.head) {
            sb_puts(b, "{}");
            break;
        }
        sb_putc(b, '{');
        bool first = true;
        for (jmember *m = v->u.obj.head; m; m = m->next) {
            if (!first) sb_putc(b, ',');
            first = false;
            if (pretty) {
                sb_putc(b, '\n');
                for (int k = 0; k <= depth; k++) sb_puts(b, "  ");
            }
            dump_string(b, m->key);
            sb_puts(b, pretty ? ": " : ":");
            dump_value(b, m->value, pretty, depth + 1);
        }
        if (pretty) {
            sb_putc(b, '\n');
            for (int k = 0; k < depth; k++) sb_puts(b, "  ");
        }
        sb_putc(b, '}');
        break;
    }
    }
}

char *json_dump(const json *v, bool pretty) {
    strbuf b;
    sb_init(&b);
    dump_value(&b, v, pretty, 0);
    return sb_take(&b);
}
