# 0034 — CI + firmware download helper

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0027 (smoke), ideally 0030 (REPL tests)
**Effort:** M

## Story

Make the emulator a fixture: one command to fetch a released firmware and run it, and a CI
job that keeps the emulator green against firmware releases.

## Tasks

1. `halo-emu --fetch <version>`: download the firmware binary for a GitHub release of
   `brilliantlabsAR/halo-firmware` (tag e.g. `0.8.8`) into a local cache
   (`~/.cache/halo-emu/` or `./firmwares/`), then run it — the full
   "download a firmware and use it" UX. If releases ship only signed images, handle
   `zephyr.signed.bin` (already supported by 0027); document what artifact names are
   expected.
2. CI workflow (GitHub Actions in this repo): build the QEMU fork (cached), then
   headless boot smoke — `halo-emu -f <firmware> --headless`, assert the boot banner and
   `main()` completion (0028+); once 0030 lands, add `repl_smoke.py` and the M1 test
   subset. Firmware under test: the latest release artifact (via --fetch) — CI must not
   depend on any local checkout.
3. Version matrix note: the synthetic ROM stub is pinned to ROM map v1_2 — CI should fail
   loudly (not obscurely) when a release moves to a different ROM image version.

## Gate (acceptance)

- `halo-emu --fetch 0.8.8` (or the current latest release) boots headless with the boot
  banner asserted, on a machine with no `~/halo-firmware`.
- CI pipeline green: QEMU build (cached) + boot smoke (+ REPL tests when available).
