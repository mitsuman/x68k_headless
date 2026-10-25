#include "client.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "ipc.h"
#include "ipc.h"
#include "json.h"
#include "log.h"
#include "util.h"

/* Rewrites compact JSON to indented JSON without parsing it. */
char *json_pretty_text(const char *json) {
    strbuf b;
    sb_init(&b);
    int indent = 0;
    bool instr = false, esc = false;
    for (const char *p = json; *p; p++) {
        char c = *p;
        if (instr) {
            sb_putc(&b, c);
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') instr = false;
            continue;
        }
        switch (c) {
        case '"':
            instr = true;
            sb_putc(&b, c);
            break;
        case '{':
        case '[': {
            char close = c == '{' ? '}' : ']';
            const char *q = p + 1;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            sb_putc(&b, c);
            if (*q == close) {
                sb_putc(&b, close);
                p = q;
            } else {
                indent++;
                sb_putc(&b, '\n');
                for (int i = 0; i < indent; i++) sb_puts(&b, "  ");
            }
            break;
        }
        case '}':
        case ']':
            if (indent > 0) {
                indent--;
                sb_putc(&b, '\n');
                for (int i = 0; i < indent; i++) sb_puts(&b, "  ");
            }
            sb_putc(&b, c);
            break;
        case ',':
            sb_putc(&b, c);
            sb_putc(&b, '\n');
            for (int i = 0; i < indent; i++) sb_puts(&b, "  ");
            break;
        case ':':
            sb_puts(&b, ": ");
            break;
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            break;
        default:
            sb_putc(&b, c);
            break;
        }
    }
    return sb_take(&b);
}

static int extract_exit(const char *json, int def) {
    const char *p = strstr(json, "\"exit\":");
    if (!p) return def;
    p += 7;
    while (*p == ' ') p++;
    int v = 0;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    while (isdigit((unsigned char)*p)) v = v * 10 + (*p++ - '0');
    return neg ? -v : v;
}

static void print_response(const char *json, bool pretty) {
    char *out = pretty ? json_pretty_text(json) : xstrdup(json);
    fputs(out, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    free(out);
}


/* Make path arguments absolute so the daemon (cwd = ~/.x68k) sees what the
 * user meant. */
static char *replace_path_value(const char *prefix, const char *value) {
    char *abs = path_absolute(value);
    size_t n = strlen(prefix) + strlen(abs) + 1;
    char *out = xmalloc(n);
    snprintf(out, n, "%s%s", prefix, abs);
    free(abs);
    return out;
}

/* Make path arguments absolute so the daemon (cwd = ~/.x68k) sees what the
 * user meant.  Only known path options and boot's positional images are
 * rewritten; everything else is forwarded verbatim. */
static char **prepare_request(int argc, char **argv, const char *cmd) {
    char **req = xcalloc((size_t)argc + 1, sizeof(char *));
    bool boot = !strcmp(cmd, "boot");
    bool snap = !strcmp(cmd, "snap") || !strcmp(cmd, "screenshot") ||
                !strcmp(cmd, "snapshot") || !strcmp(cmd, "screen");
    bool step = !strcmp(cmd, "step") || !strcmp(cmd, "advance") || !strcmp(cmd, "run");
    bool rec = !strcmp(cmd, "record");
    bool disk = !strcmp(cmd, "disk") || !strcmp(cmd, "media");
    bool boot_skip_value = false;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (i == 0) {
            req[i] = xstrdup(a);
            continue;
        }
        if (boot_skip_value) {
            req[i] = xstrdup(a);
            boot_skip_value = false;
            continue;
        }
        if (boot) {
            if (!strcmp(a, "--roms")) {
                req[i] = xstrdup(a);
                if (i + 1 < argc) { int v = ++i; req[v] = path_absolute(argv[v]); }
                continue;
            }
            if (!strcmp(a, "--hdd") || !strcmp(a, "--fdd0") || !strcmp(a, "--fdd1")) {
                req[i] = xstrdup(a);
                if (i + 1 < argc) { int v = ++i; req[v] = path_absolute(argv[v]); }
                continue;
            }
            if (!strncmp(a, "--roms=", 7)) {
                req[i] = replace_path_value("--roms=", a + 7);
                continue;
            }
            if (!strncmp(a, "--hdd=", 6)) {
                req[i] = replace_path_value("--hdd=", a + 6);
                continue;
            }
            if (!strncmp(a, "--fdd0=", 7)) {
                req[i] = replace_path_value("--fdd0=", a + 7);
                continue;
            }
            if (!strncmp(a, "--fdd1=", 7)) {
                req[i] = replace_path_value("--fdd1=", a + 7);
                continue;
            }
        }
        if ((snap || step || rec) && (!strcmp(a, "--out") || !strcmp(a, "-o"))) {
            req[i] = xstrdup(a);
            if (i + 1 < argc) { int v = ++i; req[v] = path_absolute(argv[v]); }
            continue;
        }
        if ((snap || step || rec) && !strncmp(a, "--out=", 6)) {
            req[i] = replace_path_value("--out=", a + 6);
            continue;
        }
        if (disk && argc > 1 && !strcmp(argv[1], "insert") && i == 3) {
            req[i] = path_absolute(a);
            continue;
        }
        if (boot) {
            if (a[0] == '-') {
                if (!strchr(a, '=')) boot_skip_value = true;
                req[i] = xstrdup(a);
                continue;
            }
            req[i] = path_absolute(a);
            continue;
        }
        req[i] = xstrdup(a);
    }
    return req;
}

