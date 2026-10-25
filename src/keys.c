#include "keys.h"
#include "util.h"

#include <ctype.h>
#include <libretro.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    unsigned retro;
    bool shift;
} retro_name;

static const retro_name retro_keys[] = {
    {"return", RETROK_RETURN, false},
    {"enter", RETROK_RETURN, false},
    {"esc", RETROK_ESCAPE, false},
    {"escape", RETROK_ESCAPE, false},
    {"space", RETROK_SPACE, false},
    {"tab", RETROK_TAB, false},
    {"backspace", RETROK_BACKSPACE, false},
    {"bs", RETROK_BACKSPACE, false},
    {"up", RETROK_UP, false},
    {"down", RETROK_DOWN, false},
    {"left", RETROK_LEFT, false},
    {"right", RETROK_RIGHT, false},
    {"home", RETROK_HOME, false},
    {"end", RETROK_END, false},
    {"insert", RETROK_INSERT, false},
    {"ins", RETROK_INSERT, false},
    {"pageup", RETROK_PAGEUP, false},
    {"pgup", RETROK_PAGEUP, false},
    {"pagedown", RETROK_PAGEDOWN, false},
    {"pgdn", RETROK_PAGEDOWN, false},
    {"shift", RETROK_LSHIFT, false},
    {"lshift", RETROK_LSHIFT, false},
    {"rshift", RETROK_RSHIFT, false},
    {"ctrl", RETROK_LCTRL, false},
    {"control", RETROK_LCTRL, false},
    {"lctrl", RETROK_LCTRL, false},
    {"rctrl", RETROK_RCTRL, false},
    {"numlock", RETROK_NUMLOCK, false},
    {"scrolllock", RETROK_SCROLLOCK, false},
    {"printscreen", RETROK_PRINT, false},
    {"pause", RETROK_PAUSE, false},
    {"menu", RETROK_MENU, false},
    {"compose", RETROK_COMPOSE, false},
    /* punctuation names */
    {"minus", RETROK_MINUS, false},
    {"hyphen", RETROK_MINUS, false},
    {"equal", RETROK_EQUALS, false},
    {"equals", RETROK_EQUALS, false},
    {"plus", RETROK_EQUALS, true},
    {"comma", RETROK_COMMA, false},
    {"period", RETROK_PERIOD, false},
    {"dot", RETROK_PERIOD, false},
    {"slash", RETROK_SLASH, false},
    {"backslash", RETROK_BACKSLASH, false},
    {"semicolon", RETROK_SEMICOLON, false},
    {"apostrophe", RETROK_QUOTE, false},
    {"quote", RETROK_QUOTE, false},
    {"grave", RETROK_BACKQUOTE, false},
    {"backquote", RETROK_BACKQUOTE, false},
    {"lbracket", RETROK_LEFTBRACKET, false},
    {"rbracket", RETROK_RIGHTBRACKET, false},
    {"leftbracket", RETROK_LEFTBRACKET, false},
    {"rightbracket", RETROK_RIGHTBRACKET, false},
    /* keypad */
    {"kp0", RETROK_KP0, false}, {"kp1", RETROK_KP1, false}, {"kp2", RETROK_KP2, false},
    {"kp3", RETROK_KP3, false}, {"kp4", RETROK_KP4, false}, {"kp5", RETROK_KP5, false},
    {"kp6", RETROK_KP6, false}, {"kp7", RETROK_KP7, false}, {"kp8", RETROK_KP8, false},
    {"kp9", RETROK_KP9, false},
    {"kp_plus", RETROK_KP_PLUS, false},
    {"kp_minus", RETROK_KP_MINUS, false},
    {"kp_multiply", RETROK_KP_MULTIPLY, false},
    {"kp_divide", RETROK_KP_DIVIDE, false},
    {"kp_enter", RETROK_KP_ENTER, false},
};

typedef struct {
    const char *name;
    unsigned code;
} raw_name;

/* X68000 scan codes which the libretro keyboard path cannot reach.
 * Reference: gamesx.com X68000 keycode table (verified). */
