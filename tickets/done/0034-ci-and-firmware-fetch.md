# 0034 — CI + firmware download helper

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0027 (smoke), ideally 0030 (REPL tests)
**Effort:** M

## Story

Make the emulator a fixture: one command to fetch a released firmware and run it, and a CI
job that keeps the emulator green against firmware releases.

## Tasks

1. `halo-emu --fetch <version>`: download the firmware binary for a GitHub release of
   `brilliantlabsAR/halo-firmware` (tag e.g. `0.8.8`) into a local cache, then run it —
   the full "download a firmware and use it" UX. If releases ship only signed images,
   handle `zephyr.signed.bin` (already supported by 0027); document what artifact names
   are expected.
   *(Reworked per AGENTS.md: the cache is `./firmwares/` inside the repo, not
   `~/.cache/halo-emu/` — the emulator never writes outside its own tree. `$HALO_EMU_CACHE`
   relocates it for a CI runner that wants it elsewhere.)*
2. CI workflow (GitHub Actions in this repo): build the QEMU fork (cached), then
   headless boot smoke — `halo-emu -f <firmware> --headless`, assert the boot banner and
   `main()` completion (0028+); once 0030 lands, add `repl_smoke.py` and the M1 test
   subset. Firmware under test: the latest release artifact (via --fetch) — CI must not
   depend on any local checkout.
3. Version matrix note: the synthetic ROM stub is pinned to ROM map v1_2 — CI should fail
   loudly (not obscurely) when a release moves to a different ROM image version.
