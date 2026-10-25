#include "app.h"
#include "ipc.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static void set_path(char *dst, size_t n, const char *a, const char *b) {
    char *p = path_join(a, b);
    snprintf(dst, n, "%s", p);
    free(p);
}

void app_paths_init(app_paths *p, const char *override) {
    memset(p, 0, sizeof(*p));
    char *home = NULL;
    if (override && *override) {
        home = path_absolute(override);
    } else {
        home = env_dup("X68K_HOME");
        if (!home) {
            char *h = env_home_dir();
            home = path_join(h, ".x68k");
            free(h);
        } else {
            char *a = path_absolute(home);
            free(home);
            home = a;
        }
    }
    snprintf(p->home, sizeof(p->home), "%s", home ? home : ".");
    free(home);
    set_path(p->sock, sizeof(p->sock), p->home, "daemon.sock");
    set_path(p->pidfile, sizeof(p->pidfile), p->home, "daemon.pid");
    set_path(p->lockfile, sizeof(p->lockfile), p->home, "daemon.lock");
    set_path(p->logfile, sizeof(p->logfile), p->home, "daemon.log");
    set_path(p->screens, sizeof(p->screens), p->home, "screens");
    set_path(p->recordings, sizeof(p->recordings), p->home, "recordings");
    set_path(p->states, sizeof(p->states), p->home, "states");
    set_path(p->roms, sizeof(p->roms), p->home, "roms");
}

int app_make_dirs(const app_paths *p, char *err, size_t errlen) {
    if (strlen(p->sock) >= 100) {
        snprintf(err, errlen,
                 "the session path is too long for a unix socket (%s); choose a shorter --home",
                 p->sock);
        return -1;
    }
    const char *dirs[] = {p->home, p->screens, p->recordings, p->states, p->roms};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (mkdir_p(dirs[i], i == 0 ? 0700 : 0755) != 0) {
            snprintf(err, errlen, "cannot create directory %s: %s", dirs[i], strerror(errno));
            return -1;
        }
    }
    return 0;
}

char *app_exe_path(void) {
#ifdef __APPLE__
    char buf[X68K_PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return NULL;
    return path_real(buf);
#else
    char buf[X68K_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return NULL;
    buf[n] = '\0';
    return path_real(buf);
#endif
}

void app_spawn_daemon(const app_paths *p, const char *exe) {
    if (!exe) {
        xlog("cannot find own executable path; cannot start daemon");
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        xlog("fork failed: %s", strerror(errno));
        return;
    }
    if (pid > 0) return;

    setsid();
    struct stat st;
    if (stat(p->logfile, &st) == 0 && st.st_size > (8 << 20)) {
        char bak[X68K_PATH_MAX];
        snprintf(bak, sizeof(bak), "%s.1", p->logfile);
        unlink(bak);
        rename(p->logfile, bak);
    }
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, 0);
        if (devnull > 2) close(devnull);
    }
    int logfd = open(p->logfile, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (logfd >= 0) {
        dup2(logfd, 1);
        dup2(logfd, 2);
        if (logfd > 2) close(logfd);
    }
    execl(exe, "x68k", "--home", p->home, "daemon", NULL);
    _exit(127);
}

void app_read_pid(const char *path, long *pid) {
    *pid = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    long v = 0;
    if (fscanf(f, "%ld", &v) == 1) *pid = v;
    fclose(f);
}

bool app_daemon_alive(const app_paths *p) {
    long pid = 0;
    app_read_pid(p->pidfile, &pid);
    if (pid <= 0) return false;
    if (kill((pid_t)pid, 0) != 0 && errno != EPERM) return false;
    return true;
}

int app_connect_or_start(const app_paths *p, int timeout_ms, char *err, size_t errlen) {
    int fd = ipc_connect(p->sock);
    if (fd >= 0) return fd;

    bool alive = app_daemon_alive(p);
    if (!alive) {
        unlink(p->sock);
        unlink(p->pidfile);
        char *exe = app_exe_path();
        app_spawn_daemon(p, exe);
        free(exe);
    }

    int waited = 0;
    while (waited < timeout_ms) {
        usleep(50 * 1000);
        waited += 50;
        fd = ipc_connect(p->sock);
        if (fd >= 0) return fd;
    }

    char *tail = tail_file(p->logfile, 2500);
    snprintf(err, errlen,
             "could not start or reach the x68k daemon at %s%s%s. Try running 'x68k daemon --foreground' "
             "in another terminal, or remove a stale socket.",
             p->sock, tail ? "\nlog tail:\n" : "", tail ? tail : "");
    free(tail);
    return -1;
}
