# x68k — headless X68000 emulator for scripted / LLM play

`x68k` runs a Sharp X68000 with **no window, no real-time pacing and no GUI**. It
is a small CLI/daemon built around the [px68k libretro
core](https://github.com/libretro/px68k-libretro): you boot a machine, advance
emulated time in explicit chunks, take PNG screenshots, record H.264/AAC video,
and send keyboard / mouse / joystick input. Every command prints one JSON
object, so an LLM (or a shell script) can drive a game deterministically.

There is no background emulation loop. Time only advances when you ask for it
(`step`, or the convenience input commands such as `key tap` and `joy hold`).
A 2-second step is typically 20–100× faster than real time.

---

## 日本語クイックスタート

X68000 の ROM（`iplrom.dat` と `cgrom.dat`）は著作権の都合で同梱していません。
`--roms` で ROM のあるディレクトリを指定してください。

```sh
# ビルド（macOS / Linux。CMake, C++ コンパイラ, zlib が必要）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 起動（デーモンは自動で立ち上がります）
./build/x68k boot "game.dim" --roms ~/Documents/MPX68K

# 12 秒進めて PNG を保存（JSON の data.snapshot.path を見る）
./build/x68k step 12s --snap

# 入力
./build/x68k key tap return
./build/x68k joy hold 1s right --snap

# 画面と音を録画
./build/x68k record start
./build/x68k step 5s
./build/x68k record stop

./build/x68k status
./build/x68k quit
```

詳しいコマンドは `./build/x68k help`、キー名は `./build/x68k help keys`、
LLM 向けの運用ガイドは [docs/LLM_GUIDE.md](docs/LLM_GUIDE.md) を参照してください。

---

## Features

* Boot a machine from 0–2 floppy images (`dim`, `d88`, `hdm`, `2hd`, `xdf`,
  `dup`, `img`) and/or one hard-disk image (`hdf`).
* Advance exactly N frames or a wall-clock-independent duration (`1.5s`,
  `500ms`, `2m`, `90f`).
* Dump the current screen as a PNG at native resolution (optional integer
  upscaling).
* Start/stop recording of video + stereo audio to MP4 (H.264 + AAC).
* VGM sound logs of the YM2151 (OPM) and MSM6258 ADPCM, for chiptune players.
* Keyboard (including chords and text typing), relative mouse, and X68000
  joystick input; `tap`, `hold`, `down`/`up` variants.
* Hot floppy swap with write-back to the image file.
* Compressed save states (branching / retry), machine reset, SRAM persistence,
  configurable RAM / CPU speed / refresh-rate mode / joystick type.
* Persistent background daemon with automatic start, one session per `--home`.
* Batch mode, `doctor` diagnostics, machine-readable errors.

## Requirements

* macOS (arm64/x86_64) or Linux; a C11/C++17 compiler and CMake ≥ 3.20.
* zlib (a system library on both platforms).
* `ffmpeg` and `ffprobe` on `PATH` **only for MP4 recording**. Everything else
  works without them.
* Optional: a VGM player or converter such as `vgmplay` / `vgm2wav` (libvgm)
  for the VGM output.
* An X68000 ROM set (`iplrom.dat`, `cgrom.dat`) and disk images. **They are not
  included** (see [NOTICE](NOTICE)).

The vendored px68k core is GPLv2; this project is therefore GPLv2 as well.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/x68k version
```

or simply `make` (a thin CMake wrapper). Install with `cmake --install build`
(installs `bin/x68k`).

## Quick start

```sh
# Where are my ROMs / is ffmpeg available?
./build/x68k doctor

# Boot with the ROMs and a two-disk game.
# ROMs may also come from $X68K_ROM_DIR, or be auto-detected in
# ~/Documents/MPX68K, ~/x68k, ~/x68k/roms or ~/.x68k/roms.
./build/x68k boot "Disk A.dim" "Disk B.dim" --roms ~/Documents/MPX68K

# Wait for the loader, then save a screenshot.
./build/x68k step 12s --snap
# -> {"ok":true,...,"data":{"snapshot":{"path":"/Users/me/.x68k/screens/f00000667.png",...}}}

# Look at the PNG, then interact.
./build/x68k key tap return
./build/x68k joy hold 1s right --snap
./build/x68k mouse move 30 -10
./build/x68k step 0.2s --snap

# Record a few seconds of emulated time.
./build/x68k record start
./build/x68k step 5s
./build/x68k record stop
# -> ...recording.path = /Users/me/.x68k/recordings/rec-20261025-182757.mp4

./build/x68k quit
```

Every invocation is independent; the daemon keeps the machine alive between
calls. Relative paths in the client are resolved against the **client's**
current directory.

## How it works

```
 x68k CLI ──Unix socket──> x68kd (same binary, "daemon" mode)
                            └─ px68k core (statically linked)
                                ├─ RGB565 framebuffer → PNG / ffmpeg
                                ├─ 44.1 kHz stereo audio → WAV → ffmpeg
                                └─ keyboard / mouse / joypad callbacks
```

* The first command (except `help`, `version`, `doctor`, `daemon`) auto-starts
  a daemon for the selected home directory and connects to it.
* The daemon processes one request at a time, synchronously. A long `step`
  blocks other commands; that is intentional and keeps state deterministic.
* Emulation only advances inside `step` and inside input conveniences that
  hold something for a duration (`key tap`, `key hold`, `key type`, `joy tap`,
  `joy hold`, `mouse click`).
* `x68k quit` finalizes any recording, flushes floppy write-back and SRAM, and
  stops the daemon.

## Commands

Global options go **before** the command:

```sh
x68k [--home DIR] [--pretty] [--timeout SEC] <command> [args...]
```

| Option | Meaning |
|---|---|
| `--home DIR` | Session directory (default `$X68K_HOME` or `~/.x68k`). |
| `--pretty` | Indent JSON output (may also be written after the command). |
| `--timeout SEC` | Client wait limit for one command (default 600). |

### `boot`

```sh
x68k boot [IMAGE ...] [--fdd0 PATH] [--fdd1 PATH] [--hdd PATH]
          [--roms DIR] [--ram 1..12] [--cpu 10|16|25|33|66|100]
          [--fps actual|compat] [--joy1 TYPE] [--joy2 TYPE]
```

* 0–2 positional images go into FDD0/FDD1. `--hdd` boots an HDF (the core only
  treats `.hdf` as a hard disk).
* ROMs are searched in `--roms`, `$X68K_ROM_DIR`, `~/.x68k/roms/keropi`,
  `~/.x68k/roms`, `~/Documents/MPX68K`, `~/x68k`, and `~/x68k/roms`
  (case-insensitive filenames). `x68k doctor` additionally checks the client's
  current directory. They are copied into `<home>/roms/keropi/`.
* Defaults: 4 MB RAM, 10 MHz, actual refresh rates (`--fps actual`), 2-button
  pads, mouse enabled.
* RAM/CPU/refresh mode are fixed for the lifetime of the boot; boot again to
  change them.
* Booting while a machine is already running replaces it (SRAM and floppy
  changes are flushed first).

### `step`

```sh
x68k step 1.5s [--snap] [--out PATH] [--scale N]
x68k step --frames 120 [--snap ...]
```

Advances emulation as fast as possible. Durations are converted with the
current refresh rate (55.46 or 61.46 Hz; `compat` uses 55.5/59.94), so the
number of frames is rounded up to a frame boundary. The response includes the
exact `frames`, `emu_seconds`, `wall_ms` and `speed_x` (emulated seconds per
wall second). A single step is limited to 1 hour of emulated time (or 300,000
frames) to protect the daemon from runaway requests; issue several steps for
long waits.

`--snap` captures the last rendered frame. `--out` implies `--snap`. The
default path is `<home>/screens/f<frame>.png` (adds `@2x` for scale > 1).

### `snap`

```sh
x68k snap [--out PATH] [--scale 1..8]
```

Writes the current screen to a PNG. Native X68000 resolution is used unless
`--scale` is given (nearest-neighbour). The parent directory is created.
`--out -` is not supported (the JSON response always contains a path).

### `record`

```sh
x68k record start [--out PATH] [--fps N] [--size WxH] [--no-audio]
x68k record stop
x68k record status
```

* Defaults: 640×480, the current refresh rate as the output frame rate,
  stereo audio. Codec: H.264 (`libx264`, ultrafast, CRF 18) + AAC 160 kbps.
* The recording timeline is **emulated time**. Audio is written at its native
  44.1 kHz; video frames are duplicated or dropped if `--fps` differs from the
  emulated refresh rate. `--fps 30` therefore produces a real-time 30 fps file,
  not a slow-motion one.
* Only time advanced while recording is captured. Wall-clock pauses between
  commands do not appear in the file.
* `record stop` finalizes and muxes the MP4. If no frames were recorded, it
  returns an error and removes the empty files.
* Recording requires `ffmpeg`; `record start` fails with a clear message if it
  is missing. `--size` must be even (yuv420p).
* A recording that is still active when you `quit` or `boot` is finalized first.

#### VGM sound logs

```sh
x68k record start --vgm [--out FILE.vgm] [--opm-only]
x68k step 20s
x68k record stop
```

`--vgm` records the sound chips instead of video: a standard VGM 1.61 stream
with YM2151 (OPM) register writes at 4 MHz and, unless `--opm-only` is given,
MSM6258 ADPCM data. The timeline is 44.1 kHz and the recorded duration is the
emulated time spent inside `step`.

* OPM writes are timestamped with the current scanline (about 1–2 VGM samples
  of resolution). At record start the frontend dumps the current OPM register
  state, so starting in the middle of a song works.
* ADPCM bytes are queued and emitted at the MSM6258's current sample rate, so
  players with a small sample FIFO receive them smoothly. The X68000's ADPCM
  clock modes are selected with the libvgm/vgmplay MSM6258 clock registers;
  use `--opm-only` for a log that avoids that extension.
* Start the VGM **before** the music or PCM sample starts. The OPM state is
  reconstructed, but the position of an already-playing ADPCM sample cannot
  be.
* `reset` and `load` are rejected while a VGM recording is active (the chips
  change state without register writes). Stop the recording, change state,
  then restart it.
* Play it with `vgmplay file.vgm` or convert with
  `vgm2wav --fade 0 file.vgm out.wav` (libvgm). A 20-second OPM-heavy segment
  is typically 100–200 KB; ADPCM adds at most ~40 KB/s.
* The VGM recorder and the MP4 recorder are mutually exclusive; `record stop`
  finalizes whichever is active.

### Input

```sh
x68k key tap <chord...> [--hold 100ms]
x68k key press <chord...> [--hold 100ms]     # alias
x68k key down <chord...>
x68k key up <chord...>
x68k key hold <DURATION> <chord...>
x68k key type "TEXT" [--delay 80ms]

x68k joy tap|press <button...> [--hold 100ms] [--port 1|2]
x68k joy down|up <button...> [--port 1|2]
x68k joy hold <DURATION> <button...> [--port 1|2]

x68k mouse move DX DY
x68k mouse down|up [left|right]
x68k mouse click [left|right] [--hold 100ms]
```

* `tap`/`press`/`hold`/`click` **advance time** (hold + one frame to deliver the
  release). `down`/`up`/`move` do not; follow them with `step`.
* Chords are joined with `+`: `ctrl+shift+s`, `shift+up`. Use `plus` for the
  `+` key. Uppercase letters and shifted symbols automatically hold shift.
* `key type` accepts printable ASCII plus `\n`, `\t`, `\r`, `\b`, `\\` and
  `\e` (escape). It holds shift as needed and advances a few frames per
  character. Japanese text cannot be typed.
* Joystick buttons: `up down left right a b x y l r start select`
  (aliases `trg1`/`trg2`, `run`, `mode`, `b1`/`b2`). `a` is TRG1 and `b` is
  TRG2 because the frontend configures the core with `VbtnSwap`.
* Mouse movement is **relative** and is delivered on the following frames; a
  large move is split across frames so the guest does not lose motion. Use
  several `mouse move` + `step` pairs to travel across the screen.
* `key`, `joy` and `mouse` responses and the `state` object report what is
  currently held.

### Media / machine

```sh
x68k disk insert <0|1> PATH
x68k disk eject <0|1>
x68k disk list
x68k save [NAME]        # default name: state-<frame>
x68k load NAME
x68k states
x68k reset
x68k status
x68k ping
x68k quit
```

* Floppy images are writable. Ejecting, swapping, booting and quitting flush
  changes back to the image file. Copy images first if you want to preserve
  the originals.
* SRAM (`sram.dat`, 16 KB) persists in `<home>/roms/keropi/`. Delete that file
  to get a fresh SRAM.
* `save`/`load` snapshot the emulated machine (CPU, RAM, VRAM, devices, SRAM)
  plus the frame/time counters into `<home>/states/NAME.bin` (zlib-compressed,
  typically 0.5–3 MB). `load` clears held input and the current framebuffer;
  run `step 0.1s` afterwards to render the restored screen. Load a state only
  into the same boot configuration (same game/media/RAM/CPU); the format is not
  portable across architectures or core versions. Floppy writes that happened
  after the save are not undone.
* `reset` reboots the machine and keeps the mounted media.
* `status` includes daemon uptime, media, recording, input state, screen hash
  and configuration.
* `quit` also stops the daemon; the next command starts an idle daemon again.

### `batch`, `doctor`, `help`

```sh
x68k batch [--file FILE] [--keep-going] < commands.txt
x68k doctor
x68k help [keys]
```

Batch lines are shell-like (`#` comments, single/double quotes, backslash
escapes). Each line is executed in order and the output contains the raw JSON
response of every command. Without `--keep-going`, the batch stops at the first
error.

