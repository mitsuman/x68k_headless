#include "emu.h"

#include <libretro.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "log.h"
#include "png.h"
#include "recorder.h"
#include "vgm.h"

/* px68k internals we deliberately touch: only this flag, to make the core
 * publish its real refresh rate after the first boot frame. */
extern int CHANGEAV_TIMING;
/* raw X68000 key events (see libretro/keyboard.h); flag 1 = down, 2 = up */
extern void send_keycode(uint8_t code, int flag);

#define PX68K_KEY_DOWN 1
#define PX68K_KEY_UP 2

#define MAX_HELD_KEYS 24
#define KEY_STATE_SLOTS 512

typedef struct {
    key_kind kind;
    unsigned code;
    char name[KEY_NAME_MAX];
} held_key;

struct emu {
    emu_config cfg;
    char system_root[X68K_PATH_MAX];
    char core_version[160];

    bool booted;
    double fps;
    double sample_rate;
    uint64_t frame;
    double seconds;

    const uint16_t *pixels;
    unsigned width, height;
    size_t pitch;
    bool video_updated;
    uint64_t screen_hash;
    bool hash_valid;

    bool retro_keys[KEY_STATE_SLOTS];
    unsigned joy_mask[2];
    int mouse_dx, mouse_dy;
    unsigned mouse_buttons;

    held_key held[MAX_HELD_KEYS];
    int nheld;

    int16_t *audio_buf;
    size_t audio_len, audio_cap;

    recorder *rec;
    vgm_recorder *vgm;

    int disk_drive;
    bool fdd_inserted[2];
    char fdd_path[2][X68K_PATH_MAX];
    char hdd_path[X68K_PATH_MAX];
};

static emu *g_emu;

static const struct retro_disk_control_ext_callback *g_disk_ext;
static const struct retro_disk_control_callback *g_disk_legacy;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static int fail(char *err, size_t errlen, const char *fmt, ...) {
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

static const char *joy_type_name(int t) {
    switch (t) {
    case JOY_CPSF_MD: return "CPSF-MD (8 Buttons)";
    case JOY_CPSF_SFC: return "CPSF-SFC (8 Buttons)";
    case JOY_CYBERSTICK_D: return "Cyberstick (Digital)";
    case JOY_CYBERSTICK_A: return "Cyberstick (Analog)";
    case JOY_2BUTTON:
    default: return "Default (2 Buttons)";
    }
}

static const char *cpu_speed_name(int mhz) {
    switch (mhz) {
    case 16: return "16Mhz";
    case 25: return "25Mhz";
    case 33: return "33Mhz (OC)";
    case 66: return "66Mhz (OC)";
    case 100: return "100Mhz (OC)";
    case 10:
    default: return "10Mhz";
    }
}

/* ------------------------------------------------------------------ */
/* libretro callbacks                                                  */
/* ------------------------------------------------------------------ */

static void core_log_cb(enum retro_log_level level, const char *fmt, ...) {
    (void)level;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    xlog("core: %s", buf);
}

static bool env_cb(unsigned cmd, void *data) {
    emu *e = g_emu;
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = e ? e->system_root : ".";
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *var = data;
        const char *key = var->key;
        const char *val = NULL;
        static __thread char rambuf[16];
        if (!e) return false;
        if (!strcmp(key, "px68k_no_wait_mode")) val = "enabled";
        else if (!strcmp(key, "px68k_frameskip")) val = "Full Frame";
        else if (!strcmp(key, "px68k_adjust_frame_rates")) val = e->cfg.compat_fps ? "enabled" : "disabled";
        else if (!strcmp(key, "px68k_ramsize")) {
            snprintf(rambuf, sizeof(rambuf), "%dMB", e->cfg.ram_mb);
            val = rambuf;
        }
        else if (!strcmp(key, "px68k_cpuspeed")) val = cpu_speed_name(e->cfg.cpu_mhz);
        else if (!strcmp(key, "px68k_joy_mouse")) val = "Mouse";
        else if (!strcmp(key, "px68k_joytype1")) val = joy_type_name(e->cfg.joy_type[0]);
        else if (!strcmp(key, "px68k_joytype2")) val = joy_type_name(e->cfg.joy_type[1]);
        else if (!strcmp(key, "px68k_disk_drive")) val = e->disk_drive ? "FDD1" : "FDD0";
        else if (!strcmp(key, "px68k_save_fdd_path")) val = "disabled";
        else if (!strcmp(key, "px68k_save_hdd_path")) val = "disabled";
        else if (!strcmp(key, "px68k_audio_desync_hack")) val = "disabled";
        else if (!strcmp(key, "px68k_text_off")) val = "disabled";
        else if (!strcmp(key, "px68k_grp_off")) val = "disabled";
        else if (!strcmp(key, "px68k_sp_off")) val = "disabled";
        else if (!strcmp(key, "px68k_menufontsize")) val = "normal";
        else if (!strcmp(key, "px68k_joy1_select")) val = "none";
        else if (!strcmp(key, "px68k_vbtn_swap")) val = "TRIG2 TRIG1";
        else if (!strcmp(key, "px68k_analog")) val = "disabled";
        else if (!strcmp(key, "px68k_rumble_on_disk_read")) val = "disabled";
        else if (!strcmp(key, "px68k_midi_output")) val = "disabled";
        else if (!strcmp(key, "px68k_midi_output_type")) val = "GM";
        else if (!strcmp(key, "px68k_opm_vol")) val = "12";
        else if (!strcmp(key, "px68k_adpcm_vol")) val = "15";
        else if (!strcmp(key, "px68k_mercury_vol")) val = "13";
        if (!val) return false;
        var->value = val;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log_cb;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
        const struct retro_system_av_info *av = data;
        if (e) {
            if (av->timing.fps > 1.0) e->fps = av->timing.fps;
            if (av->timing.sample_rate > 1000.0) e->sample_rate = av->timing.sample_rate;
        }
        return true;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
        g_disk_legacy = data;
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
        g_disk_ext = data;
        return true;
    case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
        *(unsigned *)data = 1;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        return true;
    default:
        return false;
    }
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch) {
    emu *e = g_emu;
    if (!e || !data) return;
    e->pixels = data;
    e->width = width;
    e->height = height;
    e->pitch = pitch;
    e->video_updated = true;
    e->hash_valid = false;
}

