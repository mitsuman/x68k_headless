#include "recorder.h"
#include "log.h"
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

struct recorder {
    char out_path[X68K_PATH_MAX];
    char video_tmp[X68K_PATH_MAX];
    char wav_tmp[X68K_PATH_MAX];
    char log_path[X68K_PATH_MAX];

    double fps;
    unsigned width, height;
    int audio_rate;
    bool audio;

    FILE *ff;
    FILE *wav;
    pid_t ff_pid;
    uint64_t frames;
    uint64_t audio_samples;
    double video_acc;

    uint8_t *canvas;
    unsigned *xmap;
    unsigned xmap_src_w;

    bool failed;
    char fail_msg[2048];
    int64_t wall_start_ms;
};

static int run_ffmpeg(char *const argv[], const char *log_path, char *err, size_t errlen) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);

    pid_t pid = -1;
    int rc = posix_spawnp(&pid, "ffmpeg", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        snprintf(err, errlen, "could not run ffmpeg: %s (is ffmpeg installed?)", strerror(rc));
        return -1;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            snprintf(err, errlen, "waitpid failed: %s", strerror(errno));
            return -1;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        char *tail = tail_file(log_path, 2000);
        snprintf(err, errlen, "ffmpeg failed%s%s", tail ? ": " : "", tail ? tail : "");
        free(tail);
        return -1;
    }
    return 0;
}

static pid_t spawn_encoder(const char *video_tmp, double fps, unsigned w, unsigned h,
                           const char *log_path, int *pipe_read_end, char *err, size_t errlen) {
    char rate[32], size[32];
    snprintf(rate, sizeof(rate), "%.6f", fps);
    snprintf(size, sizeof(size), "%ux%u", w, h);

    char *argv[] = {
        "ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", size, "-r", rate, "-i", "pipe:0",
        "-an", "-c:v", "libx264", "-preset", "ultrafast", "-crf", "18",
        "-pix_fmt", "yuv420p", "-y", (char *)video_tmp, NULL
    };

    int p[2];
    if (pipe(p) != 0) {
        snprintf(err, errlen, "pipe failed: %s", strerror(errno));
        return -1;
    }
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[0], 0);
    posix_spawn_file_actions_addclose(&fa, p[0]);
    posix_spawn_file_actions_addclose(&fa, p[1]);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);

    pid_t pid = -1;
    int rc = posix_spawnp(&pid, "ffmpeg", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(p[0]);
    if (rc != 0) {
        close(p[1]);
        snprintf(err, errlen, "could not run ffmpeg: %s (is ffmpeg installed?)", strerror(rc));
        return -1;
    }
    *pipe_read_end = p[1];

    /* Catch an immediate encoder failure (missing libx264, bad arguments). */
    usleep(60 * 1000);
    int status = 0;
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
        close(p[1]);
        char *tail = tail_file(log_path, 2000);
        snprintf(err, errlen, "ffmpeg exited during start-up%s%s", tail ? ": " : "", tail ? tail : "");
        free(tail);
        return -1;
    }
    return pid;
}

static void set_fail(recorder *r, const char *fmt, ...) {
    if (r->failed) return;
    r->failed = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->fail_msg, sizeof(r->fail_msg), fmt, ap);
    va_end(ap);
}

static char *make_temp(const char *out_path, const char *suffix) {
    char *dir = path_dirname(out_path);
    char name[128];
    snprintf(name, sizeof(name), ".x68k-rec-%ld%s", (long)getpid(), suffix);
    char *p = path_join(dir, name);
    free(dir);
    return p;
}