static void free_request(int argc, char **req) {
    if (!req) return;
    for (int i = 0; i < argc; i++) free(req[i]);
    free(req);
}

static int print_client_error(const char *cmd, const char *code, const char *message, int exit_code,
                              bool pretty) {
    json *r = json_object();
    json_set_bool(r, "ok", false);
    if (cmd) json_set_str(r, "command", cmd);
    json *e = json_object();
    json_set_str(e, "code", code);
    json_set_str(e, "message", message);
    json_set_str(e, "hint", "run 'x68k doctor' and check the daemon log; the next command starts an idle daemon");
    json_set_int(e, "exit", exit_code);
    json_set(r, "error", e);
    char *js = json_dump(r, false);
    print_response(js, pretty);
    free(js);
    json_free(r);
    return exit_code;
}

static int send_one(int fd, int argc, char **argv, bool pretty, const char *cmd) {
    char **req = prepare_request(argc, argv, cmd);
    int rc = ipc_send_request(fd, argc, req);
    free_request(argc, req);
    if (rc != 0) {
        return print_client_error(cmd, "ipc_send_failed", "failed to send the request to the daemon", 3, pretty);
    }
    size_t len = 0;
    char *resp = ipc_recv_response(fd, &len);
    if (!resp) {
        return print_client_error(cmd, "daemon_closed", "the daemon closed the connection", 3, pretty);
    }
    print_response(resp, pretty);
    int code = extract_exit(resp, 0);
    if (strstr(resp, "\"ok\":false")) {
        if (code == 0) code = 1;
    } else {
        code = 0;
    }
    free(resp);
    return code;
}