static void audio_sample_cb(int16_t left, int16_t right) {
    int16_t frame[2] = {left, right};
    if (!g_emu || !g_emu->rec) return;
    emu *e = g_emu;
    size_t need = e->audio_len + 2;
    if (need > e->audio_cap) {
        if (e->audio_cap > (1u << 21)) return; /* > 2M samples, drop */
        size_t cap = e->audio_cap ? e->audio_cap * 2 : 2048;
        while (cap < need) cap *= 2;
        e->audio_buf = xrealloc(e->audio_buf, cap * sizeof(int16_t));
        e->audio_cap = cap;
    }
    e->audio_buf[e->audio_len++] = frame[0];
    e->audio_buf[e->audio_len++] = frame[1];
}

static size_t audio_batch_cb(const int16_t *data, size_t frames) {
    emu *e = g_emu;
    if (!e || !e->rec || !data || !frames) return frames;
    size_t need = e->audio_len + frames * 2;
    if (need > e->audio_cap) {
        if (e->audio_cap > (1u << 21)) return frames;
        size_t cap = e->audio_cap ? e->audio_cap : 2048;
        while (cap < need) cap *= 2;
        e->audio_buf = xrealloc(e->audio_buf, cap * sizeof(int16_t));
        e->audio_cap = cap;
    }
    memcpy(e->audio_buf + e->audio_len, data, frames * 2 * sizeof(int16_t));
    e->audio_len += frames * 2;
    return frames;
}