recorder *recorder_start(const char *out_path, double fps, unsigned width, unsigned height,
                         int audio_rate, bool audio, char *err, size_t errlen) {
    if (!out_path || !*out_path) {
        snprintf(err, errlen, "no output path for recording");
        return NULL;
    }
    if (fps < 1.0 || fps > 240.0) {
        snprintf(err, errlen, "invalid recording frame rate %.3f", fps);
        return NULL;
    }
    if (width < 16 || height < 16 || (width & 1) || (height & 1)) {
        snprintf(err, errlen, "recording size must be even and at least 16x16");
        return NULL;
    }
    char *dir = path_dirname(out_path);
    if (mkdir_p(dir, 0755) != 0) {
        snprintf(err, errlen, "cannot create recording directory %s: %s", dir, strerror(errno));
        free(dir);
        return NULL;
    }
    free(dir);

    recorder *r = xcalloc(1, sizeof(*r));
    snprintf(r->out_path, sizeof(r->out_path), "%s", out_path);
    snprintf(r->wav_tmp, sizeof(r->wav_tmp), "%s.wav", out_path);
    char *vt = make_temp(out_path, ".video.mp4");
    char *lg = make_temp(out_path, ".ffmpeg.log");
    snprintf(r->video_tmp, sizeof(r->video_tmp), "%s", vt);
    snprintf(r->log_path, sizeof(r->log_path), "%s", lg);
    free(vt);
    free(lg);

    r->fps = fps;
    r->width = width;
    r->height = height;
    r->audio_rate = audio_rate > 0 ? audio_rate : 44100;
    r->audio = audio;
    r->wall_start_ms = now_ms_monotonic();

    unlink(r->video_tmp);
    unlink(r->log_path);

    int pipe_fd = -1;
    r->ff_pid = spawn_encoder(r->video_tmp, fps, width, height, r->log_path, &pipe_fd, err, errlen);
    if (r->ff_pid < 0) {
        unlink(r->video_tmp);
        unlink(r->log_path);
        free(r);
        return NULL;
    }
    r->ff = fdopen(pipe_fd, "wb");
    if (!r->ff) {
        snprintf(err, errlen, "fdopen failed: %s", strerror(errno));
        close(pipe_fd);
        kill(r->ff_pid, SIGTERM);
        waitpid(r->ff_pid, NULL, 0);
        free(r);
        return NULL;
    }
    setvbuf(r->ff, NULL, _IOFBF, 1 << 20);

    if (r->audio) {
        r->wav = fopen(r->wav_tmp, "wb");
        if (!r->wav) {
            snprintf(err, errlen, "cannot create %s: %s", r->wav_tmp, strerror(errno));
            recorder_abort(r);
            return NULL;
        }
        unsigned char hdr[44] = {0};
        memcpy(hdr, "RIFF", 4);
        memcpy(hdr + 8, "WAVEfmt ", 8);
        hdr[16] = 16;
        hdr[20] = 1;  /* PCM */
        hdr[22] = 2;  /* stereo */
        uint32_t rate = (uint32_t)r->audio_rate;
        uint32_t byte_rate = rate * 2 * 2;
        memcpy(hdr + 24, &rate, 4);
        memcpy(hdr + 28, &byte_rate, 4);
        hdr[32] = 4;
        hdr[34] = 16;
        memcpy(hdr + 36, "data", 4);
        fwrite(hdr, 1, sizeof(hdr), r->wav);
    }

    r->canvas = xmalloc((size_t)width * height * 3);
    xlog("recording started: %s (%ux%u @ %.3f fps, audio=%s)", out_path, width, height, fps,
         audio ? "yes" : "no");
    return r;
}

static void convert_frame(recorder *r, const uint16_t *src, unsigned sw, unsigned sh, size_t pitch) {
    size_t out_stride = (size_t)r->width * 3;
    if (!src || !sw || !sh) {
        memset(r->canvas, 0, out_stride * r->height);
        return;
    }
    if (r->xmap_src_w != sw) {
        free(r->xmap);
        r->xmap = xmalloc(sizeof(unsigned) * r->width);
        for (unsigned x = 0; x < r->width; x++) {
            uint64_t sx = (uint64_t)x * sw / r->width;
            if (sx >= sw) sx = sw - 1;
            r->xmap[x] = (unsigned)sx;
        }
        r->xmap_src_w = sw;
    }
    for (unsigned y = 0; y < r->height; y++) {
        uint64_t sy = (uint64_t)y * sh / r->height;
        if (sy >= sh) sy = sh - 1;
        const uint16_t *row = (const uint16_t *)((const uint8_t *)src + (size_t)sy * pitch);
        uint8_t *dst = r->canvas + out_stride * y;
        for (unsigned x = 0; x < r->width; x++) {
            uint16_t p = row[r->xmap[x]];
            unsigned rr = (p >> 11) & 0x1f;
            unsigned gg = (p >> 5) & 0x3f;
            unsigned bb = p & 0x1f;
            dst[0] = (uint8_t)((rr << 3) | (rr >> 2));
            dst[1] = (uint8_t)((gg << 2) | (gg >> 4));
            dst[2] = (uint8_t)((bb << 3) | (bb >> 2));
            dst += 3;
        }
    }
}

