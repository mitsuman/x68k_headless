/*
 * vgm.c - VGM sound logger
 *
 * Records YM2151 (OPM) register writes and, optionally, MSM6258 ADPCM data
 * from the px68k core.  The OPM register writes are timestamped with the
 * current scanline, giving roughly 1-2 VGM samples of timing resolution.
 * ADPCM data is queued and emitted at the chip's sample rate so that VGM
 * players with a small MSM6258 FIFO receive it at a playable pace.
 */
#include "vgm.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "util.h"

/* px68k globals / hooks (the core is patched to call these callbacks) */
extern uint32_t x68k_headless_vline(void);
extern uint16_t x68k_headless_vline_total(void);
extern void (*x68k_opm_write_hook)(uint8_t reg, uint8_t data);
extern void (*x68k_adpcm_write_hook)(uint32_t adr, uint8_t data);
extern void (*x68k_adpcm_clock_hook)(int mode);
extern int x68k_adpcm_clock_mode(void);
extern double x68k_adpcm_sample_rate(void);
extern void OPM_GetRegs(uint8_t *out);

#define VGM_HEADER_SIZE 0x100
#define VGM_VERSION 0x161
#define OPM_CLOCK 4000000u
#define MSM6258_CLOCK 8000000u
#define ADPCM_QUEUE_LIMIT (4u << 20)

typedef struct {
    uint8_t data;
    double due;
} pending_byte;

struct vgm_recorder {
    FILE *f;
    char path[X68K_PATH_MAX];
    bool failed;
    char fail_msg[1024];
    bool adpcm;

    uint64_t frames;
    uint64_t emitted;      /* integer VGM samples written as waits */

    double time;           /* end of the last completed frame */
    double frame_base;     /* start of the frame currently running */
    double frame_samples;  /* duration of the current frame */
    bool frame_active;

    double adpcm_dt;       /* VGM samples per ADPCM byte */
    double next_adpcm_time;

    pending_byte *pending;
    size_t head, tail, cap;
};

static vgm_recorder *g_vgm;

static void set_fail(vgm_recorder *v, const char *fmt, ...) {
    if (!v || v->failed) return;
    v->failed = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v->fail_msg, sizeof(v->fail_msg), fmt, ap);
    va_end(ap);
    xlog("VGM: %s", v->fail_msg);
}

static void put_bytes(vgm_recorder *v, const void *data, size_t len) {
    if (!v || v->failed) return;
    if (len && fwrite(data, 1, len, v->f) != len)
        set_fail(v, "failed to write %s: %s", v->path, strerror(errno));
}

static void put_cmd(vgm_recorder *v, uint8_t cmd) {
    put_bytes(v, &cmd, 1);
}

static void put_cmd3(vgm_recorder *v, uint8_t cmd, uint8_t a, uint8_t b) {
    uint8_t d[3] = {cmd, a, b};
    put_bytes(v, d, sizeof(d));
}

static void emit_wait_to(vgm_recorder *v, double t) {
    if (!v || v->failed) return;
    double r = floor(t + 0.5);
    if (r < 0) r = 0;
    uint64_t target = (uint64_t)r;
    if (target < v->emitted) target = v->emitted;
    uint64_t n = target - v->emitted;

    while (n) {
        if (n > 65535) {
            uint8_t b[3] = {0x61, 0xff, 0xff};
            put_bytes(v, b, sizeof(b));
            n -= 65535;
        } else if (n == 735) {
            put_cmd(v, 0x62);
            n = 0;
        } else if (n == 882) {
            put_cmd(v, 0x63);
            n = 0;
        } else if (n <= 16) {
            put_cmd(v, (uint8_t)(0x70 + n - 1));
            n = 0;
        } else {
            uint8_t b[3] = {0x61, (uint8_t)(n & 0xff), (uint8_t)((n >> 8) & 0xff)};
            put_bytes(v, b, sizeof(b));
            n = 0;
        }
        if (v->failed) return;
    }
    v->emitted = target;
}

static void write_opm(vgm_recorder *v, uint8_t reg, uint8_t data) {
    put_cmd3(v, 0x54, reg, data);
}