static void input_poll_cb(void) {}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)index;
    emu *e = g_emu;
    if (!e) return 0;
    switch (device) {
    case RETRO_DEVICE_KEYBOARD:
        return (id < KEY_STATE_SLOTS && e->retro_keys[id]) ? 1 : 0;
    case RETRO_DEVICE_JOYPAD:
        if (port > 1) return 0;
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
            return (int16_t)(e->joy_mask[port] & 0xffffu);
        if (id < 32) return (int16_t)((e->joy_mask[port] >> id) & 1u);
        return 0;
    case RETRO_DEVICE_MOUSE: {
        /* The core reads X and Y once per frame.  Feed at most 100 units per
         * frame so that large moves are not clamped away by the guest. */
        if (id == RETRO_DEVICE_ID_MOUSE_X) {
            int v = e->mouse_dx;
            if (v > 100) v = 100;
            if (v < -100) v = -100;
            e->mouse_dx -= v;
            return (int16_t)v;
        }
        if (id == RETRO_DEVICE_ID_MOUSE_Y) {
            int v = e->mouse_dy;
            if (v > 100) v = 100;
            if (v < -100) v = -100;
            e->mouse_dy -= v;
            return (int16_t)v;
        }
        if (id == RETRO_DEVICE_ID_MOUSE_LEFT) return (e->mouse_buttons & 1) ? 1 : 0;
        if (id == RETRO_DEVICE_ID_MOUSE_RIGHT) return (e->mouse_buttons & 2) ? 1 : 0;
        return 0;
    }
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* ROM preparation                                                     */
/* ------------------------------------------------------------------ */

static int copy_if_needed(const char *src, const char *dst, char *err, size_t errlen) {
    struct stat ss, ds;
    if (stat(src, &ss) != 0) return fail(err, errlen, "cannot stat %s: %s", src, strerror(errno));
    if (stat(dst, &ds) == 0 && ds.st_size == ss.st_size && ds.st_mtime >= ss.st_mtime)
        return 0;

    char tmpname[X68K_PATH_MAX];
    snprintf(tmpname, sizeof(tmpname), "%s.tmp.%ld", dst, (long)getpid());
    FILE *in = fopen(src, "rb");
    if (!in) {
        return fail(err, errlen, "cannot open %s: %s", src, strerror(errno));
    }
    FILE *out = fopen(tmpname, "wb");
    if (!out) {
        fclose(in);
        return fail(err, errlen, "cannot create %s: %s", tmpname, strerror(errno));
    }
    char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (ferror(in)) rc = -1;
    fclose(in);
    if (fclose(out) != 0) rc = -1;
    if (rc != 0) {
        unlink(tmpname);
        return fail(err, errlen, "failed to copy %s to %s", src, tmpname);
    }
    if (rename(tmpname, dst) != 0) {
        unlink(tmpname);
        return fail(err, errlen, "failed to install %s: %s", dst, strerror(errno));
    }
    return 0;
}

int emu_prepare_roms(const char *rom_source, const char *home, char *err, size_t errlen) {
    if (!rom_source || !*rom_source) return fail(err, errlen, "no ROM directory configured");
    if (!path_is_dir(rom_source))
        return fail(err, errlen, "ROM directory does not exist: %s", rom_source);

    char *romroot = path_join(home, "roms");
    char *keropi = path_join(romroot, "keropi");
    free(romroot);
    if (mkdir_p(keropi, 0755) != 0) {
        int r = fail(err, errlen, "cannot create %s: %s", keropi, strerror(errno));
        free(keropi);
        return r;
    }

    static const char *ipl_candidates[] = {"iplrom.dat", "iplrom30.dat", "iplromco.dat", "iplromxv.dat"};
    char *ipl_src = NULL;
    for (size_t i = 0; i < sizeof(ipl_candidates) / sizeof(ipl_candidates[0]); i++) {
        ipl_src = find_file_ci(rom_source, ipl_candidates[i]);
        if (ipl_src) break;
    }
    if (!ipl_src) {
        int r = fail(err, errlen,
                     "iplrom.dat not found in %s (ROM images are not bundled; point --roms at a directory "
                     "containing iplrom.dat and cgrom.dat)", rom_source);
        free(keropi);
        return r;
    }

    char *cg_src = find_file_ci(rom_source, "cgrom.dat");
    if (!cg_src) cg_src = find_file_ci(rom_source, "cgrom.tmp");
    if (!cg_src) {
        free(ipl_src);
        int r = fail(err, errlen, "cgrom.dat not found in %s", rom_source);
        free(keropi);
        return r;
    }

    struct stat st;
    if (stat(ipl_src, &st) == 0 && st.st_size < 0x20000) {
        xlog("warning: %s is smaller than 128KB; boot may fail", ipl_src);
    }
    if (stat(cg_src, &st) == 0 && st.st_size < 0xc0000) {
        xlog("warning: %s is smaller than 768KB; fonts may be incomplete", cg_src);
    }

    char *ipl_dst = path_join(keropi, "iplrom.dat");
    char *cg_dst = path_join(keropi, "cgrom.dat");
    int rc = 0;
    if (copy_if_needed(ipl_src, ipl_dst, err, errlen) != 0) rc = -1;
    if (rc == 0 && copy_if_needed(cg_src, cg_dst, err, errlen) != 0) rc = -1;

    xlog("ROMs: %s, %s -> %s", ipl_src, cg_src, keropi);

    free(ipl_src);
    free(cg_src);
    free(ipl_dst);
    free(cg_dst);
    free(keropi);
    return rc;
}

/* ------------------------------------------------------------------ */
/* create / destroy                                                    */
/* ------------------------------------------------------------------ */

emu *emu_create(const emu_config *cfg, char *err, size_t errlen) {
    emu *e = xcalloc(1, sizeof(*e));
    e->cfg = *cfg;
    e->fps = 61.46;
    e->sample_rate = 44100.0;
    e->disk_drive = 0;
    snprintf(e->system_root, sizeof(e->system_root), "%s/roms", cfg->home);
    snprintf(e->hdd_path, sizeof(e->hdd_path), "%s", cfg->hdd);

    g_emu = e;
    g_disk_ext = NULL;
    g_disk_legacy = NULL;

    struct retro_system_info si;
    memset(&si, 0, sizeof(si));
    retro_get_system_info(&si);
    snprintf(e->core_version, sizeof(e->core_version), "%s %s", si.library_name, si.library_version);

    retro_set_environment(env_cb);
    retro_init();
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_sample_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);

    struct retro_game_info gi;
    memset(&gi, 0, sizeof(gi));
    bool loaded;
    if (e->hdd_path[0]) {
        gi.path = e->hdd_path;
        loaded = retro_load_game(&gi);
    } else {
        loaded = retro_load_game(NULL);
    }
    if (!loaded) {
        fail(err, errlen, "the emulator core rejected the media (HDD: %s)",
             e->hdd_path[0] ? e->hdd_path : "(none)");
        goto error;
    }

    struct retro_system_av_info av;
    retro_get_system_av_info(&av);
    if (av.timing.fps > 1.0) e->fps = av.timing.fps;
    if (av.timing.sample_rate > 1000.0) e->sample_rate = av.timing.sample_rate;

    /* First retro_run performs the machine boot (pre_main).  It must happen
     * before floppy insertion because it initialises the FDC/FDD layer. */
    retro_run();

    if (!g_disk_ext && !g_disk_legacy) {
        fail(err, errlen, "emulator core did not expose a disk control interface");
        goto error;
    }
    /* Reserve one image slot per floppy drive. */
    for (int i = 0; i < 2; i++) {
        if (g_disk_ext) {
            if (!g_disk_ext->add_image_index()) {
                fail(err, errlen, "failed to reserve floppy image slot %d", i);
                goto error;
            }
        } else {
            if (!g_disk_legacy->add_image_index()) {
                fail(err, errlen, "failed to reserve floppy image slot %d", i);
                goto error;
            }
        }
    }

    for (int d = 0; d < 2; d++) {
        e->fdd_inserted[d] = false;
        e->fdd_path[d][0] = '\0';
    }
    for (int d = 0; d < 2; d++) {
        if (e->cfg.fdd[d][0]) {
            if (emu_insert_fdd(e, d, e->cfg.fdd[d], err, errlen) != 0) goto error;
        }
    }

    /* Ask the core to publish its actual refresh rate on the next frame
     * (the frontend option is only applied after pre_main). */
    CHANGEAV_TIMING = 1;

    e->booted = true;
    xlog("machine booted: ram=%dMB cpu=%dMHz fps=%.3f core=%s",
         e->cfg.ram_mb, e->cfg.cpu_mhz, e->fps, e->core_version);
    return e;

error:
    xlog("emu_create failed: %s", err ? err : "unknown");
    retro_unload_game();
    retro_deinit();
    g_emu = NULL;
    free(e->audio_buf);
    free(e);
    return NULL;
}

