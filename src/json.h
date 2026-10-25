/*
 * json.h - minimal JSON value tree + serializer (no parser)
 */
#ifndef X68K_JSON_H
#define X68K_JSON_H

#include <stdbool.h>
#include <stdint.h>

typedef struct json json;

json *json_null(void);
json *json_bool(bool v);
json *json_int(int64_t v);
json *json_double(double v);
json *json_string(const char *s);
json *json_object(void);
json *json_array(void);

void json_free(json *v);

/* object setters (take ownership of v) */
void json_set(json *obj, const char *key, json *v);
void json_set_null(json *obj, const char *key);
void json_set_bool(json *obj, const char *key, bool v);
void json_set_int(json *obj, const char *key, int64_t v);
void json_set_double(json *obj, const char *key, double v);
void json_set_str(json *obj, const char *key, const char *v);
void json_set_fmt(json *obj, const char *key, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* array appenders (take ownership of v) */
void json_add(json *arr, json *v);
void json_add_str(json *arr, const char *v);
void json_add_int(json *arr, int64_t v);

const char *json_string_value(const json *v);
json *json_get(const json *obj, const char *key);
bool json_get_bool(const json *obj, const char *key, bool def);
bool        json_is_object(const json *v);

/* returns malloc'd serialization */
char *json_dump(const json *v, bool pretty);

#endif