`doctor` prints ROM/ffmpeg/daemon diagnostics without starting a machine.

## Time, screenshots and recordings

* One emulated frame is 1/55.46 s in 31 kHz mode and 1/61.46 s in 15 kHz mode
  (or the `compat` rates 55.5/59.94). The mode can change while a game boots.
* Screenshots use the core's native framebuffer, typically 512×512 or
  768×512. The aspect ratio is 4:3; pixels are not square.
* Recordings use a fixed 4:3 canvas (640×480 by default) with nearest-neighbour
  scaling from the native framebuffer, so mode switches and letterboxing stay
  consistent.
* There is no real-time pacing. `step 10s` may finish in 0.1 s; recording adds
  encoder cost but is still much faster than real time.

## JSON output

Every command prints exactly one JSON object. Success:

```json
{
  "ok": true,
  "command": "step",
  "data": {
    "frames": 667,
    "requested_seconds": 12,
    "emu_seconds": 12.01436422,
    "wall_ms": 144,
    "speed_x": 83.43,
    "snapshot": {"path": "...png", "width": 768, "height": 512, "scale": 1, "bytes": 82250}
  },
  "state": {
    "booted": true,
    "frame": 667,
    "emu_seconds": 12.01436422,
    "fps": 55.46,
    "video": {"ready": true, "width": 768, "height": 512, "mode": "31k", "aspect": "4:3"},
    "screen": {"hash": "f3643e33127a1bb2", "changed": false},
    "input": {"keys": [], "joy": {"p1": [], "p2": []}, "mouse": {"buttons": []}},
    "media": {"fdd0": "...", "fdd1": null, "hdd": null},
    "recording": null,
    "perf_last_step_ms": 144,
    "perf_speed_x": 83.43,
    "machine": {"ram_mb": 4, "cpu_mhz": 10, "compat_fps": false, "core": "PX68K 0.15+ 0ad84d7"}
  }
}
```