static const raw_name raw_keys[] = {
    {"xf1", 0x55}, {"xf2", 0x56}, {"xf3", 0x57}, {"xf4", 0x58}, {"xf5", 0x59},
    {"kana", 0x5a}, {"romaji", 0x5b}, {"code", 0x5c},
    {"help", 0x54}, {"hiragana", 0x5f}, {"zenkaku", 0x60},
    {"touroku", 0x53}, {"kigou", 0x52}, {"caps", 0x5d},
    {"undo", 0x3a}, {"clr", 0x3f}, {"break", 0x61}, {"copy", 0x62},
    {"delete", 0x37}, {"del", 0x37},
    {"opt1", 0x72}, {"opt2", 0x73},
};

/* Characters which are produced with shift on a US layout. */
static int shifted_ascii(unsigned char c, unsigned *base) {
    switch (c) {
    case '!': *base = '1'; return 1;
    case '@': *base = '2'; return 1;
    case '#': *base = '3'; return 1;
    case '$': *base = '4'; return 1;
    case '%': *base = '5'; return 1;
    case '^': *base = '6'; return 1;
    case '&': *base = '7'; return 1;
    case '*': *base = '8'; return 1;
    case '(': *base = '9'; return 1;
    case ')': *base = '0'; return 1;
    case '_': *base = '-'; return 1;
    case '+': *base = '='; return 1;
    case '{': *base = '['; return 1;
    case '}': *base = ']'; return 1;
    case '|': *base = '\\'; return 1;
    case ':': *base = ';'; return 1;
    case '"': *base = '\''; return 1;
    case '<': *base = ','; return 1;
    case '>': *base = '.'; return 1;
    case '?': *base = '/'; return 1;
    case '~': *base = '`'; return 1;
    default: return 0;
    }
}

static void set_name(key_action *out, const char *s) {
    snprintf(out->name, sizeof(out->name), "%s", s);
}

int key_lookup(const char *token, key_action *out, char *err, size_t errlen) {
    memset(out, 0, sizeof(*out));
    if (!token || !*token) {
        snprintf(err, errlen, "empty key name");
        return -1;
    }

    /* single printable character: use the ASCII key code (libretro matches ASCII) */
    if (token[1] == '\0') {
        unsigned char c = (unsigned char)token[0];
        if (c == ' ') {
            out->kind = KEY_KIND_RETRO;
            out->code = RETROK_SPACE;
            set_name(out, "space");
            return 0;
        }
        if (c >= 0x20 && c < 0x7f) {
            unsigned base = c;
            bool shift = false;
            if (c >= 'A' && c <= 'Z') {
                base = (unsigned)tolower(c);
                shift = true;
            } else if (shifted_ascii(c, &base)) {
                shift = true;
            }
            out->kind = KEY_KIND_RETRO;
            out->code = base;
            out->shift = shift;
            set_name(out, token);
            return 0;
        }
        snprintf(err, errlen, "unsupported single-character key 0x%02x", c);
        return -1;
    }

    char lower[64];
    if (strlen(token) >= sizeof(lower)) {
        snprintf(err, errlen, "key name too long");
        return -1;
    }
    size_t i;
    for (i = 0; token[i]; i++) lower[i] = (char)tolower((unsigned char)token[i]);
    lower[i] = '\0';

    for (size_t k = 0; k < sizeof(retro_keys) / sizeof(retro_keys[0]); k++) {
        if (!strcmp(lower, retro_keys[k].name)) {
            out->kind = KEY_KIND_RETRO;
            out->code = retro_keys[k].retro;
            out->shift = retro_keys[k].shift;
            set_name(out, retro_keys[k].name);
            return 0;
        }
    }
    if (lower[0] == 'f' && isdigit((unsigned char)lower[1])) {
        char *end = NULL;
        long n = strtol(lower + 1, &end, 10);
        if (end && *end == '\0' && n >= 1 && n <= 10) {
            out->kind = KEY_KIND_RETRO;
            out->code = (unsigned)(RETROK_F1 + (n - 1));
            snprintf(out->name, sizeof(out->name), "f%ld", n);
            return 0;
        }
        snprintf(err, errlen, "only f1..f10 exist on the X68000 keyboard");
        return -1;
    }
    for (size_t k = 0; k < sizeof(raw_keys) / sizeof(raw_keys[0]); k++) {
        if (!strcmp(lower, raw_keys[k].name)) {
            out->kind = KEY_KIND_RAW;
            out->code = raw_keys[k].code;
            set_name(out, raw_keys[k].name);
            return 0;
        }
    }

    snprintf(err, errlen,
             "unknown key '%s' (see 'x68k help keys'; single printable characters are accepted too)",
             token);
    return -1;
}

