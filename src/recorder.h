#ifndef X68K_RECORDER_H
#define X68K_RECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct recorder recorder;

/* fps is the output video frame rate; every emulated frame becomes one video
 * frame, and its audio is resampled so audio and video stay in sync. */
recorder *recorder_start(const char *out_path, double fps, unsigned width, unsigned height,
                         int audio_rate, bool audio, char *err, size_t errlen);

void recorder_push_frame(recorder *r, const uint16_t *pixels, unsigned w, unsigned h,
                         size_t pitch, const int16_t *audio, size_t audio_frames,
                         double emu_fps);

/* finalises the file.  0 = success, -1 = failure (err filled).  Frees r. */
int recorder_stop(recorder *r, char *err, size_t errlen);

/* aborts and frees r, removing temporary files */
void recorder_abort(recorder *r);

/* true if an asynchronous ffmpeg failure was detected */
bool recorder_failed(const recorder *r, char *err, size_t errlen);

uint64_t    recorder_frames(const recorder *r);
double      recorder_seconds(const recorder *r);
double      recorder_fps(const recorder *r);
unsigned    recorder_width(const recorder *r);
unsigned    recorder_height(const recorder *r);
bool        recorder_has_audio(const recorder *r);
const char *recorder_path(const recorder *r);

#endif
