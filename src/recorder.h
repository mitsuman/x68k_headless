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

/* Size of the most recent emulated frame.  The output size is fixed when the
 * recording starts, so this differs from width/height once the X68000 changes
 * video mode: frames are then rescaled into the original output size. */
unsigned    recorder_source_width(const recorder *r);
unsigned    recorder_source_height(const recorder *r);

/* Number of mid-recording screen-resolution changes, and a counter that
 * increments per change so a caller can report each one exactly once. */
uint64_t    recorder_resolution_changes(const recorder *r);
uint64_t    recorder_resolution_seq(const recorder *r);

/* Details of the most recent change.  false when nothing changed yet. */
bool        recorder_last_resolution_change(const recorder *r, unsigned *from_w,
                                            unsigned *from_h, unsigned *to_w,
                                            unsigned *to_h);

#endif