4. Running the smoke suite in sequence is safe as of **0036**: `halo-emu` now reaps QEMU on
   SIGTERM/SIGHUP, so runs no longer leak an orphaned emulator. CI should still assert no
   stray `qemu-system-arm` survives a job, so a regression of that teardown is caught in
   CI rather than as a mystery flake. (The parenthetical this ticket originally carried —
   that the orphans caused the one-off `smoke_audio` amplitude failure — was disproved in
   0036: the real cause was measuring inside the microphone's start-of-stream ramp.)

## Gate (acceptance)

- `halo-emu --fetch 0.8.8` (or the current latest release) boots headless with the boot
  banner asserted, on a machine with no `~/halo-firmware`.
- CI pipeline green: QEMU build (cached) + boot smoke (+ REPL tests when available).

## Outcome (done 2026-08-28)

### 1. `halo-emu --fetch <version>` — `tools/fetch_firmware.py`, wired into the launcher

`--fetch <tag|latest>` replaces `-f` (argparse rejects both or neither) and resolves to an
ordinary local path before anything else runs, so every downstream path — MRAM staging,
the ROM-map check, `--mcuboot`, the smoke tests — is unchanged. Cache layout
`firmwares/<tag>/<asset-name>`, already covered by `.gitignore`; a cached file is reused
when its size matches the release's, `--fetch-refresh` forces a re-download.
`$HALO_FW_TOKEN`/`$GITHUB_TOKEN` lift the anonymous API rate limit; `$HALO_FW_REPO` /
`--fetch-repo` point at a fork.

Artifact names, as published today (documented in the module docstring and in
`EMULATOR.md`):

| asset | tag kind | picked by |
|---|---|---|
| `<tag>.bin` | release | default |
| `<tag>-debug.bin` | release | `--fetch-debug` |
| `halo-firmware-<ver>-release.signed.bin` | pre-release | default |
| `halo-firmware-<ver>-debug.signed.bin` | pre-release | `--fetch-debug` |
| `halo-bootloader-<ver>-*.bin`, `VERSION.txt` | pre-release | never — `--fetch-asset` only |

Selection rule: `.bin` assets that are not a bootloader, preferring the requested flavour
(release by default, falling back with a warning), then an exact `<tag>.bin`, then a name
containing "firmware", then the shortest. Signed and unsigned both boot — 0027's vector
offset auto-detection already handles the imgtool header. `latest` resolves through
`/releases/latest` (newest non-pre-release); a bare tag is tried as given, then with and
without a leading `v`; an unknown tag lists the recent ones instead of 404-ing.
`tools/fetch_firmware.py --list` / `--asset` / `--repo` expose the same thing standalone.

### 2. CI — `.github/workflows/ci.yml`

One job on `ubuntu-24.04`: push, PR, nightly cron (a new release or a ROM-map bump shows
up as a red build) and `workflow_dispatch` with `firmware_version` / `device_tests`
inputs. Two caches, both restored before `./init.sh` (which is idempotent — warm, it
re-applies the patch overlay, runs an incremental ninja and rebuilds the ROM stub):
`deps/` keyed on `init.sh` (pinned ARM toolchain + liblc3), `qemu/` keyed on the patch set
with a `restore-keys` prefix so a patch edit still starts from the previous build tree.

Steps after the build: fetch both release assets → ROM-map check on each → boot smoke on
the release build **through `halo-emu --fetch`** (so the ticket's actual UX is what CI
exercises, not a file it happens to have) → boot smoke on the debug build with the banner
asserted → `tools/repl_smoke.py` → the M1 device-test subset → the stray-process check.
Nothing depends on a local checkout: the device suite is cloned from the public firmware
repo at the same tag as the firmware under test, and `uv` resolves each test's PEP 723
dependency header (`luaparser`, …) while `pyshim/` still shadows `brilliant_ble`.

### 3. ROM-map version gate — `tools/rom_map_check.py`

The firmware sits in MRAM at `0x80020800` and the ROM at `0x0009xxxx`, far out of `BL`
range, so every ROM call goes through a 32-bit address literal. The check scans the image
for word-aligned little-endian words inside the ROM window with the Thumb bit set and
intersects them with the vendored `rom_symbols_*_v1_2.lds` maps.

Calibration: 0.8.8, 0.8.9 and the signed pre-release asset each resolve **73 of ~194**
such literals — 37–38%, the remainder being ordinary constants that land in the window by
coincidence. The bare-metal `fw_blesmoke.bin` scores 26 of 28 (93%), `fw_dispsmoke.bin`
has none at all. So the discriminator is the **ratio**, not a count: a count threshold
rejects the small test firmwares, and — checked against a deliberately mangled image with
every ROM literal shifted by one entry (+0x10) — still passes a near miss, because 25 of
the shifted literals land on some *other* real symbol. That image scores 13% and is
refused; the untouched ones pass with a wide margin.

Three verdicts: `ok`, `none` (fewer than 8 ROM-window literals — nothing to judge, which
is how the bare-metal test firmwares keep working) and `mismatch`. `halo-emu` refuses a
`mismatch` **before** it writes MRAM or starts QEMU, naming the release, the ratio, the
`modules/hal/alif/ble/<ver>/rom_symbols_*.lds` to rebuild the stub against, and the
`--no-rom-map-check` override. CI runs the check as its own named step with
`--require-rom`, which also fails the `none` case — an application image with no ROM calls
is itself wrong. Without this, the failure mode is what the mangled image actually
produced: `Failed to initialize LC3 codec: -1`, then
`ROM stub trap: acc_mcc_configure (symbol #10) … unimplemented pinned ROM function`.

### 4. Boot smoke and teardown — `tests/smoke_boot.py`

Banner, bring-up and `main()` completion, on a throwaway MRAM image:

- **banner** — `main()` guards the `printk` with `CONFIG_HALO_LOG_LEVEL_DBG`, so only the
  `-debug` asset prints it and the string is garbage-collected out of the release build
  entirely. The check is therefore auto-detected: required iff the image contains
  `Hardware Version: `, overridable with `--banner yes|no`. CI asserts it on the debug
  asset (`--banner yes`), which is where the gate's "boot banner asserted" lives.
- **bring-up, in order** — `Power management initialized` → `BLE manager initialized`
  (the ROM stub answered `alif_ble_enable`) → `Lua runtime initialized successfully` →
  `MCUboot image confirmed`, which is literally main()'s last statement before it sleeps
  forever, so reaching it means every subsystem came up.
- **clean** — no fault/assert/panic markers, and exactly one boot (a watchdog reset would
  print the bring-up twice).
- **teardown** — the QEMU PIDs present before the run must be the only ones left after
  SIGTERM, catching a regression of 0036's reaping. CI repeats this as a job-wide step;
  both use a `[q]`-bracketed pgrep pattern so the check cannot match the shell running it.

A reader thread drains the console while the main thread enforces the deadline: the
console goes quiet for seconds at a time under TCG, and a firmware that hangs with nothing
more to say must still fail at the timeout rather than block in `readline()`.

### 5. Fixed-sleep boot waits replaced (CI runners are slower than this host)

`tools/repl_smoke.py` and `tools/run_emu_tests.py` both slept a flat 20 s for the Lua
runtime. Boot takes ~11 s of wall clock here and a good deal more on a 2-vCPU runner, so
that was a latent CI failure. Both now poll: connect, `0x03` to interrupt whatever
`main.lua` is doing, evaluate `print('up')`, and proceed the moment it answers (the bridge
accepts TCP before the guest is up, so a connect alone proves nothing). Bounded by
`--boot-wait` / `--boot-timeout`, default 120 s. Side effect: `repl_smoke` got *faster*
locally, 28 s → 18 s.

### 6. Gate evidence

All against release **0.8.9** (fetched, no local checkout involved):

- `halo-emu --fetch 0.8.8` / `--fetch latest` / `--fetch v0.8.9-3` (pre-release, picks
  `halo-firmware-0.8.9-release.signed.bin`) / `--fetch latest --fetch-debug` — all cache
  and boot; `--fetch 9.9.9` lists the real tags instead of failing obscurely.
- `tests/smoke_boot.py --fetch latest` → PASS (banner correctly skipped, release build).
- `tests/smoke_boot.py -f firmwares/0.8.9/0.8.9-debug.bin --banner yes` → PASS, all three
  banner lines. The same flag on the release asset fails, which is the proof the check has
  teeth: `FAIL: boot banner: no logo line … on the console`.
- `tools/rom_map_check.py --require-rom` on both 0.8.9 assets → 0; on the +0x10-shifted
  image → 1, and `halo-emu` refuses it without creating the MRAM file.
- `tools/repl_smoke.py -f firmwares/0.8.9/0.8.9.bin` → 10/10 checks green.
- `tools/run_emu_tests.py -f firmwares/0.8.9/0.8.9.bin --tests-dir <clone>/applications/halo/tests`
  → **6/6** M1 tests pass (`test_time.py` needs `luaparser`, which `uv` supplies in CI and
  a venv supplied here).
- Zero stray emulator processes after the whole sequence.

The workflow itself has not run on GitHub yet — it needs to be pushed first; everything it
runs was executed step by step locally, in order, on this host.
