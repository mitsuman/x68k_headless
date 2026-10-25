/*
 * ipc.h - tiny length-prefixed argv protocol over a unix socket
 */
#ifndef X68K_IPC_H
#define X68K_IPC_H

#include <stddef.h>

#define IPC_MAX_ARGS     96
#define IPC_MAX_ARG      (4u << 20)
#define IPC_MAX_RESPONSE (128u << 20)

/* Returns a listening fd or -1 (errno set).  An existing socket file is
 * removed first, so only call this when you are sure no daemon is alive. */
int  ipc_listen(const char *path);
int  ipc_connect(const char *path);
void ipc_close(int fd);
void ipc_set_timeout(int fd, int send_ms, int recv_ms);

int  ipc_write_all(int fd, const void *buf, size_t len);
int  ipc_read_all(int fd, void *buf, size_t len);

int  ipc_send_request(int fd, int argc, char *const *argv);
/* 0 = ok, 1 = clean EOF before a request, -1 = protocol/io error */
int  ipc_recv_request(int fd, int *argc, char ***argv);
void ipc_free_request(int argc, char **argv);

int  ipc_send_response(int fd, const char *data, size_t len);
/* returns malloc'd NUL-terminated JSON, or NULL on error/EOF */
char *ipc_recv_response(int fd, size_t *len);

#endif