static int push_audio(recorder *r, const int16_t *audio, size_t frames) {
    if (!r->audio || frames == 0) return 0;
    size_t written = fwrite(audio, sizeof(int16_t) * 2, frames, r->wav);
    if (written != frames) {
        set_fail(r, "failed to write audio data");
        return -1;
    }
    r->audio_samples += frames;
    return 0;
}

void recorder_push_frame(recorder *r, const uint16_t *pixels, unsigned w, unsigned h,
                         size_t pitch, const int16_t *audio, size_t audio_frames,
                         double emu_fps) {
    if (!r || r->failed) return;

    if (r->audio && push_audio(r, audio, audio_frames) != 0) return;

    /* The recording timeline is emulated time: audio is written at its native
     * 44.1kHz (which already tracks emulated time), and video frames are
     * emitted at the requested output rate by duplicating/dropping emulated
     * frames as needed. */
    double dt = (emu_fps > 1.0) ? 1.0 / emu_fps : (1.0 / 60.0);
    r->video_acc += dt;
    double step = 1.0 / r->fps;
    int emitted = 0;
    while ((r->video_acc >= step || r->frames == 0) && emitted < 8) {
        convert_frame(r, pixels, w, h, pitch);
        size_t bytes = (size_t)r->width * r->height * 3;
        size_t written = fwrite(r->canvas, 1, bytes, r->ff);
        if (written != bytes) {
            char *tail = tail_file(r->log_path, 1500);
            set_fail(r, "ffmpeg stopped accepting video data%s%s", tail ? ": " : "", tail ? tail : "");
            free(tail);
            return;
        }
        r->frames++;
        r->video_acc -= step;
        emitted++;
        if (r->frames == 1) break; /* first frame: no burst */
    }
    if (emitted == 0) {
        /* keep the accumulator from drifting too far behind */
        if (r->video_acc < 0) r->video_acc = 0;
    }
}

bool recorder_failed(const recorder *r, char *err, size_t errlen) {
    if (!r || !r->failed) return false;
    if (err && errlen) snprintf(err, errlen, "%s", r->fail_msg);
    return true;
}

uint64_t recorder_frames(const recorder *r) { return r ? r->frames : 0; }
double recorder_seconds(const recorder *r) { return r ? (double)r->frames / r->fps : 0.0; }
double recorder_fps(const recorder *r) { return r ? r->fps : 0.0; }
unsigned recorder_width(const recorder *r) { return r ? r->width : 0; }
unsigned recorder_height(const recorder *r) { return r ? r->height : 0; }
bool recorder_has_audio(const recorder *r) { return r ? r->audio : false; }
const char *recorder_path(const recorder *r) { return r ? r->out_path : ""; }

static void finish_wav(recorder *r) {
    if (!r->wav) return;
    uint32_t data_bytes = (uint32_t)(r->audio_samples * 2 * 2);
    uint32_t riff_size = 36 + data_bytes;
    fseek(r->wav, 4, SEEK_SET);
    fwrite(&riff_size, 4, 1, r->wav);
    fseek(r->wav, 40, SEEK_SET);
    fwrite(&data_bytes, 4, 1, r->wav);
    fclose(r->wav);
    r->wav = NULL;
}

