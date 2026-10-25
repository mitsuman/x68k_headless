#!/usr/bin/env python3
"""Smoke tests for the x68k headless emulator.

Requires a real X68000 ROM set and one bootable floppy image:

    X68K_TEST_ROMS=$HOME/Documents/MPX68K \
    X68K_TEST_DISK="$HOME/x68/Demon Slayer 3 (...Disk A).dim" \
    X68K_TEST_DISK2="$HOME/x68/Demon Slayer 3 (...Disk B).dim" \
    python3 tests/smoke_test.py --bin build/x68k

The images are copied to a temporary directory first; the originals are never
written to.  ffmpeg/ffprobe are optional (recording tests are skipped if they
are not installed).  This is deliberately a black-box test: it drives the CLI
exactly like an LLM would.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

FAILED = []


def info(msg):
    print(f"  {msg}")


def check(cond, msg, extra=None):
    if cond:
        print(f"ok   {msg}")
    else:
        print(f"FAIL {msg}")
        if extra is not None:
            print(f"     {extra}")
        FAILED.append(msg)


def run(binary, home, *args, timeout=180):
    cmd = [binary, "--home", home, *args]
    t0 = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    dt = time.time() - t0
    raw = p.stdout.strip()
    try:
        data = json.loads(raw) if raw else None
    except json.JSONDecodeError:
        data = None
    if data is None:
        print(f"     non-JSON output for {' '.join(cmd)}: {raw[:500]} {p.stderr[:500]}")
    return p.returncode, data, dt, raw, p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.environ.get("X68K_BIN", "build/x68k"))
    ap.add_argument("--roms", default=os.environ.get("X68K_TEST_ROMS", ""))
    ap.add_argument("--disk", default=os.environ.get("X68K_TEST_DISK", ""))
    ap.add_argument("--disk2", default=os.environ.get("X68K_TEST_DISK2", ""))
    args = ap.parse_args()

    binary = os.path.abspath(args.bin)
    if not os.access(binary, os.X_OK):
        print(f"FAIL: binary not found/executable: {binary}")
        return 1
    if not args.roms or not args.disk:
        print("SKIP: set X68K_TEST_ROMS and X68K_TEST_DISK")
        return 0

    tmp = tempfile.mkdtemp(prefix="x68k-test-")
    home = os.path.join(tmp, "home")
    d1 = os.path.join(tmp, "disk1.dim")
    shutil.copyfile(args.disk, d1)
    d2 = None
    if args.disk2:
        d2 = os.path.join(tmp, "disk2.dim")
        shutil.copyfile(args.disk2, d2)

    has_ffmpeg = shutil.which("ffmpeg") is not None
    has_ffprobe = shutil.which("ffprobe") is not None

    def cleanup():
        try:
            run(binary, home, "quit", timeout=30)
        except Exception:
            pass
        shutil.rmtree(tmp, ignore_errors=True)

    try:
        print("== basics")
        rc, j, _, _, err = run(binary, home, "version")
        check(rc == 0 and j and j.get("ok") and "core" in j.get("data", {}), "version")
        rc, j, _, _, err = run(binary, home, "doctor")
        check(rc == 0 and j and j.get("ok"), "doctor", err)
        rc, j, _, _, _ = run(binary, home, "step", "1s")
        check(rc == 1 and j and j["error"]["code"] == "not_booted", "step before boot -> not_booted")
        rc, j, _, _, _ = run(binary, home, "step", "nonsense")
        check(rc == 2 and j and j["error"]["code"] == "usage", "bad duration -> usage (exit 2)")

        print("== boot / step / snapshot")
        boot_args = ["boot", d1]
        if d2:
            boot_args.append(d2)
        boot_args += ["--roms", args.roms]
        rc, j, _, _, err = run(binary, home, *boot_args)
        check(rc == 0 and j and j.get("ok"), "boot", err)
        if j and j.get("ok"):
            check(j["data"]["media"]["fdd0"] == d1, "FDD0 media path")
            if d2:
                check(j["data"]["media"]["fdd1"] == d2, "FDD1 media path")

        rc, j, dt, _, _ = run(binary, home, "step", "2s", "--snap")
        check(rc == 0 and j and j.get("ok"), "step 2s --snap")
        snap = os.path.join(home, "screens")
        if j and j.get("ok") and j["data"].get("snapshot"):
            path = j["data"]["snapshot"]["path"]
            check(os.path.isfile(path), "snapshot file exists")
            with open(path, "rb") as f:
                sig = f.read(8)
            check(sig == b"\x89PNG\r\n\x1a\n", "snapshot is a PNG")
            w0, h0 = j["data"]["snapshot"]["width"], j["data"]["snapshot"]["height"]
            check(w0 > 0 and h0 > 0, "snapshot dimensions")
            check(dt < 20, f"2s step is fast ({dt:.2f}s wall)")
        else:
            w0 = h0 = 0

        custom = os.path.join(tmp, "sub", "shot.png")
        rc, j, _, _, _ = run(binary, home, "snap", "--out", custom, "--scale", "2")
        check(rc == 0 and j and j.get("ok") and os.path.isfile(custom), "snap --out (nested dir, 2x)")
        if j and j.get("ok"):
            check(j["data"]["width"] == w0 * 2 and j["data"]["height"] == h0 * 2, "snap scale doubles dimensions")

        print("== save states")
        rc, j, _, _, _ = run(binary, home, "save", "smoke1")
        check(rc == 0 and j and j.get("ok") and j["data"]["save"]["bytes"] > 0, "save state")
        rc, j, _, _, _ = run(binary, home, "step", "--frames", "5")
        check(rc == 0, "step after save")
        rc, j, _, _, _ = run(binary, home, "load", "smoke1")
        check(rc == 0 and j and j.get("ok") and j["data"]["load"]["frame"] > 0, "load state")
        rc, j, _, _, _ = run(binary, home, "step", "--frames", "1")
        check(rc == 0, "step after load")
        rc, j, _, _, _ = run(binary, home, "states")
        check(rc == 0 and j and "smoke1" in j["data"]["states"], "states lists the save")

        print("== input")
        rc, j, _, _, _ = run(binary, home, "key", "tap", "return")
        check(rc == 0 and j and j.get("ok"), "key tap return")
        rc, j, _, _, _ = run(binary, home, "key", "tap", "ctrl+shift+s", "--hold", "50ms")
        check(rc == 0 and j and j.get("ok"), "key tap chord")
        rc, j, _, _, _ = run(binary, home, "key", "down", "a")
        check(rc == 0 and j.get("data", {}).get("down") is True, "key down a")
        rc, j, _, _, _ = run(binary, home, "step", "0.1s")
        rc, j, _, _, _ = run(binary, home, "key", "up", "a")
        check(rc == 0 and j.get("data", {}).get("down") is False, "key up a")
        rc, j, _, _, _ = run(binary, home, "key", "hold", "200ms", "right")
        check(rc == 0 and j and j.get("ok") and j["data"]["frames"] > 0, "key hold duration")
        rc, j, _, _, _ = run(binary, home, "key", "type", "DIR\\n", "--delay", "60ms")
        check(rc == 0 and j and j.get("ok") and j["data"]["chars"] == 4, "key type with \\n")
        rc, j, _, _, _ = run(binary, home, "key", "tap", "nosuchkey")
        check(rc == 2 and j and j["error"]["code"] == "usage", "unknown key -> usage")

        rc, j, _, _, _ = run(binary, home, "joy", "tap", "a", "--hold", "50ms")
        check(rc == 0 and j and j.get("ok"), "joy tap a")
        rc, j, _, _, _ = run(binary, home, "joy", "hold", "200ms", "left", "a", "--port", "1")
        check(rc == 0 and j and j.get("ok") and j["data"]["frames"] > 0, "joy hold left+a")

        rc, j, _, _, _ = run(binary, home, "mouse", "move", "20", "-10")
        check(rc == 0 and j and j.get("ok"), "mouse move")
        rc, j, _, _, _ = run(binary, home, "step", "0.1s")
        rc, j, _, _, _ = run(binary, home, "mouse", "click", "left", "--hold", "50ms")
        check(rc == 0 and j and j.get("ok"), "mouse click left")

        print("== disks")
        if d2:
            rc, j, _, _, _ = run(binary, home, "disk", "eject", "1")
            check(rc == 0 and j["data"]["media"]["fdd1"] is None, "disk eject 1")
            rc, j, _, _, _ = run(binary, home, "disk", "insert", "1", d2)
            check(rc == 0 and j["data"]["media"]["fdd1"] == d2, "disk insert 1")
        rc, j, _, _, _ = run(binary, home, "disk", "list")
        check(rc == 0 and j and j.get("ok"), "disk list")

        print("== recording")
        if has_ffmpeg:
            rec = os.path.join(tmp, "rec.mp4")
            rc, j, _, _, err = run(binary, home, "record", "start", "--out", rec)
            check(rc == 0 and j and j.get("ok"), "record start", err)
            rc, j, _, _, _ = run(binary, home, "step", "0.6s")
            rc, j, _, _, err = run(binary, home, "record", "stop")
            check(rc == 0 and j and j.get("ok"), "record stop", err)
            check(os.path.isfile(rec), "recording file exists")
            if os.path.isfile(rec) and has_ffprobe:
                pr = subprocess.run(
                    ["ffprobe", "-v", "error", "-show_entries",
                     "stream=codec_type", "-show_entries", "format=duration",
                     "-of", "json", rec],
                    capture_output=True, text=True)
                try:
                    pj = json.loads(pr.stdout)
                except json.JSONDecodeError:
                    pj = {}
                types = [s.get("codec_type") for s in pj.get("streams", [])]
                duration = float(pj.get("format", {}).get("duration", 0))
                check("video" in types, "recording has a video stream")
                check("audio" in types, "recording has an audio stream")
                check(0.3 <= duration <= 1.0, f"recording duration ~0.6s (got {duration:.3f}s)")

            rec2 = os.path.join(tmp, "rec_noaudio.mp4")
            run(binary, home, "record", "start", "--out", rec2, "--no-audio", "--fps", "30")
            run(binary, home, "step", "0.4s")
            rc, j, _, _, _ = run(binary, home, "record", "stop")
            check(rc == 0 and os.path.isfile(rec2), "video-only recording")
        else:
            info("ffmpeg not found: skipping recording tests")

        print("== batch")
        script = os.path.join(tmp, "commands.txt")
        with open(script, "w") as f:
            f.write("# a comment\nstep 0.1s\nsnap\nstatus\n")
        rc, j, _, _, err = run(binary, home, "batch", "--file", script)
        check(rc == 0 and j and j.get("ok") and j["data"]["count"] == 3, "batch file", err)

        print("== status / reset / quit")
        rc, j, _, _, _ = run(binary, home, "status")
        check(rc == 0 and j["data"]["daemon"]["pid"] > 0 and j["state"]["booted"], "status")
        rc, j, _, _, _ = run(binary, home, "reset")
        check(rc == 0 and j["state"]["frame"] == 0, "reset frame counter")
        rc, j, _, _, _ = run(binary, home, "step", "0.2s")
        check(rc == 0 and j["state"]["screen"]["hash"] is not None, "step after reset")

        rc, j, _, _, _ = run(binary, home, "quit")
        check(rc == 0 and j and j.get("ok") and j["data"]["stopped"], "quit")
        time.sleep(0.3)
        rc, j, _, _, _ = run(binary, home, "ping")
        check(rc == 0 and j and not j["data"]["booted"], "daemon restarts idle after quit")
    finally:
        cleanup()

    print()
    if FAILED:
        print(f"FAILED ({len(FAILED)}):")
        for f in FAILED:
            print(f"  - {f}")
        return 1
    print("ALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
