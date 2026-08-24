# 0017 — CI: native_sim build + headless M1 test subset

**Phase:** 1 — core emulator
**Depends on:** 0006 (subset), 0008 (headless SDL)
**Effort:** S/M

## Story

Once the emulator boots and passes the M1 subset, CI should keep it green: a native_sim
build job plus a headless smoke run — the first hardware-free regression net this
firmware has ever had.

## Tasks

1. Extend `Dockerfile.ci` (workspace prebaked at `/opt/workspace`, ZEPHYR_BASE set):
   add host gcc multilib + SDL2 dev packages (and i386 variants if 32-bit target).
2. New job in `.github/workflows/ci.yml`: `west build -b native_sim applications/halo`,
   then run the emulator with `SDL_VIDEODRIVER=dummy` + `--flash=/tmp/flash.bin` and
   execute the M1 subset through the pyshim (`run_tests.py` filtered).
3. Timeout/teardown handling: emulator process killed after the run; test output
   uploaded as artifact on failure.
4. Keep the job non-blocking (allow-failure) until it proves stable for a couple of
   weeks, then make it required.

## Key points in code

- `Dockerfile.ci`, `.github/workflows/ci.yml` — existing hw-build job as template
- `SDL_VIDEODRIVER=dummy` — headless display; audio backend must also tolerate no
  audio device (SDL dummy audio driver) — guard in `audio_host_bottom.c`

## Acceptance criteria

- [ ] CI builds native_sim and runs the M1 subset green, headless
- [ ] Failure output actionable (logs + test report artifacts)
