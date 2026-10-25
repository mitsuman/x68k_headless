#include "server.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app.h"
#include "emu.h"
#include "ipc.h"
#include "json.h"
#include "keys.h"
#include "log.h"
#include "recorder.h"
#include "util.h"

typedef struct {
    app_paths paths;
    int listen_fd;
    int lock_fd;
    int64_t start_ms;
    uint64_t request_count;

    bool has_machine;
    emu *machine;
    emu_config cfg;
    bool cfg_valid;

    recorder *rec;
    char rec_path[X68K_PATH_MAX];
    uint64_t rec_start_frame;
    double rec_start_seconds;
    int64_t rec_start_wall_ms;

    uint64_t last_hash;
    bool last_hash_valid;
    int64_t boot_wall_ms;
    int64_t last_step_ms;
    double last_speed;
    int quit_requested;
} server;

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* ------------------------------------------------------------------ */
/* response helpers                                                    */
/* ------------------------------------------------------------------ */

static json *new_response(const char *cmd) {
    json *r = json_object();
    json_set_bool(r, "ok", false);
    json_set_str(r, "command", cmd);
    return r;
}

static json *resp_data(json *r) {
    json *d = json_get(r, "data");
    if (d && json_is_object(d)) return d;
    d = json_object();
    json_set(r, "data", d);
    return d;
}

static void set_ok(json *r) { json_set_bool(r, "ok", true); }

static int set_error(json *r, const char *code, const char *msg, const char *hint, int exit_code) {
    json *e = json_object();
    json_set_str(e, "code", code);
    json_set_str(e, "message", msg ? msg : "error");
    if (hint && *hint) json_set_str(e, "hint", hint);
    json_set_int(e, "exit", exit_code);
    json_set(r, "error", e);
    json_set_bool(r, "ok", false);
    return -1;
}

static int usage_error(json *r, const char *msg, const char *hint) {
    return set_error(r, "usage", msg, hint, 2);
}

static int cmd_error(json *r, const char *code, const char *msg, const char *hint) {
    return set_error(r, code, msg, hint, 1);
}

static json *media_obj(server *s) {
    json *m = json_object();
    if (s->has_machine) {
        const char *p0 = emu_fdd_path(s->machine, 0);
        const char *p1 = emu_fdd_path(s->machine, 1);
        const char *hd = emu_hdd_path(s->machine);
        if (p0) json_set_str(m, "fdd0", p0);
        else json_set_null(m, "fdd0");
        if (p1) json_set_str(m, "fdd1", p1);
        else json_set_null(m, "fdd1");
        if (hd) json_set_str(m, "hdd", hd);
        else json_set_null(m, "hdd");
    } else {
        json_set_null(m, "fdd0");
        json_set_null(m, "fdd1");
        json_set_null(m, "hdd");
    }
    return m;
}

static bool video_is_31k(server *s) {
    return s->has_machine && emu_fps(s->machine) < 58.0;
}

static json *recording_obj(server *s) {
    if (!s->rec) return json_null();
    json *r = json_object();
    json_set_str(r, "path", recorder_path(s->rec));
    json_set_double(r, "fps", recorder_fps(s->rec));
    json_set_int(r, "width", recorder_width(s->rec));
    json_set_int(r, "height", recorder_height(s->rec));
    json_set_bool(r, "audio", recorder_has_audio(s->rec));
    json_set_int(r, "frames", (int64_t)recorder_frames(s->rec));
    json_set_double(r, "seconds", recorder_seconds(s->rec));
    json_set_int(r, "started_frame", (int64_t)s->rec_start_frame);
    char fmsg[1024];
    if (recorder_failed(s->rec, fmsg, sizeof(fmsg))) {
        json_set_bool(r, "failed", true);
        json_set_str(r, "error", fmsg);
    }
    return r;
}

/* Returns the current screen hash and updates the comparison baseline once. */
static void add_state(server *s, json *resp) {
    json *state = json_object();
    json_set_bool(state, "booted", s->has_machine);
    if (s->has_machine) {
        emu *e = s->machine;
        json_set_int(state, "frame", (int64_t)emu_frame(e));
        json_set_double(state, "emu_seconds", emu_seconds(e));
        json_set_double(state, "fps", emu_fps(e));

        json *video = json_object();
        json_set_bool(video, "ready", emu_screen_ready(e));
        unsigned w = 0, h = 0;
        emu_screen_info(e, &w, &h, NULL);
        json_set_int(video, "width", w);
        json_set_int(video, "height", h);
        json_set_str(video, "mode", video_is_31k(s) ? "31k" : "15k");
        json_set_str(video, "aspect", "4:3");
        json_set(state, "video", video);

        json *screen = json_object();
        if (emu_screen_ready(e)) {
            uint64_t hsh = emu_screen_hash(e);
            bool changed = s->last_hash_valid && hsh != s->last_hash;
            char hex[32];
            snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)hsh);
            json_set_str(screen, "hash", hex);
            json_set_bool(screen, "changed", changed);
            if (!s->last_hash_valid) s->last_hash_valid = true;
            s->last_hash = hsh;
        } else {
            json_set_null(screen, "hash");
            json_set_bool(screen, "changed", false);
        }
        json_set(state, "screen", screen);

        json *input = json_object();
        const char *names[24];
        int n = emu_held_keys(e, names, 24);
        json *keys = json_array();
        for (int i = 0; i < n; i++) json_add_str(keys, names[i]);
        json_set(input, "keys", keys);
        json *joy = json_object();
        for (int port = 0; port < 2; port++) {
            const char *jn[16];
            int jc = emu_held_joy(e, port, jn, 16);
            json *arr = json_array();
            for (int i = 0; i < jc; i++) json_add_str(arr, jn[i]);
            char key[8];
            snprintf(key, sizeof(key), "p%d", port + 1);
            json_set(joy, key, arr);
        }
        json_set(input, "joy", joy);
        json *mouse = json_object();
        json *mbtn = json_array();
        unsigned mb = emu_mouse_buttons(e);
        if (mb & 1) json_add_str(mbtn, "left");
        if (mb & 2) json_add_str(mbtn, "right");
        json_set(mouse, "buttons", mbtn);
        json_set(input, "mouse", mouse);
        json_set(state, "input", input);

        json_set(state, "media", media_obj(s));
        json_set(state, "recording", recording_obj(s));
        json_set_int(state, "perf_last_step_ms", s->last_step_ms);
        json_set_double(state, "perf_speed_x", s->last_speed);

        json *machine = json_object();
        json_set_int(machine, "ram_mb", emu_ram_mb(e));
        json_set_int(machine, "cpu_mhz", emu_cpu_mhz(e));
        json_set_bool(machine, "compat_fps", emu_compat_fps(e));
        json_set_str(machine, "core", emu_core_version());
        json_set(state, "machine", machine);
    }
    json_set(resp, "state", state);
}

/* ------------------------------------------------------------------ */
/* recording helpers                                                   */
/* ------------------------------------------------------------------ */

static void stop_machine(server *s) {
    if (s->rec) {
        char err[2048] = {0};
        if (s->machine) emu_set_recorder(s->machine, NULL);
        if (recorder_stop(s->rec, err, sizeof(err)) != 0) {
            xlog("recording could not be finalised: %s", err);
        }
        s->rec = NULL;
    }
    if (s->machine) {
        emu_destroy(s->machine);
        s->machine = NULL;
    }
    s->has_machine = false;
    s->last_hash_valid = false;
    s->last_step_ms = 0;
    s->last_speed = 0;
}

/* Called after anything that advances time.  Detects asynchronous encoder
 * death and reports it without breaking the emulator. */
static void check_recorder(server *s, json *data) {
    if (!s->rec) return;
    char msg[2048];
    if (!recorder_failed(s->rec, msg, sizeof(msg))) return;
    char path[X68K_PATH_MAX];
    snprintf(path, sizeof(path), "%s", recorder_path(s->rec));
    s->rec_path[0] = '\0';
    if (s->machine) emu_set_recorder(s->machine, NULL);
    char err[2048] = {0};
    recorder_stop(s->rec, err, sizeof(err));
    s->rec = NULL;
    json *warn = json_object();
    json_set_str(warn, "code", "recording_failed");
    json_set_str(warn, "message", msg);
    json_set_str(warn, "path", path);
    json_set(data, "warning", warn);
}