* `state.recording` is `null`, an MP4 object (`"format":"mp4"`), or a VGM
  object (`"format":"vgm"`, with `samples`, `adpcm`, `chips`).
* `state.screen.hash` is a 64-bit FNV-1a of the native framebuffer.
  `state.screen.changed` compares it with the screen at the previous command.
  This lets an agent wait for loading screens without fetching images.
* Errors use:

```json
{"ok": false, "command": "step",
 "error": {"code": "not_booted", "message": "...", "hint": "...", "exit": 1}}
```

Exit codes: `0` success, `1` command error, `2` usage error, `3` daemon
unreachable/startup failure. Error codes are stable strings
(`not_booted`, `media_missing`, `roms_error`, `bad_media`, `usage`,
`no_frame`, `recording_failed`, `state_missing`, `state_error`, ...).

## Sessions and files

Default home: `$X68K_HOME` or `~/.x68k` (mode 0700).

```
~/.x68k/
  daemon.sock, daemon.pid, daemon.lock, daemon.log
  roms/keropi/{iplrom.dat,cgrom.dat,sram.dat,config}
  screens/f00000667.png ...
  recordings/rec-YYYYMMDD-HHMMSS.mp4 ...
  states/NAME.bin ...
```

Use `--home` to run several independent machines/games at once. The daemon is
started on demand and serializes commands; `daemon.log` contains core crashes
and ffmpeg errors.

