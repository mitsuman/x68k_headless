# Driving an X68000 game from an LLM

This guide is written for an agent that can run shell commands and read image
files. The emulator is deterministic and turn-based: **nothing happens until
you ask for it**, and every command returns JSON.

## Mental model

* The machine lives in a background daemon. `x68k boot ...` starts it.
* `x68k step 2s` advances **emulated** time as fast as the host allows. A step
  is normally a fraction of a second of wall time.
* Screenshots are PNG files; the command returns a path. Videos are MP4 files.
* Input commands either change state instantly (`key down`, `mouse down`) or
  change state *and* advance time (`key tap`, `key hold`, `joy hold`,
  `mouse move`, `mouse click`, `mouse doubleclick`, `key type`).
  `mouse move --no-wait` is the instant variant.
* A 55–61 Hz X68000 frame is roughly 16–18 ms. Durations are rounded up to a
  frame; trust the returned `data.frames` / `data.emu_seconds`.

## The basic loop

1. **Boot** (once):
   ```sh
   x68k boot "game disk A.dim" "game disk B.dim" --roms ~/Documents/MPX68K
   ```
2. **Advance and look**:
   ```sh
   x68k step 5s --snap
   ```
   Read `data.snapshot.path` and open the image. The response also contains
   `state.screen.changed` and `state.screen.hash`.
3. **Act**:
   ```sh
   x68k key tap return           # menus / confirmations
   x68k joy hold 1s right        # movement in a game
   x68k key hold 500ms space     # keyboard-controlled games
   x68k mouse move 20 -5         # relative; waits for the guest
   x68k mouse click left         # single click
   x68k mouse doubleclick left   # open a desktop icon
   ```
4. **Repeat** from step 2. Add `--snap` to any `step` to observe the result in
   one round trip.
5. **Record** when the run matters:
   ```sh
   x68k record start
   x68k step 10s
   x68k record stop
   ```
6. **Stop the daemon** when done: `x68k quit`.

## What to send, and how much

* Prefer **one `step ... --snap`** over `step` followed by `snap`; it halves the
  number of calls and guarantees the image matches the reported frame.
* Use step sizes of **0.2–2 s** for gameplay, **5–20 s** for loading screens,
  logos and long intros. `step 60s` is fine when you are sure nothing
  interactive happens, but it may skip past a prompt.
* Do not call `status` after every action. The `state` object is attached to
  every successful response. Use `status` only when you need daemon/uptime or
  full configuration details.
* Use `state.screen.changed` to detect a still screen. If you step twice and
  `changed` stays `false`, stop wasting steps, try a different input, or wait
  longer.
* Keep an image from the latest step in your context and compare it to the new
  one visually. Names like `data.snapshot.path` are stable; old screenshots
  remain in `<home>/screens/`.

## Input strategy

| Situation | Best command |
|---|---|
| Title/menu with a highlighted option | `key tap return` / `key tap space` |
| Menu driven by arrows | `key tap down` then `key tap return` |
| Action game using the joypad | `joy hold 1s right`, `joy tap a` |
| Action game using the keyboard | `key hold 500ms z` |
| Text prompt / BASIC / Human68k shell | `key type "DIR\n"` |
| Point-and-click / mouse-driven UI | `mouse move 20 -5`, then `mouse click left` |
| Open a desktop icon / list item | `mouse doubleclick left` |
| Skip an intro | `key tap esc` or `key tap return` repeatedly |
| Multi-button action | `joy hold 300ms left+a` |

Notes:

* `joy` uses the X68000 joystick. `a` is TRG1 (the usual fire/confirm button),
  `b` is TRG2. Directional `joy hold` is the most natural way to walk.
* `key tap` holds for 100 ms by default; add `--hold 300ms` if a short tap is
  ignored. `key down` + `step` + `key up` gives frame-level control.
* `key type` advances time per character and can be slow (roughly 10 chars/s of
  emulated time). It handles uppercase, punctuation and `\n` automatically.
* Mouse movement is relative. `mouse move` advances emulated time until the
  guest has consumed the whole delta, so no extra `step` is needed before a
  click. It feeds one unit per frame to avoid the guest's pointer acceleration
  (a 100 px move costs about 100 frames). Use `--no-wait` only when you want to
  combine the move with your own `step`.
* `mouse click` is a single click; `mouse doubleclick` clicks twice with a
  100 ms gap by default (`--hold`, `--interval`). Both first deliver any queued
  movement, then click at the current pointer position.
* Chords use `+`: `key tap ctrl+s`, `key tap shift+up`. For the `+` key itself
  use `plus`.

## Waiting and loading

X68000 games often spend 10–30 seconds on a loader, sometimes asking for a disk
swap. A robust pattern:

```sh
x68k step 10s --snap      # inspect
x68k step 10s --snap      # still loading?
x68k step 10s --snap      # title screen?
```