static int finalize_recording(server *s, json *data) {
    if (!s->rec) return -1;
    char path[X68K_PATH_MAX];
    snprintf(path, sizeof(path), "%s", recorder_path(s->rec));
    uint64_t frames = recorder_frames(s->rec);
    double out_seconds = recorder_seconds(s->rec);
    double fps = recorder_fps(s->rec);
    unsigned w = recorder_width(s->rec);
    unsigned h = recorder_height(s->rec);
    bool audio = recorder_has_audio(s->rec);
    /* The recorder timeline is emulated time by construction (audio is
     * written at its native 44.1 kHz and video frames are emitted to match),
     * so video_seconds is also the recorded amount of emulated time.  Using
     * it keeps the report correct across a mid-recording reset. */
    double emu_secs = out_seconds;
    int64_t wall_ms = now_ms_monotonic() - s->rec_start_wall_ms;

    if (s->machine) emu_set_recorder(s->machine, NULL);
    char err[2048] = {0};
    int rc = recorder_stop(s->rec, err, sizeof(err));
    s->rec = NULL;
    s->rec_path[0] = '\0';

    json *info = json_object();
    json_set_str(info, "path", path);
    json_set_int(info, "frames", (int64_t)frames);
    json_set_double(info, "fps", fps);
    json_set_int(info, "width", w);
    json_set_int(info, "height", h);
    json_set_bool(info, "audio", audio);
    json_set_double(info, "video_seconds", out_seconds);
    json_set_double(info, "emu_seconds", emu_secs);
    json_set_int(info, "wall_ms", wall_ms);
    struct stat st;
    if (stat(path, &st) == 0) json_set_int(info, "bytes", (int64_t)st.st_size);
    else json_set_int(info, "bytes", 0);

    if (rc != 0) {
        json *e = json_object();
        json_set_str(e, "code", "recording_failed");
        json_set_str(e, "message", err);
        json_set_str(e, "path", path);
        json_set_int(e, "frames", (int64_t)frames);
        json_set_int(e, "exit", 1);
        json_set(data, "recording", info);
        json_set(data, "error_detail", e);
        return -1;
    }
    json_set(data, "recording", info);
    return 0;
}

/* ------------------------------------------------------------------ */
/* option helpers                                                      */
/* ------------------------------------------------------------------ */

/* returns 1 matched, 0 not matched, -1 missing value */
static int match_opt(int argc, char **argv, int *i, const char *name, const char **val) {
    const char *a = argv[*i];
    size_t n = strlen(name);
    if (!strcmp(a, name)) {
        if (*i + 1 >= argc) return -1;
        *val = argv[++(*i)];
        return 1;
    }
    if (!strncmp(a, name, n) && a[n] == '=') {
        *val = a + n + 1;
        return 1;
    }
    return 0;
}

static int parse_fps_mode(const char *s, bool *compat) {
    if (!strcasecmp(s, "actual")) { *compat = false; return 0; }
    if (!strcasecmp(s, "compat")) { *compat = true; return 0; }
    return -1;
}

static int parse_joy_type(const char *s, int *out) {
    if (!strcasecmp(s, "2button") || !strcasecmp(s, "2") || !strcasecmp(s, "default"))
        *out = JOY_2BUTTON;
    else if (!strcasecmp(s, "cpsf-md") || !strcasecmp(s, "md") || !strcasecmp(s, "8button"))
        *out = JOY_CPSF_MD;
    else if (!strcasecmp(s, "cpsf-sfc") || !strcasecmp(s, "sfc"))
        *out = JOY_CPSF_SFC;
    else if (!strcasecmp(s, "cyberstick-d") || !strcasecmp(s, "cyber-d"))
        *out = JOY_CYBERSTICK_D;
    else if (!strcasecmp(s, "cyberstick-a") || !strcasecmp(s, "cyber-a"))
        *out = JOY_CYBERSTICK_A;
    else
        return -1;
    return 0;
}

static bool fdd_ext_ok(const char *path) {
    static const char *exts[] = {"dim", "d88", "88d", "hdm", "dup", "2hd", "xdf", "img"};
    char *ext = file_extension(path);
    if (!ext) return false;
    bool ok = false;
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        if (!strcmp(ext, exts[i])) { ok = true; break; }
    }
    free(ext);
    return ok;
}

static char *find_rom_source(server *s) {
    if (s->cfg_valid && s->cfg.rom_source[0]) {
        char *p = find_file_ci(s->cfg.rom_source, "iplrom.dat");
        if (p) { free(p); return xstrdup(s->cfg.rom_source); }
    }
    char *env = env_dup("X68K_ROM_DIR");
    if (env) {
        char *p = find_file_ci(env, "iplrom.dat");
        if (p) { free(p); return env; }
        free(env);
    }
    char *home = env_home_dir();
    char *cands[5];
    cands[0] = path_join(s->paths.home, "roms/keropi");
    cands[1] = path_join(s->paths.home, "roms");
    cands[2] = path_join(home, "Documents/MPX68K");
    cands[3] = path_join(home, "x68k");
    cands[4] = path_join(home, "x68k/roms");
    char *found = NULL;
    for (int i = 0; i < 5; i++) {
        if (!found) {
            char *ip = find_file_ci(cands[i], "iplrom.dat");
            if (ip) {
                char *cg = find_file_ci(cands[i], "cgrom.dat");
                if (cg) {
                    free(cg);
                    found = xstrdup(cands[i]);
                }
                free(ip);
            }
        }
        free(cands[i]);
    }
    free(home);
    return found;
}

static int ensure_parent_dir(const char *path, char *err, size_t errlen) {
    char *dir = path_dirname(path);
    int rc = 0;
    if (mkdir_p(dir, 0755) != 0) {
        snprintf(err, errlen, "cannot create directory %s: %s", dir, strerror(errno));
        rc = -1;
    }
    free(dir);
    return rc;
}

static char *default_snap_path(server *s, int scale) {
    char name[64];
    uint64_t frame = s->has_machine ? emu_frame(s->machine) : 0;
    if (scale > 1)
        snprintf(name, sizeof(name), "f%08llu@%dx.png", (unsigned long long)frame, scale);
    else
        snprintf(name, sizeof(name), "f%08llu.png", (unsigned long long)frame);
    return path_join(s->paths.screens, name);
}