static int add_action(key_action *acts, int n, int max, key_action a) {
    for (int i = 0; i < n; i++) {
        if (acts[i].kind == a.kind && acts[i].code == a.code) return n;
    }
    if (n >= max) return -2;
    acts[n] = a;
    return n + 1;
}

int key_parse_chord(const char *spec, key_action *acts, int max_acts, char *err, size_t errlen) {
    if (!spec || !*spec) {
        snprintf(err, errlen, "empty key/chord");
        return -1;
    }
    int n = 0;
    bool need_shift = false;

    /* A chord is '+'-separated.  A literal plus is written as "plus". */
    const char *p = spec;
    char token[64];
    while (*p) {
        const char *plus = strchr(p, '+');
        size_t len = plus ? (size_t)(plus - p) : strlen(p);
        if (len == 0 || len >= sizeof(token)) {
            snprintf(err, errlen, "invalid chord '%s'", spec);
            return -1;
        }
        memcpy(token, p, len);
        token[len] = '\0';

        key_action a;
        if (key_lookup(token, &a, err, errlen) != 0) return -1;
        if (a.shift) need_shift = true;
        a.shift = false;
        n = add_action(acts, n, max_acts, a);
        if (n < 0) {
            snprintf(err, errlen, "too many keys in chord");
            return -1;
        }
        if (!plus) break;
        p = plus + 1;
        if (!*p) {
            snprintf(err, errlen, "chord ends with '+' (use 'plus' for the + key)");
            return -1;
        }
    }

    if (need_shift) {
        key_action shift = {KEY_KIND_RETRO, RETROK_LSHIFT, false, "lshift"};
        n = add_action(acts, n, max_acts, shift);
        if (n < 0) {
            snprintf(err, errlen, "too many keys in chord");
            return -1;
        }
    }
    return n;
}

int key_from_char(unsigned cp, key_action *out) {
    char s[2];
    char err[128];
    s[0] = (char)cp;
    s[1] = '\0';
    if (cp == '\n' || cp == '\r') {
        memset(out, 0, sizeof(*out));
        out->kind = KEY_KIND_RETRO;
        out->code = RETROK_RETURN;
        set_name(out, "return");
        return 0;
    }
    if (cp == '\t') {
        memset(out, 0, sizeof(*out));
        out->kind = KEY_KIND_RETRO;
        out->code = RETROK_TAB;
        set_name(out, "tab");
        return 0;
    }
    if (cp == '\b') {
        memset(out, 0, sizeof(*out));
        out->kind = KEY_KIND_RETRO;
        out->code = RETROK_BACKSPACE;
        set_name(out, "backspace");
        return 0;
    }
    if (cp == 27) {
        memset(out, 0, sizeof(*out));
        out->kind = KEY_KIND_RETRO;
        out->code = RETROK_ESCAPE;
        set_name(out, "esc");
        return 0;
    }
    if (cp < 0x20 || cp > 0x7e) return -1;
    return key_lookup(s, out, err, sizeof(err));
}

char *keys_help_text(void) {
    strbuf b;
    sb_init(&b);
    sb_puts(&b,
        "Key names\n"
        "---------\n"
        "letters a-z, digits 0-9, and any printable single character.\n"
        "  (uppercase letters and shifted symbols automatically hold shift)\n"
        "\n"
        "named keys:\n"
        "  return enter esc escape space tab backspace bs\n"
        "  up down left right home end insert pageup pgup pagedown pgdn\n"
        "  shift lshift rshift ctrl control lctrl rctrl numlock scrolllock\n"
        "  f1 f2 f3 f4 f5 f6 f7 f8 f9 f10\n"
        "  minus equal plus comma period dot slash backslash semicolon apostrophe quote\n"
        "  grave backquote lbracket rbracket printscreen pause menu compose\n"
        "  kp0..kp9 kp_plus kp_minus kp_multiply kp_divide kp_enter\n"
        "\n"
        "X68000 specific keys:\n"
        "  xf1 xf2 xf3 xf4 xf5 opt1 opt2 kana romaji code help\n"
        "  hiragana zenkaku touroku kigou caps delete undo clr break copy\n"
        "\n"
        "chords: join with '+' (use 'plus' for the + key), e.g. ctrl+shift+s, shift+up\n");
    return sb_take(&b);
}