If the screen is black but `state.video.ready` is true, it may be a real black
transition. Wait 1–3 s and snapshot again. If `state.video.ready` is false, you
booted but have not advanced far enough for the first draw.

Disk swaps:

```sh
x68k disk eject 1
x68k disk insert 1 "disk B.dim"
x68k step 3s --snap
```

Ejecting flushes save data back to the image, so the original file may change.

## Branching and retrying with save states

`save` captures the whole emulated machine (CPU, RAM, VRAM, devices, SRAM) plus
the frame/time counters. `load` restores it. This is the cheapest way to try a
risky move or a menu branch:

```sh
x68k save before-boss
x68k joy hold 2s right
x68k step 3s --snap
# that was a bad idea:
x68k load before-boss
x68k step 0.1s --snap     # render the restored screen
```

* States live in `<home>/states/NAME.bin` and are compressed (usually a few
  hundred KB to a few MB).
* `x68k save` without a name uses `state-<frame>`; `x68k states` lists names.
* Load only into the **same boot** (same game, media, RAM/CPU settings). The
  format is px68k-specific and not portable across core versions.
* Floppy writes made after the save are not rolled back — the disk image is
  separate from the state.
* Loading clears the held-key/mouse/joystick state and the framebuffer; always
  follow `load` with `step 0.1s` before taking a screenshot.
* Recording can continue across a load; the video will contain a jump.

## Recording for review

```sh
x68k record start            # default: fixed 1600x1024 canvas
x68k joy hold 2s right
x68k step 3s
x68k record stop
```

Every frame is resampled into a centred 4:3 rectangle (1364×1024 in the default
canvas, black bars at the sides) with Lanczos-3 when upscaling and an area
filter when downscaling. The canvas never changes, so a mode switch does not
alter the geometry. `--size WxH` (even values) picks a different canvas.

`record stop` returns `data.recording` with `path`, `width`, `height`,
`source_width`, `source_height`, `resolution_changes`, `frames`, `fps`,
`video_seconds`, `emu_seconds`, `bytes` and codec information. Recordings are
real-time in **emulated** time: 10 seconds of stepping produces a ~10-second
file, regardless of how fast the host ran. `ffprobe` can extract frames or
audio if your agent cannot watch video. Recording at 1600×1024 is roughly 2x
slower than real time.

If the game changes video mode while recording, check `data.warning` in the
response of the step that crossed the switch (`code:
"recording_resolution_changed"`, with `from` / `to`) and
`state.recording.resolution_changes`. It is informational: the canvas and the
4:3 framing stay the same.

To share a single frame instead, use `snap` or `step ... --snap`.

### Recording the music as VGM

If you want a chiptune-style sound log instead of a video, use `--vgm`:

```sh
x68k record start --vgm          # default: <home>/recordings/rec-<ts>.vgm
x68k step 20s
x68k record stop
```

The file is a standard VGM 1.61 log of the YM2151 (OPM) and, unless you pass
`--opm-only`, the MSM6258 ADPCM. OPM writes are timestamped per scanline and
the current OPM register state is written at the start, so you can begin in
the middle of a song. Start the recording **before** the music starts if you
want the ADPCM sample data; the position of a sample already playing cannot be
recovered. `reset` and `load` return `recording_active` while a VGM is being
recorded; stop it, change state, then start a new recording.

`record stop` returns `format:"vgm"`, `samples`, `seconds`, `bytes`, `adpcm`
and `chips`. Play the file with a VGM player (vgmplay) or render it with
`vgm2wav --fade 0 in.vgm out.wav` (libvgm). The VGM and MP4 recorders are
mutually exclusive.

## Inspecting PCG and palettes

`dump` extracts the sprite/BG character patterns and palette state at the
current frame. It does not advance time, so run it when the screen looks
interesting:

```sh
x68k dump
# -> data.dir = <home>/dumps/f00000667
#    pcg16.png / pcg16.bin, pcg8.png / pcg8.bin
#    palette.json / palette.bin, video.json
```

* `pcg16.*` contains 256 **16×16, 16-colour** patterns (the PCG used by
  sprites and BG); `pcg8.*` is the 256-pattern 8×8 BG set. The PNG is a
  16-column contact sheet; the BIN is pattern-major, one byte per pixel
  (0–15), 65,536 / 16,384 bytes.
* `palette.json` has 256 graphics and 256 text/sprite/BG entries. Each entry
  includes the raw 16-bit `0xGRBI` value, RGB565 and `#rrggbb`. `palette.bin`
  is the raw 1024-byte register image.
* `video.json` lists BG registers/scroll, all 128 sprite attributes (x, y,
  control word, pattern, palette bank, priority), CRTC/text registers and
  graphic scroll values.
* Use `--bank 0..15` to render the preview with a different text palette bank,
  `--scale 1..8` to enlarge it, `--grid` to separate tiles, and
  `--out DIR` to choose another directory.