static char *default_record_path(server *s) {
    char ts[32];
    time_stamp(ts, sizeof(ts));
    char name[128];
    snprintf(name, sizeof(name), "rec-%s.mp4", ts);
    char *p = path_join(s->paths.recordings, name);
    int n = 1;
    while (path_exists(p)) {
        free(p);
        snprintf(name, sizeof(name), "rec-%s-%d.mp4", ts, n++);
        p = path_join(s->paths.recordings, name);
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static json *cmd_boot(server *s, int argc, char **argv) {
    json *resp = new_response("boot");
    const char *roms = NULL, *hdd = NULL, *fdd0 = NULL, *fdd1 = NULL;
    const char *pos[4];
    int npos = 0;
    int ram = 4, cpu = 10, jt0 = JOY_2BUTTON, jt1 = JOY_2BUTTON;
    bool compat = false;

    for (int i = 1; i < argc; i++) {
        const char *v = NULL;
        int m;
        if ((m = match_opt(argc, argv, &i, "--roms", &v)) != 0) {
            if (m < 0) { usage_error(resp, "--roms requires a directory", NULL); return resp; }
            roms = v;
            continue;
        }
        if ((m = match_opt(argc, argv, &i, "--hdd", &v)) != 0) {
            if (m < 0) { usage_error(resp, "--hdd requires a path", NULL); return resp; }
            hdd = v;
            continue;
        }
        if ((m = match_opt(argc, argv, &i, "--fdd0", &v)) != 0) {
            if (m < 0) { usage_error(resp, "--fdd0 requires a path", NULL); return resp; }
            fdd0 = v;
            continue;
        }
        if ((m = match_opt(argc, argv, &i, "--fdd1", &v)) != 0) {
            if (m < 0) { usage_error(resp, "--fdd1 requires a path", NULL); return resp; }
            fdd1 = v;
            continue;
        }
        if ((m = match_opt(argc, argv, &i, "--ram", &v)) != 0) {
            if (m < 0) { usage_error(resp, "--ram requires MB", NULL); return resp; }
            long long x;
            if (!parse_int(v, &x) || x < 1 || x > 12)
                { usage_error(resp, "--ram must be 1..12 (MB)", NULL); return resp; }
            ram = (int)x;
            continue;
        }
        if ((m = match_opt(argc, argv, &i, "--cpu", &v)) != 0) {
            if (m < 0) { usage_error(resp, "--cpu requires MHz", NULL); return resp; }
            long long x;
            if (!parse_int(v, &x) || (x != 10 && x != 16 && x != 25 && x != 33 && x != 66 && x != 100))
                { usage_error(resp, "--cpu must be 10, 16, 25, 33, 66 or 100", NULL); return resp; }
            cpu = (int)x;
            continue;
        }
        if ((m = match_opt(argc, argv, &i, "--fps", &v)) != 0) {
            if (m < 0) { usage_error(resp, "--fps requires actual|compat", NULL); return resp; }
            if (parse_fps_mode(v, &compat) != 0)
                { usage_error(resp, "--fps must be 'actual' or 'compat'", NULL); return resp; }
            continue;
        }
        if ((m = match_opt(argc, argv, &i, "--joy1", &v)) != 0) {
            if (m < 0) { usage_error(resp, "--joy1 requires a type", NULL); return resp; }
            if (parse_joy_type(v, &jt0) != 0)
                { usage_error(resp, "unknown --joy1 type", "use 2button, cpsf-md, cpsf-sfc, cyberstick-d or cyberstick-a"); return resp; }
            continue;
        }
        if ((m = match_opt(argc, argv, &i, "--joy2", &v)) != 0) {
            if (m < 0) { usage_error(resp, "--joy2 requires a type", NULL); return resp; }
            if (parse_joy_type(v, &jt1) != 0)
                { usage_error(resp, "unknown --joy2 type", "use 2button, cpsf-md, cpsf-sfc, cyberstick-d or cyberstick-a"); return resp; }
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] == '-')
            { usage_error(resp, "unknown option for boot", argv[i]); return resp; }
        if (npos >= 4)
            { usage_error(resp, "too many disk images", "boot takes at most two floppy images"); return resp; }
        pos[npos++] = argv[i];
    }

    if (npos > 0 && fdd0)
        { usage_error(resp, "both a positional image and --fdd0 were given", NULL); return resp; }
    if (npos > 1 && fdd1)
        { usage_error(resp, "both a second positional image and --fdd1 were given", NULL); return resp; }
    if (npos > 2)
        { usage_error(resp, "too many floppy images", NULL); return resp; }
    if (npos >= 1) fdd0 = pos[0];
    if (npos >= 2) fdd1 = pos[1];

    const char *images[2] = {fdd0, fdd1};
    for (int i = 0; i < 2; i++) {
        if (!images[i]) continue;
        if (!path_exists(images[i])) {
            char msg[1024];
            snprintf(msg, sizeof(msg), "floppy image not found: %s", images[i]);
            { cmd_error(resp, "media_missing", msg, NULL); return resp; }
        }
        if (!fdd_ext_ok(images[i])) {
            char msg[1024];
            snprintf(msg, sizeof(msg), "unsupported floppy image: %s", images[i]);
            { cmd_error(resp, "bad_media", msg, "use dim, d88, hdm, 2hd, xdf, dup or img"); return resp; }
        }
    }
    if (hdd) {
        if (!path_exists(hdd)) {
            char msg[1024];
            snprintf(msg, sizeof(msg), "HDD image not found: %s", hdd);
            { cmd_error(resp, "media_missing", msg, NULL); return resp; }
        }
        if (!path_has_ext(hdd, "hdf")) {
            { cmd_error(resp, "bad_media", "the core only boots hard disks from .hdf images",
                                   "convert or rename the image to .hdf"); return resp; }
        }
    }

    char *rom_source = roms ? path_absolute(roms) : find_rom_source(s);
    if (!rom_source) {
        { cmd_error(resp, "roms_missing",
                               "no X68000 ROM set found (iplrom.dat + cgrom.dat)",
                               "pass --roms DIR, set X68K_ROM_DIR, or put the ROMs in ~/.x68k/roms"); return resp; }
    }

    char err[2048] = {0};
    if (emu_prepare_roms(rom_source, s->paths.home, err, sizeof(err)) != 0) {
        free(rom_source);
        { cmd_error(resp, "roms_error", err, NULL); return resp; }
    }

    stop_machine(s);

    memset(&s->cfg, 0, sizeof(s->cfg));
    snprintf(s->cfg.home, sizeof(s->cfg.home), "%s", s->paths.home);
    snprintf(s->cfg.rom_source, sizeof(s->cfg.rom_source), "%s", rom_source);
    if (hdd) snprintf(s->cfg.hdd, sizeof(s->cfg.hdd), "%s", hdd);
    if (fdd0) snprintf(s->cfg.fdd[0], sizeof(s->cfg.fdd[0]), "%s", fdd0);
    if (fdd1) snprintf(s->cfg.fdd[1], sizeof(s->cfg.fdd[1]), "%s", fdd1);
    s->cfg.ram_mb = ram;
    s->cfg.cpu_mhz = cpu;
    s->cfg.compat_fps = compat;
    s->cfg.joy_type[0] = jt0;
    s->cfg.joy_type[1] = jt1;
    s->cfg_valid = true;

    s->machine = emu_create(&s->cfg, err, sizeof(err));
    if (!s->machine) {
        free(rom_source);
        { cmd_error(resp, "boot_failed", err, "check the ROM files and disk images"); return resp; }
    }
    s->has_machine = true;
    s->boot_wall_ms = now_ms_monotonic();
    s->last_hash_valid = false;
    s->last_step_ms = 0;
    s->last_speed = 0;

    char keropi[X68K_PATH_MAX];
    snprintf(keropi, sizeof(keropi), "%s/keropi", emu_system_dir(s->machine));

    json *data = resp_data(resp);
    json *m = json_object();
    json_set_int(m, "ram_mb", emu_ram_mb(s->machine));
    json_set_int(m, "cpu_mhz", emu_cpu_mhz(s->machine));
    json_set_double(m, "fps", emu_fps(s->machine));
    json_set_str(m, "fps_mode", emu_compat_fps(s->machine) ? "compat" : "actual");
    json_set_str(m, "core", emu_core_version());
    json_set(data, "machine", m);
    json_set(data, "media", media_obj(s));
    json_set_str(data, "rom_source", rom_source);
    json_set_str(data, "system_dir", keropi);
    json_set_str(data, "hint", "run 'x68k step 10s --snap' to reach the game screen");

    free(rom_source);
    set_ok(resp);
    return resp;
}

static json *cmd_step(server *s, int argc, char **argv, const char *cmdname) {
    json *resp = new_response(cmdname);
    const char *dur = NULL, *out = NULL;
    long long frames_opt = -1;
    int scale = 1;
    bool snap = false;

    for (int i = 1; i < argc; i++) {
        const char *v = NULL;
        int m;
        if (!strcmp(argv[i], "--snap") || !strcmp(argv[i], "-s")) { snap = true; continue; }
        if ((m = match_opt(argc, argv, &i, "--out", &v)) != 0) {
            if (m < 0) return usage_error(resp, "--out requires a path", NULL), resp;
            out = v; snap = true; continue;
        }
        if ((m = match_opt(argc, argv, &i, "--scale", &v)) != 0) {
            if (m < 0) return usage_error(resp, "--scale requires a number", NULL), resp;
            long long x;
            if (!parse_int(v, &x) || x < 1 || x > 8)
                return usage_error(resp, "--scale must be 1..8", NULL), resp;
            scale = (int)x; continue;
        }
        if ((m = match_opt(argc, argv, &i, "--frames", &v)) != 0) {
            if (m < 0) return usage_error(resp, "--frames requires a count", NULL), resp;
            if (!parse_int(v, &frames_opt) || frames_opt < 1)
                return usage_error(resp, "--frames must be a positive integer", NULL), resp;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1]) {
            return usage_error(resp, "unknown option for step", argv[i]), resp;
        }
        if (dur) return usage_error(resp, "step takes exactly one duration", NULL), resp;
        dur = argv[i];
    }

    if (!dur && frames_opt < 0)
        return usage_error(resp, "step needs a duration (e.g. 'x68k step 1.5s') or --frames N", NULL), resp;
    if (dur && frames_opt >= 0)
        return usage_error(resp, "use either a duration or --frames, not both", NULL), resp;

    double seconds = 0;
    bool is_frames = false;
    long long frames = 0;
    if (dur) {
        if (!parse_duration(dur, &seconds, &is_frames, &frames))
            return usage_error(resp, "invalid duration", "examples: 1.5s, 500ms, 90f (maximum 1 hour)"), resp;
    }
    if (frames_opt >= 0) { is_frames = true; frames = frames_opt; }
    if (is_frames && frames > 300000)
        return usage_error(resp, "step is limited to 300000 frames per command",
                           "use several smaller steps"), resp;
    if (!is_frames && seconds > 3600.0)
        return usage_error(resp, "step is limited to 3600 seconds per command",
                           "use several smaller steps"), resp;

    if (!s->has_machine)
        return cmd_error(resp, "not_booted", "no machine is running", "run 'x68k boot IMAGE ...' first"), resp;

    if (s->rec) check_recorder(s, resp_data(resp));

    emu_step_result st;
    memset(&st, 0, sizeof(st));
    int rc;
    if (is_frames) rc = emu_step_frames(s->machine, frames, &st, &g_stop);
    else rc = emu_step_seconds(s->machine, seconds, &st, &g_stop);
    if (rc != 0) return cmd_error(resp, "step_failed", "emulator could not advance", NULL), resp;

    s->last_step_ms = st.wall_ms;
    s->last_speed = st.wall_ms > 0 ? st.emu_seconds / ((double)st.wall_ms / 1000.0) : 0.0;

    json *data = resp_data(resp);
    if (s->rec) check_recorder(s, data);
    json_set_int(data, "frames", (int64_t)st.frames);
    if (is_frames) json_set_null(data, "requested_seconds");
    else json_set_double(data, "requested_seconds", seconds);
    json_set_double(data, "emu_seconds", st.emu_seconds);
    json_set_int(data, "wall_ms", st.wall_ms);
    json_set_double(data, "speed_x", s->last_speed);
    if (g_stop) json_set_bool(data, "interrupted", true);

    if (snap) {
        char *path = out ? path_absolute(out) : default_snap_path(s, scale);
        if (!emu_screen_ready(s->machine)) {
            json_set_null(data, "snapshot");
            json_set_str(data, "snapshot_note", "no frame has been rendered yet; step a little longer");
        } else {
            unsigned w = 0, h = 0;
            size_t bytes = 0;
            char err[1024] = {0};
            if (ensure_parent_dir(path, err, sizeof(err)) != 0) {
                free(path);
                return cmd_error(resp, "snapshot_failed", err, NULL), resp;
            }
            if (emu_snapshot(s->machine, path, scale, &w, &h, &bytes, err, sizeof(err)) != 0) {
                free(path);
                return cmd_error(resp, "snapshot_failed", err, NULL), resp;
            }
            json *snapj = json_object();
            json_set_str(snapj, "path", path);
            json_set_int(snapj, "width", w);
            json_set_int(snapj, "height", h);
            json_set_int(snapj, "scale", scale);
            json_set_int(snapj, "bytes", (int64_t)bytes);
            json_set(data, "snapshot", snapj);
        }
        free(path);
    }
    set_ok(resp);
    return resp;
}