static void write_okim(vgm_recorder *v, uint8_t reg, uint8_t data) {
    put_cmd3(v, 0xb7, reg, data);
}

static void emit_adpcm_clock(vgm_recorder *v, int mode) {
    if (!v->adpcm) return;
    /* The X68000 selects one of eight ADPCM rates.  Modes 0-3 use an 8 MHz
     * MSM6258 clock; modes 4-7 use 4 MHz.  The low two bits select the
     * divider (the core's mode 3 equals mode 1).  The clock-buffer registers
     * (0x08..0x0B) and divider register (0x0C) are supported by
     * libvgm/vgmplay. */
    int slow = mode & 4;
    uint32_t clock = slow ? 4000000u : MSM6258_CLOCK;
    int div = mode & 3;
    if (div == 3) div = 1;
    write_okim(v, 0x08, (uint8_t)(clock & 0xff));
    write_okim(v, 0x09, (uint8_t)((clock >> 8) & 0xff));
    write_okim(v, 0x0a, (uint8_t)((clock >> 16) & 0xff));
    write_okim(v, 0x0b, (uint8_t)((clock >> 24) & 0xff));
    write_okim(v, 0x0c, (uint8_t)div);
}

static double event_time(vgm_recorder *v) {
    if (!v->frame_active) return v->time;
    uint32_t total = x68k_headless_vline_total();
    if (!total) total = 1;
    uint32_t line = x68k_headless_vline();
    if (line >= total) line = total - 1;
    return v->frame_base + v->frame_samples * (double)line / (double)total;
}

static void update_adpcm_rate(vgm_recorder *v) {
    double rate = x68k_adpcm_sample_rate();
    if (!(rate > 100.0) || rate > 1000000.0) rate = 15625.0;
    v->adpcm_dt = 2.0 * 44100.0 / rate;
}

static void compact_queue(vgm_recorder *v) {
    if (v->head == 0) return;
    if (v->head > 4096 && v->head > (v->tail - v->head)) {
        size_t n = v->tail - v->head;
        if (n) memmove(v->pending, v->pending + v->head, n * sizeof(*v->pending));
        v->tail = n;
        v->head = 0;
    }
}

static void flush_due(vgm_recorder *v, double t) {
    while (v->head < v->tail) {
        pending_byte *pb = &v->pending[v->head];
        if (pb->due > t + 1e-9) break;
        emit_wait_to(v, pb->due);
        write_okim(v, 0x01, pb->data);
        v->head++;
        if (v->failed) break;
    }
    compact_queue(v);
}

static void queue_adpcm(vgm_recorder *v, uint8_t data, double now) {
    if (v->tail >= v->cap) {
        if (v->cap >= ADPCM_QUEUE_LIMIT) {
            set_fail(v, "ADPCM queue overflow");
            return;
        }
        size_t cap = v->cap ? v->cap * 2 : 4096;
        if (cap > ADPCM_QUEUE_LIMIT) cap = ADPCM_QUEUE_LIMIT;
        v->pending = xrealloc(v->pending, cap * sizeof(*v->pending));
        v->cap = cap;
    }
    double due = v->next_adpcm_time;
    if (due < now) due = now;
    v->pending[v->tail].data = data;
    v->pending[v->tail].due = due;
    v->tail++;
    v->next_adpcm_time = due + v->adpcm_dt;
}

/* ------------------------------------------------------------------ */
/* core hooks                                                          */
/* ------------------------------------------------------------------ */

void vgm_hook_opm_write(uint8_t reg, uint8_t data) {
    vgm_recorder *v = g_vgm;
    if (!v || v->failed) return;
    double t = event_time(v);
    flush_due(v, t);
    emit_wait_to(v, t);
    write_opm(v, reg, data);
}

