# Local changes for x68k_headless

This vendored px68k-libretro tree contains a few additions used by the
x68k_headless VGM logger.  They are inert unless a VGM recording is active and
do not change emulation behaviour.

## `fmgen/fmg_wrap.cpp`, `fmgen/fmg_wrap.h`

* `void (*x68k_opm_write_hook)(uint8_t reg, uint8_t data)` is called from
  `MyOPM::WriteIO()` for every write to the OPM data port.
* `MyOPM::Shadow[256]` keeps a copy of the OPM register file.
  `OPM_GetRegs()` dumps it so a VGM can begin in the middle of a song.  The
  shadow is cleared by `OPM_Reset()` and starts at the FM::OPM reset defaults.
* `OPM_GetRegs()` is declared in `fmg_wrap.h`.

## `x68k/adpcm.c`

* `void (*x68k_adpcm_write_hook)(uint32_t adr, uint8_t data)` is called from
  `ADPCM_Write()` for the control register (0xE92001) and for data bytes that
  are accepted while the chip is playing (0xE92003).
* `void (*x68k_adpcm_clock_hook)(int mode)` is called when
  `ADPCM_SetPan()` / `ADPCM_SetClock()` change the ADPCM clock mode.
* `int x68k_adpcm_clock_mode(void)` and `double x68k_adpcm_sample_rate(void)`
  expose the current mode and sample rate.

## `libretro.c`

* `uint32_t x68k_headless_vline(void)` and
  `uint16_t x68k_headless_vline_total(void)` expose the current scanline and
  frame line count for the VGM timing.  (`vline` is renamed to `HOGEvline` by
  `libretro/winx68k.h`, so the accessors avoid relying on the macro.)

No other source changes are made.