static int close_encoder(recorder *r, char *err, size_t errlen) {
    if (r->ff) {
        if (fclose(r->ff) != 0) {
            /* EPIPE here just means ffmpeg already died */
        }
        r->ff = NULL;
    }
    if (r->ff_pid > 0) {
        int status = 0;
        while (waitpid(r->ff_pid, &status, 0) < 0) {
            if (errno != EINTR) break;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            char *tail = tail_file(r->log_path, 2000);
            snprintf(err, errlen, "ffmpeg video encoder failed%s%s", tail ? ": " : "", tail ? tail : "");
            free(tail);
            return -1;
        }
    }
    return 0;
}

int recorder_stop(recorder *r, char *err, size_t errlen) {
    if (!r) return -1;
    int rc = 0;
    char why[2048] = {0};
    if (r->failed) {
        snprintf(why, sizeof(why), "%s", r->fail_msg);
        rc = -1;
    } else if (r->frames == 0) {
        snprintf(why, sizeof(why), "no emulated frames were recorded (run 'step' while recording)");
        rc = -1;
        /* nothing useful was captured: remove every trace */
        if (r->ff) { fclose(r->ff); r->ff = NULL; }
        if (r->ff_pid > 0) {
            kill(r->ff_pid, SIGTERM);
            while (waitpid(r->ff_pid, NULL, 0) < 0 && errno == EINTR) {}
            r->ff_pid = -1;
        }
        if (r->wav) { fclose(r->wav); r->wav = NULL; }
        unlink(r->video_tmp);
        unlink(r->wav_tmp);
        unlink(r->log_path);
        unlink(r->out_path);
        snprintf(err, errlen, "%s", why);
        free(r->canvas);
        free(r->xmap);
        free(r);
        return -1;
    }

    if (close_encoder(r, err, errlen) != 0) {
        rc = -1;
        if (!why[0]) snprintf(why, sizeof(why), "%s", err);
    }
    finish_wav(r);

    if (rc != 0) {
        snprintf(err, errlen, "%s", why);
        /* keep the partial files, they can help diagnose the problem */
        xlog("recording failed: %s (partial video: %s)", why, r->video_tmp);
        free(r->canvas);
        free(r->xmap);
        free(r);
        return -1;
    }

    char out[X68K_PATH_MAX];
    snprintf(out, sizeof(out), "%s", r->out_path);
    if (r->audio && r->audio_samples > 0) {
        char *argv[] = {
            "ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
            "-i", r->video_tmp, "-i", r->wav_tmp,
            "-c:v", "copy", "-c:a", "aac", "-b:a", "160k", "-shortest",
            "-movflags", "+faststart", out, NULL
        };
        if (run_ffmpeg(argv, r->log_path, err, errlen) != 0) {
            xlog("recording mux failed: %s", err);
            free(r->canvas);
            free(r->xmap);
            free(r);
            return -1;
        }
        unlink(r->video_tmp);
        unlink(r->wav_tmp);
    } else {
        if (rename(r->video_tmp, out) != 0) {
            snprintf(err, errlen, "rename failed: %s", strerror(errno));
            free(r->canvas);
            free(r->xmap);
            free(r);
            return -1;
        }
        unlink(r->wav_tmp);
    }
    unlink(r->log_path);

    struct stat st;
    xlog("recording finished: %s (%llu frames, %.2f s, %.1f KB)", out,
         (unsigned long long)r->frames, recorder_seconds(r),
         stat(out, &st) == 0 ? (double)st.st_size / 1024.0 : 0.0);

    free(r->canvas);
    free(r->xmap);
    free(r);
    return 0;
}

void recorder_abort(recorder *r) {
    if (!r) return;
    if (r->ff) {
        fclose(r->ff);
        r->ff = NULL;
    }
    if (r->ff_pid > 0) {
        kill(r->ff_pid, SIGTERM);
        while (waitpid(r->ff_pid, NULL, 0) < 0 && errno == EINTR) {
        }
        r->ff_pid = -1;
    }
    if (r->wav) {
        fclose(r->wav);
        r->wav = NULL;
    }
    unlink(r->video_tmp);
    unlink(r->wav_tmp);
    unlink(r->log_path);
    free(r->canvas);
    free(r->xmap);
    free(r);
}
