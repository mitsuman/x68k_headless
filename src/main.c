#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "client.h"
#include "emu.h"
#include "ipc.h"
#include "json.h"
#include "keys.h"
#include "log.h"
#include "server.h"
#include "util.h"

#ifndef X68K_VERSION
#define X68K_VERSION "0.1.0"
#endif

static const char *HELP_TEXT =
    "x68k - headless X68000 emulator for scripted / LLM play\n"
    "\n"
    "Usage: x68k [global options] <command> [args]\n"
    "\n"
    "Global options (before the command):\n"
    "  --home DIR        session directory (default: $X68K_HOME or ~/.x68k)\n"
    "  --pretty          pretty-print JSON output\n"
    "  --timeout SEC     wait at most SEC seconds for a command (default 600)\n"
    "\n"
    "Setup:\n"
    "  X68000 ROMs are required but not included. Put iplrom.dat and cgrom.dat\n"
    "  in a directory and pass --roms DIR (or set X68K_ROM_DIR).\n"
    "  ffmpeg must be on PATH for recording.\n"
    "\n"
    "Commands:\n"
    "  boot [IMAGE ...] [--fdd0 P] [--fdd1 P] [--hdd P] [--roms DIR]\n"
    "       [--ram 1..12] [--cpu 10|16|25|33|66|100] [--fps actual|compat]\n"
    "       [--joy1 T] [--joy2 T]\n"
    "  step DURATION [--snap] [--out PATH] [--scale N]\n"
    "  step --frames N [...same options]\n"
    "  snap [-o PATH] [--scale N]\n"
    "  record start [--out PATH] [--fps N] [--size WxH] [--no-audio]\n"
    "  record stop\n"
    "  record status\n"
    "  key tap|press <chord...> [--hold DUR]\n"
    "  key down|up <chord...>\n"
    "  key hold <DURATION> <chord...>\n"
    "  key type \"TEXT\" [--delay DUR]\n"
    "  joy tap|down|up <button...> [--hold DUR] [--port 1|2]\n"
    "  joy hold <DURATION> <button...> [--port 1|2]\n"
    "  mouse move DX DY\n"
    "  mouse down|up [left|right]\n"
    "  mouse click [left|right] [--hold DUR]\n"
    "  disk insert DRIVE PATH | disk eject DRIVE | disk list\n"
    "  save [NAME] | load NAME | states\n"
    "  reset | status | ping | quit\n"
    "  batch [--file FILE] [--keep-going]\n"
    "  doctor | help [keys] | version\n"
    "\n"
    "DURATION: 1.5s, 500ms, 2m, or 90f for frames. A bare number means seconds.\n"
    "Joystick buttons: up down left right a b x y l r start select\n"
    "\n"
    "Typical loop:\n"
    "  x68k boot \"game.dim\" --roms ~/Documents/MPX68K\n"
    "  x68k step 12s --snap          # PNG path is in data.snapshot.path\n"
    "  x68k key tap return\n"
    "  x68k joy hold 1s right --snap\n"
    "  x68k record start\n"
    "  x68k step 5s\n"
    "  x68k record stop\n"
    "  x68k quit\n"
    "\n"
    "Notes:\n"
    "  - Emulation advances only when a command advances time (step, tap, hold,\n"
    "    click, type). Nothing runs in real time in the background.\n"
    "  - Screenshots are native X68000 pixels (usually 512x512 or 512x424).\n"
    "    Recordings are 640x480 by default with stereo audio.\n"
    "  - Every command prints one JSON object on stdout. Exit codes: 0 = ok,\n"
    "    1 = command error, 2 = usage error, 3 = daemon error.\n"
    "  - 'x68k help keys' lists all key names.\n"
    "\n"
    "Global --pretty/--compact may also be written after the command.\n";