void vgm_hook_adpcm_write(uint32_t adr, uint8_t data) {
    vgm_recorder *v = g_vgm;
    if (!v || v->failed || !v->adpcm) return;
    double t = event_time(v);
    if (adr == 0xe92001) {
        flush_due(v, t);
        emit_wait_to(v, t);
        write_okim(v, 0x00, data);
        if (data & 3) {
            /* start or stop: drop data that is still queued for the future */
            v->head = v->tail = 0;
            v->next_adpcm_time = t;
        }
    } else if (adr == 0xe92003) {
        queue_adpcm(v, data, t);
    }
}

void vgm_hook_adpcm_clock(int mode) {
    vgm_recorder *v = g_vgm;
    if (!v || v->failed || !v->adpcm) return;
    double t = event_time(v);
    flush_due(v, t);
    emit_wait_to(v, t);
    emit_adpcm_clock(v, mode);
    update_adpcm_rate(v);
    if (v->next_adpcm_time < t) v->next_adpcm_time = t;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

vgm_recorder *vgm_start(const char *out_path, bool record_adpcm, char *err, size_t errlen) {
    if (g_vgm) {
        snprintf(err, errlen, "a VGM recording is already active");
        return NULL;
    }
    if (!out_path || !*out_path) {
        snprintf(err, errlen, "no output path for the VGM recording");
        return NULL;
    }
    char *dir = path_dirname(out_path);
    if (mkdir_p(dir, 0755) != 0) {
        snprintf(err, errlen, "cannot create the VGM directory %s: %s", dir, strerror(errno));
        free(dir);
        return NULL;
    }
    free(dir);
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        snprintf(err, errlen, "cannot create %s: %s", out_path, strerror(errno));
        return NULL;
    }
    uint8_t zero[VGM_HEADER_SIZE];
    memset(zero, 0, sizeof(zero));
    if (fwrite(zero, 1, sizeof(zero), f) != sizeof(zero)) {
        snprintf(err, errlen, "cannot write the VGM header to %s", out_path);
        fclose(f);
        unlink(out_path);
        return NULL;
    }

    vgm_recorder *v = xcalloc(1, sizeof(*v));
    v->f = f;
    snprintf(v->path, sizeof(v->path), "%s", out_path);
    v->adpcm = record_adpcm;
    update_adpcm_rate(v);

    g_vgm = v;
    x68k_opm_write_hook = vgm_hook_opm_write;
    x68k_adpcm_write_hook = vgm_hook_adpcm_write;
    x68k_adpcm_clock_hook = vgm_hook_adpcm_clock;

    /* Initial chip state at time 0 so that recording can start in the middle
     * of a song: the OPM shadow contains every register the guest has written
     * since boot. */
    emit_wait_to(v, 0.0);
    if (v->adpcm) emit_adpcm_clock(v, x68k_adpcm_clock_mode());
    uint8_t regs[256];
    OPM_GetRegs(regs);
    /* Set up everything first, then trigger any notes that are already
     * sounding (0x08-0x0F are the key-on registers). */
    for (int i = 0; i < 256; i++) {
        if (i >= 0x08 && i <= 0x0f) continue;
        write_opm(v, (uint8_t)i, regs[i]);
    }
    for (int i = 0x08; i <= 0x0f; i++) write_opm(v, (uint8_t)i, regs[i]);
    if (v->failed) {
        snprintf(err, errlen, "%s", v->fail_msg);
        vgm_abort(v);
        return NULL;
    }

    xlog("VGM recording started: %s (YM2151%s)", out_path, record_adpcm ? " + MSM6258" : " only");
    return v;
}

void vgm_begin_frame(vgm_recorder *v, double emu_fps) {
    if (!v || v->failed) return;
    v->frame_base = v->time;
    v->frame_samples = 44100.0 / (emu_fps > 1.0 ? emu_fps : 60.0);
    v->frame_active = true;
    v->frames++;
    update_adpcm_rate(v);
}

void vgm_end_frame(vgm_recorder *v) {
    if (!v || v->failed) return;
    double end = v->frame_base + v->frame_samples;
    flush_due(v, end);
    v->time = end;
    v->frame_active = false;
}

static void clear_hooks(vgm_recorder *v) {
    if (g_vgm == v) {
        x68k_opm_write_hook = NULL;
        x68k_adpcm_write_hook = NULL;
        x68k_adpcm_clock_hook = NULL;
        g_vgm = NULL;
    }
}

