# Driving an X68000 game from an LLM

This guide is written for an agent that can run shell commands and read image
files. The emulator is deterministic and turn-based: **nothing happens until
you ask for it**, and every command returns JSON.

## Mental model

* The machine lives in a background daemon. `x68k boot ...` starts it.
* `x68k step 2s` advances **emulated** time as fast as the host allows. A step
  is normally a fraction of a second of wall time.
* Screenshots are PNG files; the command returns a path. Videos are MP4 files.
* Input commands either change state instantly (`key down`, `mouse move`) or
  change state *and* advance time (`key tap`, `key hold`, `joy hold`,
  `mouse click`, `key type`).
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
   x68k mouse click left         # point-and-click games
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
| Point-and-click / mouse-driven UI | `mouse move 20 -5` then `step 0.1s`, `mouse click left` |
| Skip an intro | `key tap esc` or `key tap return` repeatedly |
| Multi-button action | `joy hold 300ms left+a` |

Notes:

* `joy` uses the X68000 joystick. `a` is TRG1 (the usual fire/confirm button),
  `b` is TRG2. Directional `joy hold` is the most natural way to walk.
* `key tap` holds for 100 ms by default; add `--hold 300ms` if a short tap is
  ignored. `key down` + `step` + `key up` gives frame-level control.
* `key type` advances time per character and can be slow (roughly 10 chars/s of
  emulated time). It handles uppercase, punctuation and `\n` automatically.
* Mouse movement is relative and is delivered over the next frames. After a
  large `mouse move`, run `step 0.2s` before clicking; the guest must process
  the motion before the click.
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
x68k record start --size 640x480
x68k joy hold 2s right
x68k step 3s
x68k record stop
```

`record stop` returns `data.recording` with `path`, `frames`, `fps`,
`video_seconds`, `emu_seconds`, `bytes` and codec information. Recordings are
real-time in **emulated** time: 10 seconds of stepping produces a ~10-second
file, regardless of how fast the host ran. `ffprobe` can extract frames or
audio if your agent cannot watch video.

To share a single frame instead, use `snap` or `step ... --snap`.

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
> `mouse move DX DY`, then `step 0.1s`, then `mouse click left`.
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