static json *cmd_snap(server *s, int argc, char **argv, const char *cmdname) {
    json *resp = new_response(cmdname);
    const char *out = NULL;
    int scale = 1;
    for (int i = 1; i < argc; i++) {
        const char *v = NULL;
        int m;
        if (!strcmp(argv[i], "-o")) {
            if (i + 1 >= argc) return usage_error(resp, "-o requires a path", NULL), resp;
            out = argv[++i];
            continue;
        }
        if ((m = match_opt(argc, argv, &i, "--out", &v)) != 0) {
            if (m < 0) return usage_error(resp, "-o/--out requires a path", NULL), resp;
            out = v; continue;
        }
        if ((m = match_opt(argc, argv, &i, "--scale", &v)) != 0) {
            if (m < 0) return usage_error(resp, "--scale requires a number", NULL), resp;
            long long x;
            if (!parse_int(v, &x) || x < 1 || x > 8)
                return usage_error(resp, "--scale must be 1..8", NULL), resp;
            scale = (int)x; continue;
        }
        if (argv[i][0] == '-') return usage_error(resp, "unknown option for snap", argv[i]), resp;
        return usage_error(resp, "snap takes no positional arguments", NULL), resp;
    }
    if (!s->has_machine)
        return cmd_error(resp, "not_booted", "no machine is running", "run 'x68k boot IMAGE ...' first"), resp;
    if (!emu_screen_ready(s->machine))
        return cmd_error(resp, "no_frame", "no frame has been rendered yet",
                         "run 'x68k step 0.2s' first"), resp;

    char *path = out ? path_absolute(out) : default_snap_path(s, scale);
    unsigned w = 0, h = 0;
    size_t bytes = 0;
    char err[1024] = {0};
    if (ensure_parent_dir(path, err, sizeof(err)) != 0) {
        free(path);
        return cmd_error(resp, "snapshot_failed", err, NULL), resp;
    }
    if (emu_snapshot(s->machine, path, scale, &w, &h, &bytes, err, sizeof(err)) != 0) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "%s", err);
        free(path);
        return cmd_error(resp, "snapshot_failed", msg, NULL), resp;
    }
    json *data = resp_data(resp);
    json_set_str(data, "path", path);
    json_set_int(data, "width", w);
    json_set_int(data, "height", h);
    json_set_int(data, "scale", scale);
    json_set_int(data, "bytes", (int64_t)bytes);
    json_set_int(data, "frame", (int64_t)emu_frame(s->machine));
    char hex[32];
    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)emu_screen_hash(s->machine));
    json_set_str(data, "hash", hex);
    free(path);
    set_ok(resp);
    return resp;
}

static json *cmd_record(server *s, int argc, char **argv) {
    json *resp = new_response("record");
    if (argc < 2) return usage_error(resp, "record needs a subcommand", "record start | record stop | record status"), resp;
    const char *sub = argv[1];

    if (!strcmp(sub, "start")) {
        const char *out = NULL, *fps_s = NULL, *size_s = NULL;
        bool audio = true;
        for (int i = 2; i < argc; i++) {
            const char *v = NULL;
            int m;
            if (!strcmp(argv[i], "-o")) {
                if (i + 1 >= argc) return usage_error(resp, "-o requires a path", NULL), resp;
                out = argv[++i];
                continue;
            }
            if ((m = match_opt(argc, argv, &i, "--out", &v)) != 0) {
                if (m < 0) return usage_error(resp, "--out requires a path", NULL), resp;
                out = v; continue;
            }
            if ((m = match_opt(argc, argv, &i, "--fps", &v)) != 0) {
                if (m < 0) return usage_error(resp, "--fps requires a number", NULL), resp;
                fps_s = v; continue;
            }
            if ((m = match_opt(argc, argv, &i, "--size", &v)) != 0) {
                if (m < 0) return usage_error(resp, "--size requires WxH", NULL), resp;
                size_s = v; continue;
            }
            if (!strcmp(argv[i], "--no-audio")) { audio = false; continue; }
            return usage_error(resp, "unknown option for record start", argv[i]), resp;
        }
        if (!s->has_machine)
            return cmd_error(resp, "not_booted", "no machine is running", "run 'x68k boot IMAGE ...' first"), resp;
        if (s->rec) check_recorder(s, resp_data(resp));
        if (s->rec)
            return cmd_error(resp, "already_recording", "a recording is already in progress",
                             "run 'x68k record stop' first"), resp;
        double fps = emu_fps(s->machine);
        if (fps_s) {
            errno = 0;
            char *end = NULL;
            fps = strtod(fps_s, &end);
            if (errno || !end || *end || fps < 1.0 || fps > 240.0)
                return usage_error(resp, "--fps must be between 1 and 240", NULL), resp;
        }
        unsigned w = 640, h = 480;
        if (size_s && !parse_size_arg(size_s, &w, &h))
            return usage_error(resp, "--size must be EVENxEVEN, e.g. 640x480", NULL), resp;
        char *path = out ? path_absolute(out) : default_record_path(s);
        char err[2048] = {0};
        recorder *r = recorder_start(path, fps, w, h, 44100, audio, err, sizeof(err));
        if (!r) {
            char msg[2200];
            snprintf(msg, sizeof(msg), "%s", err);
            free(path);
            return cmd_error(resp, "record_start_failed", msg, "check that ffmpeg is installed and the path is writable"), resp;
        }
        s->rec = r;
        snprintf(s->rec_path, sizeof(s->rec_path), "%s", path);
        s->rec_start_frame = emu_frame(s->machine);
        s->rec_start_seconds = emu_seconds(s->machine);
        s->rec_start_wall_ms = now_ms_monotonic();
        emu_set_recorder(s->machine, r);
        json *data = resp_data(resp);
        json_set(data, "recording", recording_obj(s));
        json_set_str(data, "note", "frames are recorded only while 'step' advances emulated time");
        free(path);
        set_ok(resp);
        return resp;
    }

    if (!strcmp(sub, "stop")) {
        if (!s->rec)
            return cmd_error(resp, "no_recording", "no recording is in progress", NULL), resp;
        json *data = resp_data(resp);
        int rc = finalize_recording(s, data);
        if (rc != 0) {
            char msg[2200] = "recording failed";
            json *det = json_get(data, "error_detail");
            const char *m = det ? json_string_value(json_get(det, "message")) : NULL;
            if (m) snprintf(msg, sizeof(msg), "%s", m);
            /* keep the partial-file info in data */
            return cmd_error(resp, "recording_failed", msg, "see daemon.log for ffmpeg output"), resp;
        }
        set_ok(resp);
        return resp;
    }

    if (!strcmp(sub, "status")) {
        json *data = resp_data(resp);
        json_set(data, "recording", recording_obj(s));
        set_ok(resp);
        return resp;
    }

    return usage_error(resp, "unknown record subcommand", "record start | record stop | record status"), resp;
}