## Limitations / notes

* **ROMs are not included.** Supply `iplrom.dat` and `cgrom.dat` yourself.
* Save states are best-effort snapshots of px68k's state; they are not
  compatible with other emulators or px68k versions. There is no rewind
  history (only explicit named states). The only automatic persistence is
  floppy write-back and SRAM.
* The emulated RTC follows the host clock; games that read it are not
  deterministic across wall-clock time.
* VGM logs the OPM (and optionally the MSM6258). It does not include the
  Mercury sound board (not compiled in). OPM playback works in any VGM player;
  the 4 MHz ADPCM clock modes rely on the MSM6258 clock-register extension
  implemented by libvgm/vgmplay.
* `.hdf` is the only hard-disk format accepted by the core. If both an HDF and
  floppies are given, the HDF is the boot content and the floppies are inserted
  immediately after the core initializes.
* Floppy insertion happens after the first (initialization) frame. The X68000
  IPL scans drives a few frames later, so this reproduces normal booting; a
  program that probes the FDC in the first frame of reset could miss it.
* Mouse coordinates are relative; there is no host-side cursor or absolute
  position.
* `key type` uses US-ASCII key positions translated through px68k's keyboard
  table. Some shifted symbols may land on different JIS key positions than a
  real Japanese keyboard.
