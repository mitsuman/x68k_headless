#ifndef X68K_VGM_H
#define X68K_VGM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct vgm_recorder vgm_recorder;

typedef struct {
    uint64_t frames;
    uint64_t samples;
    double seconds;
    size_t bytes;
    bool adpcm;
} vgm_stats;

/* Starts a VGM recording.  The core hooks are installed here and removed on
 * stop/abort.  Returns NULL on error. */
vgm_recorder *vgm_start(const char *out_path, bool record_adpcm, char *err, size_t errlen);

/* Called by the emulator around each emulated frame. */
void vgm_begin_frame(vgm_recorder *v, double emu_fps);
void vgm_end_frame(vgm_recorder *v);

/* Called by the px68k core hooks. */
void vgm_hook_opm_write(uint8_t reg, uint8_t data);
void vgm_hook_adpcm_write(uint32_t adr, uint8_t data);
void vgm_hook_adpcm_clock(int mode);

/* Finalises the file (0 = ok).  Frees v. */
int vgm_stop(vgm_recorder *v, vgm_stats *stats, char *err, size_t errlen);
void vgm_abort(vgm_recorder *v);

bool vgm_failed(const vgm_recorder *v, char *err, size_t errlen);
const char *vgm_path(const vgm_recorder *v);
uint64_t vgm_frames(const vgm_recorder *v);
uint64_t vgm_samples(const vgm_recorder *v);
double vgm_seconds(const vgm_recorder *v);
bool vgm_has_adpcm(const vgm_recorder *v);

#endif