static int parse_chords(const char **args, int n, key_action *acts, int max, char *err, size_t errlen) {
    int total = 0;
    for (int i = 0; i < n; i++) {
        key_action tmp[12];
        int k = key_parse_chord(args[i], tmp, 12, err, errlen);
        if (k < 0) return -1;
        for (int j = 0; j < k; j++) {
            if (total >= max) {
                snprintf(err, errlen, "too many keys in one command");
                return -1;
            }
            acts[total++] = tmp[j];
        }
    }
    return total;
}

static void keys_json(json *parent, const key_action *acts, int n) {
    json *arr = json_array();
    for (int i = 0; i < n; i++) json_add_str(arr, acts[i].name);
    json_set(parent, "keys", arr);
}

static int step_hold(server *s, double seconds, bool is_frames, long long frames,
                     emu_step_result *st) {
    if (is_frames) return emu_step_frames(s->machine, frames, st, &g_stop);
    return emu_step_seconds(s->machine, seconds, st, &g_stop);
}

static json *cmd_key(server *s, int argc, char **argv) {
    json *resp = new_response("key");
    if (argc < 2) return usage_error(resp, "key needs a subcommand",
                                     "key tap|down|up|hold|type ..."), resp;
    const char *sub = argv[1];
    const char *keys_spec[32];
    int nkeys = 0;
    const char *hold_s = NULL, *delay_s = NULL;
    int ntype_args = 0;
    char type_buf[8192];
    size_t type_len = 0;
    type_buf[0] = '\0';

    for (int i = 2; i < argc; i++) {
        const char *v = NULL;
        int m;
        if ((m = match_opt(argc, argv, &i, "--hold", &v)) != 0) {
            if (m < 0) return usage_error(resp, "--hold requires a duration", NULL), resp;
            hold_s = v; continue;
        }
        if ((m = match_opt(argc, argv, &i, "--delay", &v)) != 0) {
            if (m < 0) return usage_error(resp, "--delay requires a duration", NULL), resp;
            delay_s = v; continue;
        }
        if (argv[i][0] == '-' && argv[i][1] == '-')
            return usage_error(resp, "unknown option for key", argv[i]), resp;
        if (!strcmp(sub, "type")) {
            size_t l = strlen(argv[i]);
            if (type_len + l + 2 >= sizeof(type_buf))
                return usage_error(resp, "text is too long", NULL), resp;
            if (ntype_args++) type_buf[type_len++] = ' ';
            memcpy(type_buf + type_len, argv[i], l + 1);
            type_len += l;
        } else {
            if (nkeys >= 32) return usage_error(resp, "too many keys", NULL), resp;
            keys_spec[nkeys++] = argv[i];
        }
    }

    if (!s->has_machine)
        return cmd_error(resp, "not_booted", "no machine is running", "run 'x68k boot IMAGE ...' first"), resp;
    if (s->rec) check_recorder(s, resp_data(resp));

    char err[512] = {0};
    key_action acts[64];
    int nacts = 0;

    if (!strcmp(sub, "tap") || !strcmp(sub, "press") || !strcmp(sub, "down") || !strcmp(sub, "up") ||
        !strcmp(sub, "hold")) {
        if (nkeys == 0) return usage_error(resp, "no keys given", "example: x68k key tap return"), resp;

        double hold_sec = 0.1;
        bool hold_frames = false;
        long long hold_nframes = 0;
        bool sub_hold = !strcmp(sub, "hold");
        if (sub_hold) {
            /* duration is one of the positional arguments */
            double dsec = 0;
            bool dfr = false;
            long long dframes = 0;
            int dur_count = 0;
            const char *dur_str = NULL;
            const char *real_keys[32];
            int nreal = 0;
            for (int i = 0; i < nkeys; i++) {
                if (parse_duration(keys_spec[i], &dsec, &dfr, &dframes)) {
                    dur_count++;
                    dur_str = keys_spec[i];
                } else {
                    real_keys[nreal++] = keys_spec[i];
                }
            }
            if (dur_count != 1)
                return usage_error(resp, "key hold needs exactly one duration",
                                   "example: x68k key hold 1s right"), resp;
            if (nreal == 0)
                return usage_error(resp, "key hold needs at least one key",
                                   "example: x68k key hold 1s right"), resp;
            nkeys = nreal;
            for (int i = 0; i < nreal; i++) keys_spec[i] = real_keys[i];
            if (!parse_duration(dur_str, &hold_sec, &hold_frames, &hold_nframes))
                return usage_error(resp, "invalid hold duration", NULL), resp;
        } else if (hold_s) {
            if (!parse_duration(hold_s, &hold_sec, &hold_frames, &hold_nframes))
                return usage_error(resp, "invalid --hold duration", "examples: 100ms, 5f"), resp;
        }

        nacts = parse_chords(keys_spec, nkeys, acts, 64, err, sizeof(err));
        if (nacts < 0) return usage_error(resp, err, "see 'x68k help keys'"), resp;

        if (!strcmp(sub, "down") || !strcmp(sub, "up")) {
            bool down = !strcmp(sub, "down");
            if (emu_keys_event(s->machine, acts, nacts, down, err, sizeof(err)) != 0)
                return cmd_error(resp, "input_failed", err, NULL), resp;
            json *data = resp_data(resp);
            keys_json(data, acts, nacts);
            json_set_bool(data, "down", down);
            json_set_str(data, "note", "use 'x68k step' to advance time while the key is held");
            set_ok(resp);
            return resp;
        }

        if (emu_keys_event(s->machine, acts, nacts, true, err, sizeof(err)) != 0)
            return cmd_error(resp, "input_failed", err, NULL), resp;
        emu_step_result st;
        memset(&st, 0, sizeof(st));
        step_hold(s, hold_sec, hold_frames, hold_nframes, &st);
        emu_keys_event(s->machine, acts, nacts, false, err, sizeof(err));
        emu_step_result st2;
        memset(&st2, 0, sizeof(st2));
        emu_step_frames(s->machine, 1, &st2, &g_stop);

        json *data = resp_data(resp);
        keys_json(data, acts, nacts);
        json_set_double(data, "hold_seconds", hold_frames ? (double)hold_nframes / emu_fps(s->machine) : hold_sec);
        json_set_int(data, "frames", (int64_t)(st.frames + st2.frames));
        json_set_double(data, "emu_seconds", st.emu_seconds + st2.emu_seconds);
        if (s->rec) check_recorder(s, data);
        set_ok(resp);
        return resp;
    }

    if (!strcmp(sub, "type")) {
        if (ntype_args == 0) return usage_error(resp, "nothing to type", NULL), resp;
        double delay_sec = 0.08;
        bool delay_frames = false;
        long long delay_nframes = 0;
        if (delay_s && !parse_duration(delay_s, &delay_sec, &delay_frames, &delay_nframes))
            return usage_error(resp, "invalid --delay duration", NULL), resp;

        /* decode escapes and validate everything before typing */
        char decoded[8192];
        size_t dn = 0;
        for (size_t i = 0; i < type_len; i++) {
            char c = type_buf[i];
            if (c == '\\' && i + 1 < type_len) {
                char n = type_buf[++i];
                if (n == 'n') c = '\n';
                else if (n == 't') c = '\t';
                else if (n == 'r') c = '\r';
                else if (n == '\\') c = '\\';
                else if (n == 'e') c = 27;
                else c = n;
            }
            unsigned char uc = (unsigned char)c;
            bool typeable = (uc >= 0x20 && uc <= 0x7e) || uc == '\n' || uc == '\t' ||
                            uc == '\r' || uc == '\b' || uc == 27;
            if (!typeable)
                return cmd_error(resp, "unsupported_character",
                                 "only printable ASCII text (plus \n, \t, \b and escape) can be typed",
                                 "use key tap for special keys"), resp;
            decoded[dn++] = c;
        }
        decoded[dn] = '\0';
        if (dn == 0) return usage_error(resp, "nothing to type", NULL), resp;

        uint64_t f0 = emu_frame(s->machine);
        double s0 = emu_seconds(s->machine);
        for (size_t i = 0; i < dn; i++) {
            key_action a;
            if (key_from_char((unsigned char)decoded[i], &a) != 0)
                return cmd_error(resp, "unsupported_character", "character cannot be typed", NULL), resp;
            bool shift = a.shift;
            a.shift = false;
            key_action shift_act;
            char kerr[64];
            key_lookup("shift", &shift_act, kerr, sizeof(kerr));
            if (shift) {
                emu_keys_event(s->machine, &shift_act, 1, true, err, sizeof(err));
                emu_step_result tmp;
                emu_step_frames(s->machine, 1, &tmp, &g_stop);
            }
            emu_keys_event(s->machine, &a, 1, true, err, sizeof(err));
            emu_step_result tmp;
            emu_step_frames(s->machine, 1, &tmp, &g_stop);
            emu_keys_event(s->machine, &a, 1, false, err, sizeof(err));
            if (shift) emu_keys_event(s->machine, &shift_act, 1, false, err, sizeof(err));
            emu_step_result tmp2;
            long long wait = delay_frames ? delay_nframes
                            : (long long)(delay_sec * emu_fps(s->machine) + 0.5);
            if (wait < 1) wait = 1;
            emu_step_frames(s->machine, wait, &tmp2, &g_stop);
            if (g_stop) break;
        }
        json *data = resp_data(resp);
        json_set_str(data, "typed", decoded);
        json_set_int(data, "chars", (int64_t)dn);
        json_set_int(data, "frames", (int64_t)(emu_frame(s->machine) - f0));
        json_set_double(data, "emu_seconds", emu_seconds(s->machine) - s0);
        if (s->rec) check_recorder(s, data);
        set_ok(resp);
        return resp;
    }

    return usage_error(resp, "unknown key subcommand", "key tap|down|up|hold|type"), resp;
}