void emu_destroy(emu *e) {
    if (!e) return;
    if (e->booted) {
        retro_unload_game();
        retro_deinit();
    }
    g_emu = NULL;
    g_disk_ext = NULL;
    g_disk_legacy = NULL;
    free(e->audio_buf);
    free(e);
}

bool emu_is_booted(const emu *e) { return e && e->booted; }

/* ------------------------------------------------------------------ */
/* stepping                                                            */
/* ------------------------------------------------------------------ */

static void run_one_frame(emu *e) {
    e->audio_len = 0;
    e->video_updated = false;
    double fps = e->fps > 1.0 ? e->fps : 60.0;
    if (e->vgm) vgm_begin_frame(e->vgm, fps);
    retro_run();
    if (e->vgm) vgm_end_frame(e->vgm);
    e->frame++;
    fps = e->fps > 1.0 ? e->fps : 60.0;
    e->seconds += 1.0 / fps;

    if (e->rec) {
        size_t frames = e->audio_len / 2;
        recorder_push_frame(e->rec, e->pixels, e->width, e->height, e->pitch,
                            e->audio_buf, frames, fps);
    }
}

int emu_step_frames(emu *e, long long frames, emu_step_result *res, volatile int *stop_flag) {
    if (!e || !e->booted) return -1;
    if (frames < 0) return -1;
    int64_t t0 = now_ms_monotonic();
    uint64_t f0 = e->frame;
    double s0 = e->seconds;
    for (long long i = 0; i < frames; i++) {
        if (stop_flag && *stop_flag) break;
        run_one_frame(e);
    }
    if (res) {
        res->frames = e->frame - f0;
        res->emu_seconds = e->seconds - s0;
        res->wall_ms = now_ms_monotonic() - t0;
    }
    return 0;
}

int emu_step_seconds(emu *e, double seconds, emu_step_result *res, volatile int *stop_flag) {
    if (!e || !e->booted) return -1;
    if (seconds <= 0) return -1;
    int64_t t0 = now_ms_monotonic();
    uint64_t f0 = e->frame;
    double s0 = e->seconds;
    while (e->seconds - s0 < seconds) {
        if (stop_flag && *stop_flag) break;
        run_one_frame(e);
        if (e->frame - f0 > 20000000ULL) break;
    }
    if (res) {
        res->frames = e->frame - f0;
        res->emu_seconds = e->seconds - s0;
        res->wall_ms = now_ms_monotonic() - t0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* screen                                                              */
/* ------------------------------------------------------------------ */

bool emu_screen_ready(const emu *e) { return e && e->pixels && e->width && e->height; }

void emu_screen_info(const emu *e, unsigned *w, unsigned *h, size_t *pitch) {
    if (w) *w = e ? e->width : 0;
    if (h) *h = e ? e->height : 0;
    if (pitch) *pitch = e ? e->pitch : 0;
}

uint64_t emu_screen_hash(emu *e) {
    if (!e || !e->pixels) return 0;
    if (e->hash_valid) return e->screen_hash;
    uint64_t h = 0xcbf29ce484222325ULL;
    uint64_t dims[2] = {e->width, e->height};
    h = fnv1a64_update(h, dims, sizeof(dims));
    size_t rowbytes = (size_t)e->width * 2;
    for (unsigned y = 0; y < e->height; y++) {
        h = fnv1a64_update(h, (const uint8_t *)e->pixels + (size_t)y * e->pitch, rowbytes);
    }
    e->screen_hash = h;
    e->hash_valid = true;
    return h;
}

static void rgb565_to_rgb24(uint16_t p, uint8_t *out) {
    unsigned r = (p >> 11) & 0x1f;
    unsigned g = (p >> 5) & 0x3f;
    unsigned b = p & 0x1f;
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 2) | (g >> 4));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
}