An all-zero `pcg16.bin` can be normal: many X68000 games compose the whole
screen in GVRAM and never use hardware sprites/BG. If sprites are active,
`video.json` shows which pattern/bank each one uses.

## Recovering from errors

Every error is JSON with a stable `error.code`, a human-readable `message`, an
optional `hint`, and an `exit` code. Common cases:

| `error.code` | Meaning / action |
|---|---|
| `not_booted` | Boot a machine first. |
| `roms_missing` / `roms_error` | Pass `--roms DIR` with `iplrom.dat` + `cgrom.dat`; run `x68k doctor`. |
| `media_missing` / `bad_media` | Check the image path/extension; run `x68k doctor`. |
| `usage` | Bad argument. Read `hint`; `x68k help`, `x68k help keys`. |
| `no_frame` | Advance at least one frame before snapping. |
| `dump_failed` | Check the output directory/permissions; the message names the file. |
| `recording_failed` | Read `message`; ffmpeg may have died or the disk is full. |
| `daemon_unavailable` | The daemon failed to start; the error includes the log tail. |

The daemon survives emulator errors. A `boot` replaces the current machine; a
`reset` keeps the media. If the daemon itself dies, the next command starts a
new one (the machine state is lost).

## Token economy

* `--snap` results are one JSON object; images are files, not base64 blobs.
* A 768×512 screenshot is a small PNG (typically 1–80 KB).
* The state object repeats some data you already know. Read the fields you
  need; ignore `perf_*` unless you are profiling.
* Batch mode can replay a long action sequence in one call:
  ```sh
  printf 'step 1s\njoy hold 2s right\nstep 1s --snap\n' | x68k batch
  ```
  Each result embeds the full response, so verify the final `all_ok`.
* `dump` writes files. The response inlines the palettes (about 10 KB); read
  `palette.json`/`video.json` and the PCG images only when you need them.
* Use `x68k help keys` once and keep the list; do not call it repeatedly.

## Suggested system prompt

You can paste this into an agent's instructions:

> You are playing a Sharp X68000 game through the `x68k` CLI. The emulator is
> turn-based: time advances only when you run `step` or an input command that
> holds something. Drive it from the shell, one JSON command at a time.
>
> Start by running `x68k doctor`. Boot with
> `x68k boot "<disk images>" --roms <ROM dir>`. To observe, run
> `x68k step <duration> --snap` and look at `data.snapshot.path` with your
> image-reading tool. Prefer 0.5–2 s steps during play and 5–20 s while
> loading. The `state.screen.changed` flag tells you whether the last step
> changed anything.
>
> Use `joy hold <duration> <direction>` to move, `joy tap a` or
> `key tap return` to confirm, `key tap esc` to back out, and
> `key type "TEXT\n"` for text prompts. Mouse input is relative:
> `mouse move DX DY`, then `mouse click left` (or `mouse doubleclick left`
> for icons). `mouse move` already waits for the guest to consume the motion.
>
> If an action has no visible effect, try a longer hold, a different button,
> or wait 1–3 seconds. If an error returns, read `error.code` and `hint` and
> adapt. Record important runs with `record start` / `record stop`. Call
> `x68k quit` when finished.

## A sample session

```sh
$ x68k doctor
# ... ffmpeg ok, ROMs found in ~/Documents/MPX68K

$ x68k boot "Demon Slayer 3 (Disk A).dim" "Demon Slayer 3 (Disk B).dim" \
    --roms ~/Documents/MPX68K
{"ok":true,"command":"boot","data":{"machine":{"ram_mb":4,"cpu_mhz":10,"fps":61.46,...}}}

$ x68k step 12s --snap
{"ok":true,"command":"step","data":{"frames":667,"emu_seconds":12.01,"speed_x":83.4,
 "snapshot":{"path":"/home/me/.x68k/screens/f00000667.png","width":768,"height":512,...}},
 "state":{"frame":667,"fps":55.46,"video":{"ready":true,"width":768,"height":512,"mode":"31k"},
 "screen":{"hash":"f3643e...","changed":true},...}}
# -> open the PNG: "PLEASE WAIT A MOMENT. NOW LOADING GAME."

$ x68k step 20s --snap
# -> credits screen (game intro is automatic)

$ x68k key tap return
{"ok":true,"command":"key","data":{"keys":["return"],"frames":7,...}}

$ x68k step 3s --snap
# -> next screen; continue with menu choices

$ x68k joy hold 1s right --snap
# -> character/option moved right

$ x68k record start
$ x68k step 10s
$ x68k record stop
{"ok":true,"command":"record","data":{"recording":{"path":".../rec-....mp4",
 "frames":614,"fps":55.46,"video_seconds":11.07,"emu_seconds":11.07,...}}}

$ x68k quit
```
