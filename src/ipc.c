#include "ipc.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define IPC_MAGIC 0x5836384bU /* "X68K" */

int ipc_write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

int ipc_read_all(int fd, void *buf, size_t len) {
    char *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_u32(int fd, uint32_t v) {
    uint32_t n = htonl(v);
    return ipc_write_all(fd, &n, sizeof(n));
}

static int read_u32(int fd, uint32_t *out) {
    uint32_t n;
    if (ipc_read_all(fd, &n, sizeof(n)) != 0) return -1;
    *out = ntohl(n);
    return 0;
}

int ipc_listen(const char *path) {
    struct sockaddr_un addr;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    unlink(path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    chmod(path, 0600);
    if (listen(fd, 8) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int ipc_connect(const char *path) {
    struct sockaddr_un addr;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

void ipc_close(int fd) {
    if (fd >= 0) close(fd);
}

void ipc_set_timeout(int fd, int send_ms, int recv_ms) {
    struct timeval tv;
    if (send_ms >= 0) {
        tv.tv_sec = send_ms / 1000;
        tv.tv_usec = (send_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    if (recv_ms >= 0) {
        tv.tv_sec = recv_ms / 1000;
        tv.tv_usec = (recv_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
}

int ipc_send_request(int fd, int argc, char *const *argv) {
    if (argc < 0 || argc > IPC_MAX_ARGS) return -1;
    if (write_u32(fd, IPC_MAGIC) != 0) return -1;
    if (write_u32(fd, (uint32_t)argc) != 0) return -1;
    for (int i = 0; i < argc; i++) {
        size_t len = argv[i] ? strlen(argv[i]) : 0;
        if (len > IPC_MAX_ARG) return -1;
        if (write_u32(fd, (uint32_t)len) != 0) return -1;
        if (len && ipc_write_all(fd, argv[i], len) != 0) return -1;
    }
    return 0;
}

int ipc_recv_request(int fd, int *argc_out, char ***argv_out) {
    uint32_t magic;
    ssize_t n;
    do {
        n = read(fd, &magic, sizeof(magic));
    } while (n < 0 && errno == EINTR);
    if (n == 0) return 1; /* clean EOF */
    if (n < 0) return -1;
    if ((size_t)n != sizeof(magic)) {
        /* try to complete the header */
        char *p = (char *)&magic + n;
        size_t left = sizeof(magic) - (size_t)n;
        while (left) {
            ssize_t k = read(fd, p, left);
            if (k <= 0) return -1;
            p += k;
            left -= (size_t)k;
        }
    }
    if (ntohl(magic) != IPC_MAGIC) return -1;

    uint32_t argc;
    if (read_u32(fd, &argc) != 0) return -1;
    if (argc > IPC_MAX_ARGS) return -1;

    char **argv = xcalloc(argc ? argc : 1, sizeof(char *));
    for (uint32_t i = 0; i < argc; i++) {
        uint32_t len;
        if (read_u32(fd, &len) != 0) goto fail;
        if (len > IPC_MAX_ARG) goto fail;
        char *s = xmalloc((size_t)len + 1);
        if (len && ipc_read_all(fd, s, len) != 0) {
            free(s);
            goto fail;
        }
        s[len] = '\0';
        argv[i] = s;
    }
    *argc_out = (int)argc;
    *argv_out = argv;
    return 0;

fail:
    for (uint32_t i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    return -1;
}

void ipc_free_request(int argc, char **argv) {
    if (!argv) return;
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);
}

int ipc_send_response(int fd, const char *data, size_t len) {
    if (len > IPC_MAX_RESPONSE) return -1;
    if (write_u32(fd, IPC_MAGIC) != 0) return -1;
    if (write_u32(fd, (uint32_t)len) != 0) return -1;
    if (len && ipc_write_all(fd, data, len) != 0) return -1;
    return 0;
}

char *ipc_recv_response(int fd, size_t *len_out) {
    uint32_t magic;
    if (read_u32(fd, &magic) != 0) return NULL;
    if (magic != IPC_MAGIC) return NULL;
    uint32_t len;
    if (read_u32(fd, &len) != 0) return NULL;
    if (len > IPC_MAX_RESPONSE) return NULL;
    char *buf = xmalloc((size_t)len + 1);
    if (len && ipc_read_all(fd, buf, len) != 0) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    if (len_out) *len_out = len;
    return buf;
}