int client_run(const char *home, int argc, char **argv, bool pretty, int timeout_ms) {
    app_paths pp;
    app_paths_init(&pp, home);
    char err[4096] = {0};
    if (app_make_dirs(&pp, err, sizeof(err)) != 0) {
        char *js = NULL;
        json *r = json_object();
        json_set_bool(r, "ok", false);
        json *e = json_object();
        json_set_str(e, "code", "home_error");
        json_set_str(e, "message", err);
        json_set_int(e, "exit", 3);
        json_set(r, "error", e);
        js = json_dump(r, false);
        print_response(js, pretty);
        free(js);
        json_free(r);
        return 3;
    }

    int fd = app_connect_or_start(&pp, 8000, err, sizeof(err));
    if (fd < 0) {
        json *r = json_object();
        json_set_bool(r, "ok", false);
        json *e = json_object();
        json_set_str(e, "code", "daemon_unavailable");
        json_set_str(e, "message", err);
        json_set_str(e, "hint", "run 'x68k daemon --foreground' to see the daemon log");
        json_set_int(e, "exit", 3);
        json_set(r, "error", e);
        char *js = json_dump(r, false);
        print_response(js, pretty);
        free(js);
        json_free(r);
        return 3;
    }
    ipc_set_timeout(fd, 30 * 1000, timeout_ms);
    int rc = send_one(fd, argc, argv, pretty, argv[0]);
    ipc_close(fd);

    if (!strcmp(argv[0], "quit") && rc == 0) {
        /* wait for the socket to disappear (best effort) */
        for (int i = 0; i < 60; i++) {
            int t = ipc_connect(pp.sock);
            if (t < 0) break;
            ipc_close(t);
            usleep(50 * 1000);
        }
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* batch mode                                                          */
/* ------------------------------------------------------------------ */

static int tokenize(const char *line, char **out, int max) {
    int n = 0;
    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p || *p == '#') break;
        if (n >= max) {
            for (int i = 0; i < n; i++) free(out[i]);
            return -1;
        }
        strbuf tok;
        sb_init(&tok);
        char quote = 0;
        while (*p) {
            char c = *p;
            if (quote) {
                if (c == quote) { quote = 0; p++; }
                else if (c == '\\' && quote == '"' && p[1]) {
                    p++;
                    char e = *p++;
                    if (e == 'n') sb_putc(&tok, '\n');
                    else if (e == 't') sb_putc(&tok, '\t');
                    else sb_putc(&tok, e);
                } else {
                    sb_putc(&tok, c);
                    p++;
                }
                continue;
            }
            if (c == '\'' || c == '"') { quote = c; p++; continue; }
            if (c == '\\' && p[1]) {
                p++;
                char e = *p++;
                if (e == 'n') sb_putc(&tok, '\n');
                else if (e == 't') sb_putc(&tok, '\t');
                else sb_putc(&tok, e);
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
            sb_putc(&tok, c);
            p++;
        }
        if (quote) {
            sb_free(&tok);
            for (int i = 0; i < n; i++) free(out[i]);
            return -1;
        }
        out[n++] = sb_take(&tok);
    }
    return n;
}

static void json_escape_into(strbuf *b, const char *s) {
    json *j = json_string(s);
    char *d = json_dump(j, false);
    sb_puts(b, d);
    free(d);
    json_free(j);
}

int client_batch(const char *home, const char *file, bool pretty, int timeout_ms, bool keep_going) {
    FILE *f = file ? fopen(file, "r") : stdin;
    if (!f) {
        fprintf(stderr, "x68k: cannot open %s: %s\n", file, strerror(errno));
        return 3;
    }

    app_paths pp;
    app_paths_init(&pp, home);
    char err[4096] = {0};
    if (app_make_dirs(&pp, err, sizeof(err)) != 0) {
        fprintf(stderr, "x68k: %s\n", err);
        if (file) fclose(f);
        return 3;
    }
    int fd = app_connect_or_start(&pp, 8000, err, sizeof(err));
    if (fd < 0) {
        fprintf(stderr, "x68k: %s\n", err);
        if (file) fclose(f);
        return 3;
    }
    ipc_set_timeout(fd, 30 * 1000, timeout_ms);

    strbuf out;
    sb_init(&out);
    sb_puts(&out, "{\"ok\":true,\"command\":\"batch\",\"data\":{\"results\":[");
    int count = 0;
    bool all_ok = true, stopped = false;
    char line[8192];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *args[32];
        int n = tokenize(line, args, 32);
        if (n == 0) continue;
        if (n < 0) {
            all_ok = false;
            if (!keep_going) {
                stopped = true;
                break;
            }
            continue;
        }
        if (count) sb_puts(&out, ",");
        sb_puts(&out, "{\"line\":");
        sb_printf(&out, "%d,\"argv\":[", lineno);
        for (int i = 0; i < n; i++) {
            if (i) sb_puts(&out, ",");
            json_escape_into(&out, args[i]);
        }
        sb_puts(&out, "],\"response\":");

        char **req = prepare_request(n, args, args[0]);
        int rc = ipc_send_request(fd, n, req);
        free_request(n, req);
        if (rc != 0) {
            sb_puts(&out, "{\"ok\":false,\"error\":{\"code\":\"ipc\",\"message\":\"send failed\"}}}");
            all_ok = false;
            for (int i = 0; i < n; i++) free(args[i]);
            break;
        }
        size_t len = 0;
        char *resp = ipc_recv_response(fd, &len);
        if (!resp) {
            sb_puts(&out, "{\"ok\":false,\"error\":{\"code\":\"ipc\",\"message\":\"daemon closed connection\"}}}");
            all_ok = false;
            for (int i = 0; i < n; i++) free(args[i]);
            break;
        }
        sb_puts(&out, resp);
        sb_puts(&out, "}");
        if (strstr(resp, "\"ok\":false")) {
            all_ok = false;
            if (!keep_going) {
                stopped = true;
                free(resp);
                for (int i = 0; i < n; i++) free(args[i]);
                count++;
                break;
            }
        }
        free(resp);
        for (int i = 0; i < n; i++) free(args[i]);
        count++;
    }
    sb_puts(&out, "],\"count\":");
    sb_printf(&out, "%d,\"all_ok\":%s,\"stopped_early\":%s}}", count, all_ok ? "true" : "false",
              stopped ? "true" : "false");
    if (file) fclose(f);
    ipc_close(fd);
    char *s = sb_take(&out);
    print_response(s, pretty);
    free(s);
    return all_ok ? 0 : 1;
}
