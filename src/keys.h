/*
 * keys.h - mapping of human/LLM friendly key names to X68000 input
 */
#ifndef X68K_KEYS_H
#define X68K_KEYS_H

#include <stdbool.h>
#include <stddef.h>

#define KEY_NAME_MAX 32

typedef enum {
    KEY_KIND_RETRO, /* goes through libretro RETRO_DEVICE_KEYBOARD */
    KEY_KIND_RAW    /* send_keycode(): X68000 scan code (XF keys, OPT, ...) */
} key_kind;

typedef struct {
    key_kind kind;
    unsigned code;            /* RETROK_* or raw scancode */
    bool shift;               /* implies holding shift as well */
    char name[KEY_NAME_MAX];  /* name as written by the caller */
} key_action;

/* Look up one token.  Returns 0 on success, -1 if unknown (err filled). */
int key_lookup(const char *token, key_action *out, char *err, size_t errlen);

/* Parse a chord like "ctrl+shift+s" or "!".  Fills acts (max acts).
 * Returns number of actions (>=1) or -1 on error. */
int key_parse_chord(const char *spec, key_action *acts, int max_acts, char *err, size_t errlen);

/* Convert an ASCII character to a key action (with shift when needed).
 * Returns 0 on success, -1 if unsupported. */
int key_from_char(unsigned cp, key_action *out);

/* Human readable list of key names (malloc'd). */
char *keys_help_text(void);

#endif