static int make_rgb24(emu *e, int scale, uint8_t **out, unsigned *ow, unsigned *oh) {
    if (!e || !e->pixels || !e->width || !e->height) return -1;
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    uint64_t w = (uint64_t)e->width * (unsigned)scale;
    uint64_t h = (uint64_t)e->height * (unsigned)scale;
    if (w == 0 || h == 0 || w > 16384 || h > 16384) return -1;
    size_t stride = (size_t)w * 3;
    uint8_t *buf = xmalloc(stride * (size_t)h);
    for (unsigned sy = 0; sy < e->height; sy++) {
        const uint16_t *src = (const uint16_t *)((const uint8_t *)e->pixels + (size_t)sy * e->pitch);
        for (unsigned sx = 0; sx < e->width; sx++) {
            uint8_t rgb[3];
            rgb565_to_rgb24(src[sx], rgb);
            for (int yy = 0; yy < scale; yy++) {
                uint8_t *dst = buf + (size_t)(sy * (unsigned)scale + (unsigned)yy) * stride +
                               (size_t)sx * (unsigned)scale * 3;
                for (int xx = 0; xx < scale; xx++) {
                    dst[0] = rgb[0];
                    dst[1] = rgb[1];
                    dst[2] = rgb[2];
                    dst += 3;
                }
            }
        }
    }
    *out = buf;
    if (ow) *ow = (unsigned)w;
    if (oh) *oh = (unsigned)h;
    return 0;
}

int emu_snapshot(emu *e, const char *path, int scale, unsigned *out_w, unsigned *out_h,
                 size_t *out_bytes, char *err, size_t errlen) {
    if (!e || !e->pixels) return fail(err, errlen, "no screen frame is available yet");
    uint8_t *rgb = NULL;
    unsigned w = 0, h = 0;
    if (make_rgb24(e, scale, &rgb, &w, &h) != 0)
        return fail(err, errlen, "could not convert the screen buffer");
    int rc = png_write_rgb(path, rgb, w, h);
    free(rgb);
    if (rc != 0) return fail(err, errlen, "failed to write PNG: %s", path);
    struct stat st;
    if (stat(path, &st) == 0 && out_bytes) *out_bytes = (size_t)st.st_size;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return 0;
}

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */

static int held_index(const emu *e, key_kind kind, unsigned code) {
    for (int i = 0; i < e->nheld; i++) {
        if (e->held[i].kind == kind && e->held[i].code == code) return i;
    }
    return -1;
}

static void held_add(emu *e, key_kind kind, unsigned code, const char *name) {
    if (held_index(e, kind, code) >= 0) return;
    if (e->nheld >= MAX_HELD_KEYS) return;
    held_key *h = &e->held[e->nheld++];
    h->kind = kind;
    h->code = code;
    snprintf(h->name, sizeof(h->name), "%s", name);
}

static void held_remove(emu *e, key_kind kind, unsigned code) {
    int idx = held_index(e, kind, code);
    if (idx < 0) return;
    e->held[idx] = e->held[--e->nheld];
}

int emu_keys_event(emu *e, const key_action *acts, int n, bool down, char *err, size_t errlen) {
    if (!e) return fail(err, errlen, "machine is not running");
    if (down) {
        for (int i = 0; i < n; i++) {
            const key_action *a = &acts[i];
            if (a->kind == KEY_KIND_RETRO) {
                if (a->code >= KEY_STATE_SLOTS) continue;
                e->retro_keys[a->code] = true;
                held_add(e, a->kind, a->code, a->name);
            } else {
                if (held_index(e, a->kind, a->code) < 0) {
                    send_keycode((uint8_t)a->code, PX68K_KEY_DOWN);
                    held_add(e, a->kind, a->code, a->name);
                }
            }
        }
    } else {
        for (int i = n - 1; i >= 0; i--) {
            const key_action *a = &acts[i];
            if (a->kind == KEY_KIND_RETRO) {
                if (a->code < KEY_STATE_SLOTS) e->retro_keys[a->code] = false;
                held_remove(e, a->kind, a->code);
            } else {
                if (held_index(e, a->kind, a->code) >= 0) {
                    send_keycode((uint8_t)a->code, PX68K_KEY_UP);
                    held_remove(e, a->kind, a->code);
                }
            }
        }
    }
    return 0;
}