static json *cmd_joy(server *s, int argc, char **argv) {
    json *resp = new_response("joy");
    if (argc < 2) return usage_error(resp, "joy needs a subcommand", "joy tap|down|up|hold ..."), resp;
    const char *sub = argv[1];
    const char *buttons[16];
    int nb = 0;
    const char *hold_s = NULL;
    int port = 1;
    for (int i = 2; i < argc; i++) {
        const char *v = NULL;
        int m;
        if ((m = match_opt(argc, argv, &i, "--hold", &v)) != 0) {
            if (m < 0) return usage_error(resp, "--hold requires a duration", NULL), resp;
            hold_s = v; continue;
        }
        if ((m = match_opt(argc, argv, &i, "--port", &v)) != 0) {
            if (m < 0) return usage_error(resp, "--port requires 1 or 2", NULL), resp;
            long long x;
            if (!parse_int(v, &x) || (x != 1 && x != 2))
                return usage_error(resp, "--port must be 1 or 2", NULL), resp;
            port = (int)x; continue;
        }
        if (argv[i][0] == '-' && argv[i][1] == '-')
            return usage_error(resp, "unknown option for joy", argv[i]), resp;
        if (nb >= 16) return usage_error(resp, "too many buttons", NULL), resp;
        buttons[nb++] = argv[i];
    }
    if (!s->has_machine)
        return cmd_error(resp, "not_booted", "no machine is running", "run 'x68k boot IMAGE ...' first"), resp;
    if (s->rec) check_recorder(s, resp_data(resp));
    if (nb == 0) return usage_error(resp, "no buttons given", "example: x68k joy tap a"), resp;

    char err[512] = {0};
    if (!strcmp(sub, "down") || !strcmp(sub, "up")) {
        bool down = !strcmp(sub, "down");
        for (int i = 0; i < nb; i++) {
            if (emu_joy_event(s->machine, port - 1, buttons[i], down, err, sizeof(err)) != 0)
                return cmd_error(resp, "input_failed", err, "buttons: up down left right a b x y l r start select"), resp;
        }
        json *data = resp_data(resp);
        json *arr = json_array();
        for (int i = 0; i < nb; i++) json_add_str(arr, buttons[i]);
        json_set(data, "buttons", arr);
        json_set_int(data, "port", port);
        json_set_bool(data, "down", down);
        set_ok(resp);
        return resp;
    }

    if (!strcmp(sub, "tap") || !strcmp(sub, "press") || !strcmp(sub, "hold")) {
        double hold_sec = 0.1;
        bool is_frames = false;
        long long nframes = 0;
        const char *real_buttons[16];
        int nreal = 0;
        if (!strcmp(sub, "hold")) {
            int dur_count = 0;
            const char *dur = NULL;
            for (int i = 0; i < nb; i++) {
                double ds;
                bool df;
                long long dn;
                if (parse_duration(buttons[i], &ds, &df, &dn)) {
                    dur_count++;
                    dur = buttons[i];
                } else {
                    real_buttons[nreal++] = buttons[i];
                }
            }
            if (dur_count != 1)
                return usage_error(resp, "joy hold needs exactly one duration",
                                   "example: x68k joy hold 1s right"), resp;
            if (!parse_duration(dur, &hold_sec, &is_frames, &nframes))
                return usage_error(resp, "invalid duration", NULL), resp;
            for (int i = 0; i < nreal; i++) buttons[i] = real_buttons[i];
            nb = nreal;
        } else if (hold_s) {
            if (!parse_duration(hold_s, &hold_sec, &is_frames, &nframes))
                return usage_error(resp, "invalid --hold duration", NULL), resp;
        }
        if (nb == 0) return usage_error(resp, "no buttons given", NULL), resp;

        for (int i = 0; i < nb; i++) {
            if (emu_joy_event(s->machine, port - 1, buttons[i], true, err, sizeof(err)) != 0)
                return cmd_error(resp, "input_failed", err, "buttons: up down left right a b x y l r start select"), resp;
        }
        emu_step_result st;
        memset(&st, 0, sizeof(st));
        step_hold(s, hold_sec, is_frames, nframes, &st);
        for (int i = 0; i < nb; i++)
            emu_joy_event(s->machine, port - 1, buttons[i], false, err, sizeof(err));
        emu_step_result st2;
        memset(&st2, 0, sizeof(st2));
        emu_step_frames(s->machine, 1, &st2, &g_stop);

        json *data = resp_data(resp);
        json *arr = json_array();
        for (int i = 0; i < nb; i++) json_add_str(arr, buttons[i]);
        json_set(data, "buttons", arr);
        json_set_int(data, "port", port);
        json_set_int(data, "frames", (int64_t)(st.frames + st2.frames));
        json_set_double(data, "emu_seconds", st.emu_seconds + st2.emu_seconds);
        if (s->rec) check_recorder(s, data);
        set_ok(resp);
        return resp;
    }

    return usage_error(resp, "unknown joy subcommand", "joy tap|down|up|hold"), resp;
}