* The core is GPLv2 and is statically linked; the resulting binary is GPLv2.
* Disk images are treated as data. A malicious image exploits the emulator's
  C code, so only run images you trust.

## LLM guide

See [docs/LLM_GUIDE.md](docs/LLM_GUIDE.md) for a ready-to-use agent loop,
recommended step sizes, observation strategy, and a sample session.

## Tests

```sh
X68K_TEST_ROMS=$HOME/Documents/MPX68K \
X68K_TEST_DISK="$HOME/x68/game-diskA.dim" \
X68K_TEST_DISK2="$HOME/x68/game-diskB.dim" \
python3 tests/smoke_test.py --bin build/x68k
```

The test copies the images to a temporary session, boots, steps, snapshots,
exercises all input paths, records and probes the MP4, records and parses a
VGM, swaps disks, runs a batch, resets and quits. It skips MP4 checks when
ffmpeg is absent.

## Credits

px68k and px68k-libretro (hissorii, NONAKA Kimihiro, kenyahiro and
contributors), the C68K CPU core (Stephane Dallongeville/Yabause), fmgen
(cisc), and libretro-common. See [NOTICE](NOTICE) and
`third_party/px68k-libretro/COPYING`.

The vendored core carries a few small VGM logging hooks (OPM/ADPCM write
callbacks, OPM register shadow, scanline accessors); see
`third_party/px68k-libretro/HEADLESS_PATCH.md`. They do nothing unless a VGM
recording is active.