int emu_joy_event(emu *e, int port, const char *button, bool down, char *err, size_t errlen) {
    if (!e) return fail(err, errlen, "machine is not running");
    if (port < 0 || port > 1) return fail(err, errlen, "joystick port must be 1 or 2");
    unsigned id;
    char lower[32];
    size_t i;
    for (i = 0; button[i] && i < sizeof(lower) - 1; i++)
        lower[i] = (char)tolower((unsigned char)button[i]);
    lower[i] = '\0';

    if (!strcmp(lower, "up")) id = RETRO_DEVICE_ID_JOYPAD_UP;
    else if (!strcmp(lower, "down")) id = RETRO_DEVICE_ID_JOYPAD_DOWN;
    else if (!strcmp(lower, "left")) id = RETRO_DEVICE_ID_JOYPAD_LEFT;
    else if (!strcmp(lower, "right")) id = RETRO_DEVICE_ID_JOYPAD_RIGHT;
    else if (!strcmp(lower, "a") || !strcmp(lower, "trg1") || !strcmp(lower, "b1")) id = RETRO_DEVICE_ID_JOYPAD_A;
    else if (!strcmp(lower, "b") || !strcmp(lower, "trg2") || !strcmp(lower, "b2")) id = RETRO_DEVICE_ID_JOYPAD_B;
    else if (!strcmp(lower, "x")) id = RETRO_DEVICE_ID_JOYPAD_X;
    else if (!strcmp(lower, "y")) id = RETRO_DEVICE_ID_JOYPAD_Y;
    else if (!strcmp(lower, "l")) id = RETRO_DEVICE_ID_JOYPAD_L;
    else if (!strcmp(lower, "r")) id = RETRO_DEVICE_ID_JOYPAD_R;
    else if (!strcmp(lower, "start") || !strcmp(lower, "run")) id = RETRO_DEVICE_ID_JOYPAD_START;
    else if (!strcmp(lower, "select") || !strcmp(lower, "mode")) id = RETRO_DEVICE_ID_JOYPAD_SELECT;
    else return fail(err, errlen,
                     "unknown joystick button '%s' (use up/down/left/right/a/b/x/y/l/r/start/select)",
                     button);

    if (down) e->joy_mask[port] |= (1u << id);
    else e->joy_mask[port] &= ~(1u << id);
    return 0;
}

int emu_mouse_move(emu *e, int dx, int dy, char *err, size_t errlen) {
    if (!e) return fail(err, errlen, "machine is not running");
    long long nx = (long long)e->mouse_dx + dx;
    long long ny = (long long)e->mouse_dy + dy;
    if (nx > 100000) nx = 100000;
    if (nx < -100000) nx = -100000;
    if (ny > 100000) ny = 100000;
    if (ny < -100000) ny = -100000;
    e->mouse_dx = (int)nx;
    e->mouse_dy = (int)ny;
    return 0;
}

int emu_mouse_button(emu *e, const char *button, bool down, char *err, size_t errlen) {
    if (!e) return fail(err, errlen, "machine is not running");
    unsigned bit;
    if (!strcasecmp(button, "left") || !strcasecmp(button, "l")) bit = 1;
    else if (!strcasecmp(button, "right") || !strcasecmp(button, "r")) bit = 2;
    else return fail(err, errlen, "mouse button must be 'left' or 'right'");
    if (down) e->mouse_buttons |= bit;
    else e->mouse_buttons &= ~bit;
    return 0;
}

int emu_held_keys(const emu *e, const char **names, int max) {
    int n = 0;
    for (int i = 0; i < e->nheld && n < max; i++) names[n++] = e->held[i].name;
    return n;
}

int emu_held_joy(const emu *e, int port, const char **names, int max) {
    static const char *id_names[] = {
        "b", "y", "select", "start", "up", "down", "left", "right",
        "a", "x", "l", "r", "l2", "r2", "l3", "r3"};
    int n = 0;
    if (port < 0 || port > 1) return 0;
    for (unsigned id = 0; id < 16 && n < max; id++) {
        if (e->joy_mask[port] & (1u << id)) names[n++] = id_names[id];
    }
    return n;
}

unsigned emu_mouse_buttons(const emu *e) { return e->mouse_buttons; }

/* ------------------------------------------------------------------ */
/* floppies                                                            */
/* ------------------------------------------------------------------ */

static bool disk_callbacks_available(void) { return g_disk_ext || g_disk_legacy; }

static bool dk_set_eject(bool ejected) {
    if (g_disk_ext) return g_disk_ext->set_eject_state(ejected);
    return g_disk_legacy->set_eject_state(ejected);
}
static bool dk_set_index(unsigned index) {
    if (g_disk_ext) return g_disk_ext->set_image_index(index);
    return g_disk_legacy->set_image_index(index);
}
static bool dk_replace(unsigned index, const struct retro_game_info *info) {
    if (g_disk_ext) return g_disk_ext->replace_image_index(index, info);
    return g_disk_legacy->replace_image_index(index, info);
}

static bool fdd_ext_ok(const char *path) {
    static const char *exts[] = {"dim", "d88", "88d", "hdm", "dup", "2hd", "xdf", "img"};
    char *ext = file_extension(path);
    if (!ext) return false;
    bool ok = false;
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        if (!strcmp(ext, exts[i])) {
            ok = true;
            break;
        }
    }
    free(ext);
    return ok;
}