static json *cmd_mouse(server *s, int argc, char **argv) {
    json *resp = new_response("mouse");
    if (argc < 2) return usage_error(resp, "mouse needs a subcommand", "mouse move|down|up|click ..."), resp;
    const char *sub = argv[1];
    const char *hold_s = NULL;
    const char *args[8];
    int nargs = 0;
    for (int i = 2; i < argc; i++) {
        const char *v = NULL;
        int m;
        if ((m = match_opt(argc, argv, &i, "--hold", &v)) != 0) {
            if (m < 0) return usage_error(resp, "--hold requires a duration", NULL), resp;
            hold_s = v; continue;
        }
        if (argv[i][0] == '-' && argv[i][1] == '-' && strcmp(argv[i], "-") != 0)
            return usage_error(resp, "unknown option for mouse", argv[i]), resp;
        if (nargs >= 8) return usage_error(resp, "too many arguments", NULL), resp;
        args[nargs++] = argv[i];
    }
    if (!s->has_machine)
        return cmd_error(resp, "not_booted", "no machine is running", "run 'x68k boot IMAGE ...' first"), resp;
    if (s->rec) check_recorder(s, resp_data(resp));
    char err[512] = {0};
    json *data = resp_data(resp);

    if (!strcmp(sub, "move")) {
        long long dx, dy;
        if (nargs != 2 || !parse_int(args[0], &dx) || !parse_int(args[1], &dy))
            return usage_error(resp, "mouse move needs DX DY", "example: x68k mouse move 20 -5"), resp;
        if (emu_mouse_move(s->machine, (int)dx, (int)dy, err, sizeof(err)) != 0)
            return cmd_error(resp, "input_failed", err, NULL), resp;
        json_set_int(data, "dx", dx);
        json_set_int(data, "dy", dy);
        json_set_str(data, "note", "movement is delivered during the next step");
        set_ok(resp);
        return resp;
    }
    if (!strcmp(sub, "down") || !strcmp(sub, "up")) {
        const char *btn = nargs >= 1 ? args[0] : "left";
        bool down = !strcmp(sub, "down");
        if (emu_mouse_button(s->machine, btn, down, err, sizeof(err)) != 0)
            return cmd_error(resp, "input_failed", err, NULL), resp;
        json_set_str(data, "button", btn);
        json_set_bool(data, "down", down);
        set_ok(resp);
        return resp;
    }
    if (!strcmp(sub, "click")) {
        const char *btn = nargs >= 1 ? args[0] : "left";
        double hold_sec = 0.1;
        bool is_frames = false;
        long long nframes = 0;
        if (hold_s && !parse_duration(hold_s, &hold_sec, &is_frames, &nframes))
            return usage_error(resp, "invalid --hold duration", NULL), resp;
        if (emu_mouse_button(s->machine, btn, true, err, sizeof(err)) != 0)
            return cmd_error(resp, "input_failed", err, NULL), resp;
        emu_step_result st;
        memset(&st, 0, sizeof(st));
        step_hold(s, hold_sec, is_frames, nframes, &st);
        emu_mouse_button(s->machine, btn, false, err, sizeof(err));
        emu_step_result st2;
        memset(&st2, 0, sizeof(st2));
        emu_step_frames(s->machine, 1, &st2, &g_stop);
        json_set_str(data, "button", btn);
        json_set_int(data, "frames", (int64_t)(st.frames + st2.frames));
        json_set_double(data, "emu_seconds", st.emu_seconds + st2.emu_seconds);
        if (s->rec) check_recorder(s, data);
        set_ok(resp);
        return resp;
    }
    return usage_error(resp, "unknown mouse subcommand", "mouse move|down|up|click"), resp;
}

static json *cmd_disk(server *s, int argc, char **argv) {
    json *resp = new_response("disk");
    if (argc < 2) return usage_error(resp, "disk needs a subcommand", "disk insert|eject|list"), resp;
    const char *sub = argv[1];
    if (!s->has_machine)
        return cmd_error(resp, "not_booted", "no machine is running", "run 'x68k boot IMAGE ...' first"), resp;

    if (!strcmp(sub, "list")) {
        json *data = resp_data(resp);
        json_set(data, "media", media_obj(s));
        set_ok(resp);
        return resp;
    }
    if (!strcmp(sub, "insert")) {
        if (argc < 4) return usage_error(resp, "disk insert needs DRIVE PATH", "example: x68k disk insert 1 diskB.dim"), resp;
        long long drive;
        if (!parse_int(argv[2], &drive) || (drive != 0 && drive != 1))
            return usage_error(resp, "drive must be 0 or 1", NULL), resp;
        char err[1024] = {0};
        if (emu_insert_fdd(s->machine, (int)drive, argv[3], err, sizeof(err)) != 0)
            return cmd_error(resp, "disk_error", err, NULL), resp;
        json *data = resp_data(resp);
        json_set(data, "media", media_obj(s));
        json_set_str(data, "note", "ejecting a disk flushes its write-back changes to the image file");
        set_ok(resp);
        return resp;
    }
    if (!strcmp(sub, "eject")) {
        if (argc < 3) return usage_error(resp, "disk eject needs DRIVE", NULL), resp;
        long long drive;
        if (!parse_int(argv[2], &drive) || (drive != 0 && drive != 1))
            return usage_error(resp, "drive must be 0 or 1", NULL), resp;
        char err[1024] = {0};
        if (emu_eject_fdd(s->machine, (int)drive, err, sizeof(err)) != 0)
            return cmd_error(resp, "disk_error", err, NULL), resp;
        json *data = resp_data(resp);
        json_set(data, "media", media_obj(s));
        set_ok(resp);
        return resp;
    }
    return usage_error(resp, "unknown disk subcommand", "disk insert|eject|list"), resp;
}

static bool state_name_ok(const char *name) {
    if (!name || !*name) return false;
    size_t n = strlen(name);
    if (n > 64) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
    }
    return strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static json *cmd_save(server *s, int argc, char **argv) {
    json *resp = new_response("save");
    const char *name = argc > 1 ? argv[1] : NULL;
    char auto_name[64];
    if (!s->has_machine)
        return cmd_error(resp, "not_booted", "no machine is running", "run 'x68k boot IMAGE ...' first"), resp;
    if (!name) {
        snprintf(auto_name, sizeof(auto_name), "state-%08llu", (unsigned long long)emu_frame(s->machine));
        name = auto_name;
    }
    if (!state_name_ok(name))
        return usage_error(resp, "state name may contain letters, digits, '.', '-' and '_' (max 64 chars)", NULL), resp;
    char file[128];
    snprintf(file, sizeof(file), "%s.bin", name);
    char *path = path_join(s->paths.states, file);
    if (mkdir_p(s->paths.states, 0755) != 0) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "cannot create %s: %s", s->paths.states, strerror(errno));
        free(path);
        return cmd_error(resp, "state_error", msg, NULL), resp;
    }
    char err[1024] = {0};
    size_t bytes = 0;
    if (emu_save_state(s->machine, path, &bytes, err, sizeof(err)) != 0) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "%s", err);
        free(path);
        return cmd_error(resp, "state_error", msg, NULL), resp;
    }
    json *data = resp_data(resp);
    json *info = json_object();
    json_set_str(info, "name", name);
    json_set_str(info, "path", path);
    json_set_int(info, "bytes", (int64_t)bytes);
    json_set_int(info, "frame", (int64_t)emu_frame(s->machine));
    json_set_double(info, "emu_seconds", emu_seconds(s->machine));
    json_set(data, "save", info);
    free(path);
    set_ok(resp);
    return resp;
}

static json *cmd_load(server *s, int argc, char **argv) {
    json *resp = new_response("load");
    if (!s->has_machine)
        return cmd_error(resp, "not_booted", "no machine is running", "run 'x68k boot IMAGE ...' first"), resp;
    if (argc < 2) return usage_error(resp, "load needs a state name", "example: x68k load state-00001234"), resp;
    const char *name = argv[1];
    if (!state_name_ok(name))
        return usage_error(resp, "invalid state name", NULL), resp;
    char file[128];
    snprintf(file, sizeof(file), "%s.bin", name);
    char *path = path_join(s->paths.states, file);
    if (!path_exists(path)) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "state not found: %s", path);
        free(path);
        return cmd_error(resp, "state_missing", msg, "use 'x68k save NAME' first"), resp;
    }
    char err[1024] = {0};
    if (emu_load_state(s->machine, path, err, sizeof(err)) != 0) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "%s", err);
        free(path);
        return cmd_error(resp, "state_error", msg, NULL), resp;
    }
    s->last_hash_valid = false;
    s->last_step_ms = 0;
    s->last_speed = 0;
    json *data = resp_data(resp);
    json *info = json_object();
    json_set_str(info, "name", name);
    json_set_str(info, "path", path);
    json_set_int(info, "frame", (int64_t)emu_frame(s->machine));
    json_set_double(info, "emu_seconds", emu_seconds(s->machine));
    json_set_str(info, "note", "run 'step 0.1s' to render the loaded screen");
    json_set(data, "load", info);
    free(path);
    set_ok(resp);
    return resp;
}

static json *cmd_states(server *s) {
    json *resp = new_response("states");
    json *data = resp_data(resp);
    json *arr = json_array();
    DIR *d = opendir(s->paths.states);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            size_t n = strlen(de->d_name);
            if (n > 4 && !strcmp(de->d_name + n - 4, ".bin")) {
                char *name = xstrndup(de->d_name, n - 4);
                json_add_str(arr, name);
                free(name);
            }
        }
        closedir(d);
    }
    json_set(data, "states", arr);
    set_ok(resp);
    return resp;
}

