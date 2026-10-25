#ifndef X68K_CLIENT_H
#define X68K_CLIENT_H
#include <stdbool.h>

int  client_run(const char *home, int argc, char **argv, bool pretty, int timeout_ms);
int  client_batch(const char *home, const char *file, bool pretty, int timeout_ms, bool keep_going);

char *json_pretty_text(const char *json);

#endif
