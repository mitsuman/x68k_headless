#ifndef X68K_EMU_H
#define X68K_EMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "keys.h"
#include "util.h"

typedef struct recorder recorder;
typedef struct vgm_recorder vgm_recorder;
typedef struct emu emu;

typedef enum {
    JOY_2BUTTON = 0,
    JOY_CPSF_MD = 1,
    JOY_CPSF_SFC = 2,
    JOY_CYBERSTICK_D = 3,
    JOY_CYBERSTICK_A = 4,
} joy_type;

typedef struct {
    char home[X68K_PATH_MAX];        /* state dir, e.g. ~/.x68k */
    char rom_source[X68K_PATH_MAX];  /* dir containing iplrom.dat / cgrom.dat */
    char hdd[X68K_PATH_MAX];         /* empty = none */
    char fdd[2][X68K_PATH_MAX];      /* empty = none */
    int ram_mb;                      /* 1..12 */
    int cpu_mhz;                     /* 10,16,25,33,66,100 */
    bool compat_fps;                 /* true = 59.94/55.5, false = actual */
    int joy_type[2];
} emu_config;

typedef struct {
    uint64_t frames;
    double emu_seconds;
    int64_t wall_ms;
} emu_step_result;

/* ---- PCG / palette / video introspection -------------------------- */

#define EMU_PCG_PATTERNS  256
#define EMU_PCG16_TILE     16
#define EMU_PCG8_TILE       8
#define EMU_PALETTE_ENTRIES 256
#define EMU_SPRITE_COUNT   128

typedef struct {
    const uint8_t *indices; /* patterns * tile * tile, one palette index per byte */
    unsigned patterns;
    unsigned tile;
    unsigned bytes;
    bool active;            /* BG character size matches this tile */
} emu_pcg_view;

typedef struct {
    uint16_t raw[EMU_PALETTE_ENTRIES];   /* 0xGRBI hardware value */
    uint16_t rgb565[EMU_PALETTE_ENTRIES];/* actual host colour */
    uint8_t rgb[EMU_PALETTE_ENTRIES][3];
} emu_palette_map;

typedef struct {
    unsigned x, y;
    uint16_t ctrl;
    unsigned pattern;
    unsigned bank;
    unsigned priority;
} emu_sprite_info;

typedef struct {
    int chr_size;
    uint8_t bg_regs[0x12];
    bool bg0_on, bg1_on;
    unsigned bg0_scroll_x, bg0_scroll_y;
    unsigned bg1_scroll_x, bg1_scroll_y;
    uint8_t crtc_regs[48];
    uint8_t vc_regs[3][2];
    unsigned text_dot_x, text_dot_y;
    unsigned text_scroll_x, text_scroll_y;
    unsigned grph_scroll_x[4], grph_scroll_y[4];
    int sprite_count;
    emu_sprite_info sprites[EMU_SPRITE_COUNT];
} emu_video_state;

int  emu_get_pcg(emu *e, unsigned tile, emu_pcg_view *out);
void emu_palette_snapshot(emu *e, emu_palette_map *graphics, emu_palette_map *text);
void emu_video_snapshot(emu *e, emu_video_state *out);
int  emu_write_pcg_png(emu *e, unsigned tile, int bank, int scale, bool grid,
                       const char *path, unsigned *out_w, unsigned *out_h,
                       size_t *out_bytes, char *err, size_t errlen);
int  emu_write_pcg_bin(emu *e, unsigned tile, const char *path, size_t *out_bytes,
                       char *err, size_t errlen);

emu *emu_create(const emu_config *cfg, char *err, size_t errlen);
void emu_destroy(emu *e);
bool emu_is_booted(const emu *e);

/* copies/checks the ROM set into <home>/roms/keropi */
int emu_prepare_roms(const char *rom_source, const char *home, char *err, size_t errlen);

int emu_step_frames(emu *e, long long frames, emu_step_result *res, volatile int *stop_flag);
int emu_step_seconds(emu *e, double seconds, emu_step_result *res, volatile int *stop_flag);

bool emu_screen_ready(const emu *e);
void emu_screen_info(const emu *e, unsigned *w, unsigned *h, size_t *pitch);
uint64_t emu_screen_hash(emu *e);
int emu_snapshot(emu *e, const char *path, int scale, unsigned *out_w, unsigned *out_h,
                 size_t *out_bytes, char *err, size_t errlen);

/* input (all of these are instantaneous; callers advance time themselves) */
int emu_keys_event(emu *e, const key_action *acts, int n, bool down, char *err, size_t errlen);
int emu_joy_event(emu *e, int port, const char *button, bool down, char *err, size_t errlen);
int emu_mouse_move(emu *e, int dx, int dy, char *err, size_t errlen);
int emu_mouse_button(emu *e, const char *button, bool down, char *err, size_t errlen);
/* Advances frames until the queued relative movement has been consumed by
 * the guest.  delivered is false if the guest stopped polling (the remaining
 * movement is discarded) or the step was interrupted. */
int emu_mouse_flush(emu *e, emu_step_result *res, bool *delivered, volatile int *stop_flag);
void emu_mouse_pending(const emu *e, int *dx, int *dy);

/* floppy media */
int emu_insert_fdd(emu *e, int drive, const char *path, char *err, size_t errlen);
int emu_eject_fdd(emu *e, int drive, char *err, size_t errlen);
const char *emu_fdd_path(const emu *e, int drive);
bool emu_fdd_inserted(const emu *e, int drive);
const char *emu_hdd_path(const emu *e);

void emu_reset(emu *e);
int  emu_save_state(emu *e, const char *path, size_t *bytes, char *err, size_t errlen);
int  emu_load_state(emu *e, const char *path, char *err, size_t errlen);

double   emu_fps(const emu *e);
unsigned emu_ram_mb(const emu *e);
unsigned emu_cpu_mhz(const emu *e);
bool     emu_compat_fps(const emu *e);
uint64_t emu_frame(const emu *e);
double   emu_seconds(const emu *e);
const char *emu_core_version(void);
const char *emu_system_dir(const emu *e);
void     emu_av_info(emu *e, unsigned *w, unsigned *h, double *fps, double *sample_rate);

/* input introspection */
int      emu_held_keys(const emu *e, const char **names, int max);
int      emu_held_joy(const emu *e, int port, const char **names, int max);
unsigned emu_mouse_buttons(const emu *e);

void     emu_set_recorder(emu *e, recorder *r);
recorder *emu_get_recorder(const emu *e);
void     emu_set_vgm(emu *e, vgm_recorder *v);
vgm_recorder *emu_get_vgm(const emu *e);

#endif