static json *cmd_reset(server *s) {
    json *resp = new_response("reset");
    if (!s->has_machine)
        return cmd_error(resp, "not_booted", "no machine is running", "run 'x68k boot IMAGE ...' first"), resp;
    emu_reset(s->machine);
    s->last_hash_valid = false;
    s->last_step_ms = 0;
    s->last_speed = 0;
    json *data = resp_data(resp);
    json_set_bool(data, "reset", true);
    json_set(data, "media", media_obj(s));
    set_ok(resp);
    return resp;
}

static json *cmd_status(server *s) {
    json *resp = new_response("status");
    json *data = resp_data(resp);
    json *daemon = json_object();
    json_set_int(daemon, "pid", (int64_t)getpid());
    json_set_str(daemon, "home", s->paths.home);
    json_set_str(daemon, "socket", s->paths.sock);
    json_set_double(daemon, "uptime_seconds", (double)(now_ms_monotonic() - s->start_ms) / 1000.0);
    json_set_int(daemon, "requests", (int64_t)s->request_count);
    json_set_str(daemon, "core", emu_core_version());
    json_set(data, "daemon", daemon);
    if (s->has_machine) {
        json *m = json_object();
        json_set_int(m, "ram_mb", emu_ram_mb(s->machine));
        json_set_int(m, "cpu_mhz", emu_cpu_mhz(s->machine));
        json_set_double(m, "fps", emu_fps(s->machine));
        json_set_str(m, "fps_mode", emu_compat_fps(s->machine) ? "compat" : "actual");
        json_set_str(m, "rom_source", s->cfg.rom_source);
        char keropi[X68K_PATH_MAX];
        snprintf(keropi, sizeof(keropi), "%s/keropi", emu_system_dir(s->machine));
        json_set_str(m, "system_dir", keropi);
        json_set(data, "machine", m);
        json_set(data, "media", media_obj(s));
        json_set(data, "recording", recording_obj(s));
    }
    set_ok(resp);
    return resp;
}

static json *cmd_ping(server *s) {
    json *resp = new_response("ping");
    json *data = resp_data(resp);
    json_set_bool(data, "daemon", true);
    json_set_bool(data, "booted", s->has_machine);
    json_set_str(data, "core", emu_core_version());
    set_ok(resp);
    return resp;
}

static json *cmd_quit(server *s) {
    json *resp = new_response("quit");
    json *data = resp_data(resp);
    if (s->rec) {
        int rc = finalize_recording(s, data);
        if (rc != 0) json_set_bool(data, "recording_finalize_failed", true);
    }
    stop_machine(s);
    s->quit_requested = 1;
    json_set_bool(data, "stopped", true);
    set_ok(resp);
    return resp;
}

/* ------------------------------------------------------------------ */
/* dispatch                                                            */
/* ------------------------------------------------------------------ */

static json *dispatch(server *s, int argc, char **argv) {
    if (argc < 1) {
        json *r = new_response("");
        usage_error(r, "missing command", "run 'x68k help'");
        return r;
    }
    const char *cmd = argv[0];
    s->request_count++;
    if (!strcmp(cmd, "boot")) return cmd_boot(s, argc, argv);
    if (!strcmp(cmd, "step") || !strcmp(cmd, "advance") || !strcmp(cmd, "run"))
        return cmd_step(s, argc, argv, cmd);
    if (!strcmp(cmd, "snap") || !strcmp(cmd, "screenshot") || !strcmp(cmd, "snapshot") || !strcmp(cmd, "screen"))
        return cmd_snap(s, argc, argv, cmd);
    if (!strcmp(cmd, "record")) return cmd_record(s, argc, argv);
    if (!strcmp(cmd, "key")) return cmd_key(s, argc, argv);
    if (!strcmp(cmd, "joy") || !strcmp(cmd, "joystick")) return cmd_joy(s, argc, argv);
    if (!strcmp(cmd, "mouse")) return cmd_mouse(s, argc, argv);
    if (!strcmp(cmd, "disk") || !strcmp(cmd, "media")) return cmd_disk(s, argc, argv);
    if (!strcmp(cmd, "save")) return cmd_save(s, argc, argv);
    if (!strcmp(cmd, "load")) return cmd_load(s, argc, argv);
    if (!strcmp(cmd, "states")) return cmd_states(s);
    if (!strcmp(cmd, "reset")) return cmd_reset(s);
    if (!strcmp(cmd, "status")) return cmd_status(s);
    if (!strcmp(cmd, "ping")) return cmd_ping(s);
    if (!strcmp(cmd, "quit") || !strcmp(cmd, "shutdown")) return cmd_quit(s);
    json *r = new_response(cmd);
    usage_error(r, "unknown command", "run 'x68k help'");
    return r;
}

/* ------------------------------------------------------------------ */
/* daemon loop                                                         */
/* ------------------------------------------------------------------ */

static void handle_connection(server *s, int fd) {
    ipc_set_timeout(fd, 30 * 1000, 30 * 1000);
    for (;;) {
        int argc = 0;
        char **argv = NULL;
        int rc = ipc_recv_request(fd, &argc, &argv);
        if (rc != 0) break;
        json *resp;
        if (g_stop && argc > 0 && strcmp(argv[0], "quit") != 0) {
            resp = new_response(argv[0] ? argv[0] : "");
            set_error(resp, "shutting_down", "the daemon is shutting down", NULL, 1);
        } else {
            resp = dispatch(s, argc, argv);
        }
        if (!s->quit_requested && s->has_machine && json_get_bool(resp, "ok", false))
            add_state(s, resp);
        char *js = json_dump(resp, false);
        ipc_send_response(fd, js, strlen(js));
        free(js);
        json_free(resp);
        ipc_free_request(argc, argv);
        if (s->quit_requested) {
            g_stop = 1;
            break;
        }
    }
}

int server_run(const char *home, bool foreground) {
    server srv;
    memset(&srv, 0, sizeof(srv));
    srv.listen_fd = -1;
    srv.lock_fd = -1;
    srv.start_ms = now_ms_monotonic();

    char err[2048] = {0};
    app_paths_init(&srv.paths, home);
    if (app_make_dirs(&srv.paths, err, sizeof(err)) != 0) {
        xlog("fatal: %s", err);
        return 1;
    }
    if (chdir(srv.paths.home) != 0) {
        xlog("warning: cannot chdir to %s: %s", srv.paths.home, strerror(errno));
    }

    srv.lock_fd = open(srv.paths.lockfile, O_RDWR | O_CREAT, 0600);
    if (srv.lock_fd < 0 || flock(srv.lock_fd, LOCK_EX | LOCK_NB) != 0) {
        xlog("another daemon is already running for %s", srv.paths.home);
        return 0;
    }

    fcntl(srv.lock_fd, F_SETFD, FD_CLOEXEC);

    /* Write the pid before creating the socket so a concurrently starting
     * client sees a live daemon and waits instead of removing the socket. */
    FILE *pf = fopen(srv.paths.pidfile, "w");
    if (pf) {
        fprintf(pf, "%ld\n", (long)getpid());
        fclose(pf);
    }

    srv.listen_fd = ipc_listen(srv.paths.sock);
    if (srv.listen_fd < 0) {
        xlog("fatal: cannot listen on %s: %s", srv.paths.sock, strerror(errno));
        unlink(srv.paths.pidfile);
        return 1;
    }
    fcntl(srv.listen_fd, F_SETFD, FD_CLOEXEC);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    xlog("x68k daemon listening on %s (pid %ld, home %s)%s", srv.paths.sock, (long)getpid(),
         srv.paths.home, foreground ? " [foreground]" : "");

    while (!g_stop) {
        int cfd = accept(srv.listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (errno == EBADF) break;
            xlog("accept failed: %s", strerror(errno));
            break;
        }
        fcntl(cfd, F_SETFD, FD_CLOEXEC);
        handle_connection(&srv, cfd);
        close(cfd);
    }

    xlog("daemon shutting down");
    if (srv.rec) {
        if (srv.machine) emu_set_recorder(srv.machine, NULL);
        char e[1024] = {0};
        recorder_stop(srv.rec, e, sizeof(e));
        srv.rec = NULL;
    }
    if (srv.machine) {
        emu_destroy(srv.machine);
        srv.machine = NULL;
    }
    if (srv.listen_fd >= 0) close(srv.listen_fd);
    unlink(srv.paths.sock);
    unlink(srv.paths.pidfile);
    if (srv.lock_fd >= 0) close(srv.lock_fd);
    return 0;
}