int emu_insert_fdd(emu *e, int drive, const char *path, char *err, size_t errlen) {
    if (!e) return fail(err, errlen, "machine is not running");
    if (drive < 0 || drive > 1) return fail(err, errlen, "floppy drive must be 0 or 1");
    if (!path || !*path) return fail(err, errlen, "no disk image specified");
    if (!path_exists(path)) return fail(err, errlen, "disk image not found: %s", path);
    if (path_is_dir(path)) return fail(err, errlen, "disk image is a directory: %s", path);
    if (!fdd_ext_ok(path))
        return fail(err, errlen,
                    "unsupported floppy image extension for %s (use dim, d88, hdm, 2hd, xdf, dup or img)",
                    path);
    if (!disk_callbacks_available())
        return fail(err, errlen, "emulator core has no disk control interface");

    e->disk_drive = drive;
    /* Eject whatever is there first: this also flushes write-back changes. */
    dk_set_index((unsigned)drive);
    dk_set_eject(true);

    struct retro_game_info info;
    memset(&info, 0, sizeof(info));
    info.path = path;
    if (!dk_replace((unsigned)drive, &info))
        return fail(err, errlen, "emulator rejected the disk image path: %s", path);
    if (!dk_set_index((unsigned)drive))
        return fail(err, errlen, "emulator rejected disk slot %d", drive);
    if (!dk_set_eject(false))
        return fail(err, errlen, "emulator failed to insert the disk into drive %d", drive);

    e->fdd_inserted[drive] = true;
    snprintf(e->fdd_path[drive], sizeof(e->fdd_path[drive]), "%s", path);
    xlog("FDD%d: inserted %s", drive, path);
    return 0;
}

int emu_eject_fdd(emu *e, int drive, char *err, size_t errlen) {
    if (!e) return fail(err, errlen, "machine is not running");
    if (drive < 0 || drive > 1) return fail(err, errlen, "floppy drive must be 0 or 1");
    if (!disk_callbacks_available())
        return fail(err, errlen, "emulator core has no disk control interface");
    e->disk_drive = drive;
    dk_set_index((unsigned)drive);
    if (!dk_set_eject(true))
        return fail(err, errlen, "emulator failed to eject drive %d", drive);
    e->fdd_inserted[drive] = false;
    e->fdd_path[drive][0] = '\0';
    xlog("FDD%d: ejected", drive);
    return 0;
}

const char *emu_fdd_path(const emu *e, int drive) {
    if (!e || drive < 0 || drive > 1 || !e->fdd_inserted[drive]) return NULL;
    return e->fdd_path[drive];
}

bool emu_fdd_inserted(const emu *e, int drive) {
    return e && drive >= 0 && drive <= 1 && e->fdd_inserted[drive];
}

const char *emu_hdd_path(const emu *e) {
    return (e && e->hdd_path[0]) ? e->hdd_path : NULL;
}

/* ------------------------------------------------------------------ */
/* save states                                                         */
/* ------------------------------------------------------------------ */

#define STATE_MAGIC "X68KST02"
typedef struct {
    char magic[8];
    uint64_t frame;
    uint64_t core_size;   /* uncompressed */
    uint64_t comp_size;   /* bytes in the file after this header */
    double seconds;
    double fps;
} state_header;

static void release_all_inputs(emu *e) {
    for (int i = 0; i < e->nheld; i++) {
        if (e->held[i].kind == KEY_KIND_RAW)
            send_keycode((uint8_t)e->held[i].code, PX68K_KEY_UP);
    }
    memset(e->retro_keys, 0, sizeof(e->retro_keys));
    e->joy_mask[0] = e->joy_mask[1] = 0;
    e->mouse_buttons = 0;
    e->nheld = 0;
}

int emu_save_state(emu *e, const char *path, size_t *bytes, char *err, size_t errlen) {
    if (!e || !e->booted) return fail(err, errlen, "machine is not running");
    size_t sz = retro_serialize_size();
    if (!sz) return fail(err, errlen, "the emulator core did not provide a state size");
    uint8_t *core = xmalloc(sz);
    if (!retro_serialize(core, sz)) {
        free(core);
        return fail(err, errlen, "the emulator core failed to serialize the machine");
    }
    uLongf clen = compressBound((uLong)sz);
    uint8_t *comp = xmalloc(clen);
    if (compress2(comp, &clen, core, (uLong)sz, 3) != Z_OK) {
        free(core);
        free(comp);
        return fail(err, errlen, "failed to compress the state");
    }
    free(core);

    state_header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, STATE_MAGIC, 8);
    h.frame = e->frame;
    h.core_size = sz;
    h.comp_size = clen;
    h.seconds = e->seconds;
    h.fps = e->fps;

    char tmp[X68K_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(comp);
        return fail(err, errlen, "cannot create %s: %s", tmp, strerror(errno));
    }
    size_t n1 = fwrite(&h, 1, sizeof(h), f);
    size_t n2 = fwrite(comp, 1, clen, f);
    int close_rc = fclose(f);
    free(comp);
    if (n1 != sizeof(h) || n2 != clen || close_rc != 0) {
        unlink(tmp);
        return fail(err, errlen, "failed to write state file %s", tmp);
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return fail(err, errlen, "failed to install state file %s: %s", path, strerror(errno));
    }
    if (bytes) *bytes = sizeof(h) + clen;
    return 0;
}