int vgm_stop(vgm_recorder *v, vgm_stats *stats, char *err, size_t errlen) {
    if (!v) return -1;
    clear_hooks(v);

    if (v->failed) {
        snprintf(err, errlen, "%s", v->fail_msg);
        if (v->f) fclose(v->f);
        unlink(v->path);
        free(v->pending);
        free(v);
        return -1;
    }
    if (v->frames == 0) {
        snprintf(err, errlen, "no emulated frames were recorded (run 'step' while recording)");
        if (v->f) fclose(v->f);
        unlink(v->path);
        free(v->pending);
        free(v);
        return -1;
    }

    /* Emit queued data only up to the recorded end time; anything scheduled
     * beyond the recording is dropped. */
    flush_due(v, v->time);
    emit_wait_to(v, v->time);
    put_cmd(v, 0x66);
    if (v->failed) {
        snprintf(err, errlen, "%s", v->fail_msg);
        if (v->f) fclose(v->f);
        unlink(v->path);
        free(v->pending);
        free(v);
        return -1;
    }

    long end = ftell(v->f);
    uint8_t h[VGM_HEADER_SIZE];
    memset(h, 0, sizeof(h));
    memcpy(h, "Vgm ", 4);
    uint32_t eof = (uint32_t)(end - 4);
    memcpy(h + 0x04, &eof, 4);
    uint32_t version = VGM_VERSION;
    memcpy(h + 0x08, &version, 4);
    uint32_t samples = (uint32_t)v->emitted;
    memcpy(h + 0x18, &samples, 4);
    uint32_t opm = OPM_CLOCK;
    memcpy(h + 0x30, &opm, 4);
    uint32_t data_ofs = VGM_HEADER_SIZE - 0x34;
    memcpy(h + 0x34, &data_ofs, 4);
    uint32_t msm = v->adpcm ? MSM6258_CLOCK : 0;
    memcpy(h + 0x90, &msm, 4);
    h[0x94] = v->adpcm ? 0x06 : 0x00; /* bit2: 4-bit ADPCM, bits0-1: divider 512 */

    int rc = 0;
    if (fseek(v->f, 0, SEEK_SET) != 0 || fwrite(h, 1, sizeof(h), v->f) != sizeof(h))
        rc = -1;
    if (fclose(v->f) != 0) rc = -1;
    if (rc != 0) {
        snprintf(err, errlen, "failed to finalise %s", v->path);
        unlink(v->path);
        free(v->pending);
        free(v);
        return -1;
    }

    if (stats) {
        stats->frames = v->frames;
        stats->samples = v->emitted;
        stats->seconds = (double)v->emitted / 44100.0;
        stats->bytes = (size_t)end;
        stats->adpcm = v->adpcm;
    }
    xlog("VGM recording finished: %s (%llu samples, %.2f s, %zu bytes)", v->path,
         (unsigned long long)v->emitted, (double)v->emitted / 44100.0, (size_t)end);
    free(v->pending);
    free(v);
    return 0;
}

void vgm_abort(vgm_recorder *v) {
    if (!v) return;
    clear_hooks(v);
    if (v->f) fclose(v->f);
    unlink(v->path);
    free(v->pending);
    free(v);
}

bool vgm_failed(const vgm_recorder *v, char *err, size_t errlen) {
    if (!v || !v->failed) return false;
    if (err && errlen) snprintf(err, errlen, "%s", v->fail_msg);
    return true;
}

const char *vgm_path(const vgm_recorder *v) { return v ? v->path : ""; }
uint64_t vgm_frames(const vgm_recorder *v) { return v ? v->frames : 0; }
uint64_t vgm_samples(const vgm_recorder *v) { return v ? v->emitted : 0; }
double vgm_seconds(const vgm_recorder *v) { return v ? (double)v->emitted / 44100.0 : 0.0; }
bool vgm_has_adpcm(const vgm_recorder *v) { return v ? v->adpcm : false; }