static void print_json(json *v, bool pretty) {
    char *s = json_dump(v, pretty);
    fputs(s, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    free(s);
    json_free(v);
}

static int usage_json(const char *cmd, const char *msg, const char *hint, bool pretty) {
    json *r = json_object();
    json_set_bool(r, "ok", false);
    if (cmd) json_set_str(r, "command", cmd);
    json *e = json_object();
    json_set_str(e, "code", "usage");
    json_set_str(e, "message", msg);
    if (hint) json_set_str(e, "hint", hint);
    json_set_int(e, "exit", 2);
    json_set(r, "error", e);
    print_json(r, pretty);
    return 2;
}

static int cmd_version(bool pretty) {
    json *r = json_object();
    json_set_bool(r, "ok", true);
    json_set_str(r, "command", "version");
    json *d = json_object();
    json_set_str(d, "version", X68K_VERSION);
    json_set_str(d, "core", emu_core_version());
    json_set_str(d, "home_default", "~/.x68k");
    json_set(r, "data", d);
    print_json(r, pretty);
    return 0;
}

static void add_rom_candidate(json *arr, const char *dir) {
    if (!dir || !*dir) return;
    char *ip = find_file_ci(dir, "iplrom.dat");
    char *cg = find_file_ci(dir, "cgrom.dat");
    if (!ip && !cg) {
        free(ip);
        free(cg);
        return;
    }
    json *o = json_object();
    json_set_str(o, "dir", dir);
    json_set_bool(o, "iplrom", ip != NULL);
    json_set_bool(o, "cgrom", cg != NULL);
    json_add(arr, o);
    free(ip);
    free(cg);
}

static int cmd_doctor(const char *home, bool pretty) {
    app_paths pp;
    app_paths_init(&pp, home);
    json *r = json_object();
    json_set_bool(r, "ok", true);
    json_set_str(r, "command", "doctor");
    json *d = json_object();

    json_set_str(d, "version", X68K_VERSION);
    json_set_str(d, "core", emu_core_version());
    json_set_str(d, "home", pp.home);

    json *daemon = json_object();
    bool alive = app_daemon_alive(&pp);
    int t = ipc_connect(pp.sock);
    bool reachable = t >= 0;
    if (reachable) ipc_close(t);
    json_set_bool(daemon, "running", reachable);
    json_set_bool(daemon, "pid_alive", alive);
    json_set_str(daemon, "socket", pp.sock);
    json_set(d, "daemon", daemon);

    json *ff = json_object();
    FILE *p = popen("ffmpeg -version 2>/dev/null | head -1", "r");
    char ver[256] = {0};
    if (p) {
        if (fgets(ver, sizeof(ver), p)) {
            char *nl = strchr(ver, '\n');
            if (nl) *nl = '\0';
        }
        pclose(p);
    }
    json_set_bool(ff, "found", ver[0] != 0);
    if (ver[0]) json_set_str(ff, "version", ver);
    json_set(d, "ffmpeg", ff);

    json *roms = json_object();
    char *prep = path_join(pp.home, "roms/keropi");
    json_set_str(roms, "prepared_dir", prep);
    char *pip = find_file_ci(prep, "iplrom.dat");
    char *pcg = find_file_ci(prep, "cgrom.dat");
    json *prepared = json_object();
    json_set_bool(prepared, "iplrom", pip != NULL);
    json_set_bool(prepared, "cgrom", pcg != NULL);
    json_set(roms, "prepared", prepared);
    free(pip);
    free(pcg);
    free(prep);

    json *cands = json_array();
    char *env = env_dup("X68K_ROM_DIR");
    if (env) {
        add_rom_candidate(cands, env);
        free(env);
    }
    char *h = env_home_dir();
    char *c0 = path_join(pp.home, "roms/keropi");
    char *c1 = path_join(pp.home, "roms");
    char *c2 = path_join(h, "Documents/MPX68K");
    char *c3 = path_join(h, "x68k");
    char *c4 = path_join(h, "x68k/roms");
    add_rom_candidate(cands, c0);
    add_rom_candidate(cands, c1);
    add_rom_candidate(cands, c2);
    add_rom_candidate(cands, c3);
    add_rom_candidate(cands, c4);
    add_rom_candidate(cands, ".");
    free(c0); free(c1); free(c2); free(c3); free(c4); free(h);
    json_set(roms, "candidates", cands);
    json_set_str(roms, "note", "ROMs are not bundled; pass --roms DIR or set X68K_ROM_DIR");
    json_set(d, "roms", roms);

    json *dirs = json_object();
    json_set_str(dirs, "screens", pp.screens);
    json_set_str(dirs, "recordings", pp.recordings);
    char *probe = xstrdup(pp.home);
    while (!path_exists(probe)) {
        char *up = path_dirname(probe);
        free(probe);
        probe = up;
        if (!strcmp(probe, "/") || !strcmp(probe, ".")) break;
    }
    json_set_bool(dirs, "writable", access(probe, W_OK) == 0);
    json_set_str(dirs, "home_exists", path_exists(pp.home) ? "yes" : "no");
    free(probe);
    json_set(d, "dirs", dirs);
    json_set(r, "data", d);
    print_json(r, pretty);
    return 0;
}

static int do_help(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "keys")) {
        char *k = keys_help_text();
        fputs(k, stdout);
        free(k);
        return 0;
    }
    fputs(HELP_TEXT, stdout);
    return 0;
}