int emu_load_state(emu *e, const char *path, char *err, size_t errlen) {
    if (!e || !e->booted) return fail(err, errlen, "machine is not running");
    FILE *f = fopen(path, "rb");
    if (!f) return fail(err, errlen, "state file not found: %s", path);
    state_header h;
    if (fread(&h, 1, sizeof(h), f) != sizeof(h)) {
        fclose(f);
        return fail(err, errlen, "state file is truncated: %s", path);
    }
    if (memcmp(h.magic, STATE_MAGIC, 8) != 0) {
        fclose(f);
        return fail(err, errlen, "not an x68k state file (or an outdated version): %s", path);
    }
    if (h.core_size == 0 || h.core_size > (64u << 20) || h.comp_size > (64u << 20)) {
        fclose(f);
        return fail(err, errlen, "state file has an invalid size");
    }
    uint8_t *comp = xmalloc((size_t)h.comp_size);
    size_t got = fread(comp, 1, (size_t)h.comp_size, f);
    fclose(f);
    if (got != (size_t)h.comp_size) {
        free(comp);
        return fail(err, errlen, "state file is truncated: %s", path);
    }
    uint8_t *core = xmalloc((size_t)h.core_size);
    uLongf outlen = (uLongf)h.core_size;
    int zrc = uncompress(core, &outlen, comp, (uLong)h.comp_size);
    free(comp);
    if (zrc != Z_OK || outlen != h.core_size) {
        free(core);
        return fail(err, errlen, "state file is corrupt");
    }
    if (!retro_unserialize(core, (size_t)h.core_size)) {
        free(core);
        return fail(err, errlen, "the emulator core rejected the state");
    }
    free(core);
    /* Release whatever is held *after* the state is restored, so keys that
     * were held when the state was saved do not stay stuck down. */
    release_all_inputs(e);
    e->frame = h.frame;
    e->seconds = h.seconds;
    if (h.fps > 1.0) e->fps = h.fps;
    e->pixels = NULL;
    e->width = e->height = 0;
    e->pitch = 0;
    e->hash_valid = false;
    e->video_updated = false;
    e->mouse_dx = e->mouse_dy = 0;
    return 0;
}


/* ------------------------------------------------------------------ */
/* reset / info                                                        */
/* ------------------------------------------------------------------ */

void emu_reset(emu *e) {
    if (!e || !e->booted) return;
    retro_reset();
    e->frame = 0;
    e->seconds = 0.0;
    e->hash_valid = false;
    e->mouse_dx = e->mouse_dy = 0;
    e->mouse_buttons = 0;
    memset(e->retro_keys, 0, sizeof(e->retro_keys));
    e->joy_mask[0] = e->joy_mask[1] = 0;
    e->nheld = 0;
    xlog("machine reset");
}

double emu_fps(const emu *e) { return e ? round(e->fps * 1000.0) / 1000.0 : 0.0; }
unsigned emu_ram_mb(const emu *e) { return e ? (unsigned)e->cfg.ram_mb : 0; }
unsigned emu_cpu_mhz(const emu *e) { return e ? (unsigned)e->cfg.cpu_mhz : 0; }
bool emu_compat_fps(const emu *e) { return e ? e->cfg.compat_fps : false; }
uint64_t emu_frame(const emu *e) { return e ? e->frame : 0; }
double emu_seconds(const emu *e) { return e ? e->seconds : 0.0; }
const char *emu_core_version(void) {
    static char buf[160];
    if (g_emu) return g_emu->core_version;
    struct retro_system_info si;
    memset(&si, 0, sizeof(si));
    retro_get_system_info(&si);
    snprintf(buf, sizeof(buf), "%s %s", si.library_name, si.library_version);
    return buf;
}
const char *emu_system_dir(const emu *e) { return e ? e->system_root : ""; }

void emu_av_info(emu *e, unsigned *w, unsigned *h, double *fps, double *sample_rate) {
    if (!e) return;
    if (e->width) {
        if (w) *w = e->width;
        if (h) *h = e->height;
    } else {
        struct retro_system_av_info av;
        retro_get_system_av_info(&av);
        if (w) *w = av.geometry.base_width;
        if (h) *h = av.geometry.base_height;
    }
    if (fps) *fps = e->fps;
    if (sample_rate) *sample_rate = e->sample_rate;
}

void emu_set_recorder(emu *e, recorder *r) { if (e) e->rec = r; }
recorder *emu_get_recorder(const emu *e) { return e ? e->rec : NULL; }
void emu_set_vgm(emu *e, vgm_recorder *v) { if (e) e->vgm = v; }
vgm_recorder *emu_get_vgm(const emu *e) { return e ? e->vgm : NULL; }
