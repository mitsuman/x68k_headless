#ifndef X68K_APP_H
#define X68K_APP_H

#include <stdbool.h>
#include <stddef.h>

#include "util.h"

typedef struct {
    char home[X68K_PATH_MAX];
    char sock[X68K_PATH_MAX];
    char pidfile[X68K_PATH_MAX];
    char lockfile[X68K_PATH_MAX];
    char logfile[X68K_PATH_MAX];
    char screens[X68K_PATH_MAX];
    char recordings[X68K_PATH_MAX];
    char states[X68K_PATH_MAX];
    char roms[X68K_PATH_MAX];
} app_paths;

/* override may be NULL; otherwise $X68K_HOME or ~/.x68k */
void app_paths_init(app_paths *p, const char *override);
int  app_make_dirs(const app_paths *p, char *err, size_t errlen);

char *app_exe_path(void);
void  app_spawn_daemon(const app_paths *p, const char *exe);
int   app_connect_or_start(const app_paths *p, int timeout_ms, char *err, size_t errlen);
bool  app_daemon_alive(const app_paths *p);
void  app_read_pid(const char *path, long *pid);

#endif
