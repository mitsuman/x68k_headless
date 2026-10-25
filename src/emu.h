#ifndef X68K_EMU_H
#define X68K_EMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "keys.h"
#include "util.h"

typedef struct recorder recorder;
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

#endif