static int run_daemon(int argc, char **argv, const char *home_override) {
    const char *home = home_override;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--home") && i + 1 < argc) {
            home = argv[++i];
        } else if (!strncmp(argv[i], "--home=", 7)) {
            home = argv[i] + 7;
        } else if (!strcmp(argv[i], "--foreground") || !strcmp(argv[i], "-f")) {
            /* default */
        } else {
            fprintf(stderr, "x68k daemon: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    return server_run(home, true);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *home = NULL;
    bool pretty = false;
    int timeout_sec = 600;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--home")) {
            if (i + 1 >= argc) return usage_json(NULL, "--home requires a directory", NULL, pretty);
            home = argv[++i];
        } else if (!strncmp(argv[i], "--home=", 7)) {
            home = argv[i] + 7;
        } else if (!strcmp(argv[i], "--pretty") || !strcmp(argv[i], "-p")) {
            pretty = true;
        } else if (!strcmp(argv[i], "--compact")) {
            pretty = false;
        } else if (!strcmp(argv[i], "--timeout")) {
            if (i + 1 >= argc) return usage_json(NULL, "--timeout requires seconds", NULL, pretty);
            long long v;
            if (!parse_int(argv[++i], &v) || v < 1 || v > 86400)
                return usage_json(NULL, "--timeout must be 1..86400 seconds", NULL, pretty);
            timeout_sec = (int)v;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fputs(HELP_TEXT, stdout);
            return 0;
        } else if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-V")) {
            return cmd_version(pretty);
        } else if (argv[i][0] == '-' && argv[i][1]) {
            return usage_json(NULL, "unknown global option", argv[i], pretty);
        } else {
            break;
        }
    }
    if (i >= argc) {
        fputs(HELP_TEXT, stdout);
        return 2;
    }

    const char *cmd = argv[i];
    int cargc = 1;
    char **cargv = xcalloc((size_t)(argc - i) + 1, sizeof(char *));
    cargv[0] = (char *)cmd;
    for (int j = i + 1; j < argc; j++) {
        if (!strcmp(argv[j], "--pretty") || !strcmp(argv[j], "-p")) {
            pretty = true;
        } else if (!strcmp(argv[j], "--compact")) {
            pretty = false;
        } else {
            cargv[cargc++] = argv[j];
        }
    }

    int rc;
    if (!strcmp(cmd, "help") || !strcmp(cmd, "keys")) {
        rc = do_help(cargc, cargv);
    } else if (!strcmp(cmd, "version")) {
        rc = cmd_version(pretty);
    } else if (!strcmp(cmd, "doctor")) {
        rc = cmd_doctor(home, pretty);
    } else if (!strcmp(cmd, "daemon")) {
        rc = run_daemon(cargc, cargv, home);
    } else if (!strcmp(cmd, "batch")) {
        const char *file = NULL;
        bool keep_going = false;
        for (int j = 1; j < cargc; j++) {
            if (!strcmp(cargv[j], "--file") && j + 1 < cargc) file = cargv[++j];
            else if (!strncmp(cargv[j], "--file=", 7)) file = cargv[j] + 7;
            else if (!strcmp(cargv[j], "--keep-going")) keep_going = true;
            else {
                return usage_json("batch", "unknown batch option", cargv[j], pretty);
            }
        }
        char *abs = file ? path_absolute(file) : NULL;
        rc = client_batch(home, abs, pretty, timeout_sec * 1000, keep_going);
        free(abs);
    } else {
        rc = client_run(home, cargc, cargv, pretty, timeout_sec * 1000);
    }
    free(cargv);
    return rc;
}
