# 0036 — Test gates: close the RFNE blind spot, stop leaking QEMU

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0035 (the RFNE fix this gate must protect)
**Effort:** S

## Story

Two problems surfaced while fixing 0035, both in the test infrastructure rather than the
models:

1. **The display gate could not catch the bug it was supposed to cover.**
   `rom-stub/test/fw_dispsmoke.c` waited for received I2C bytes by polling `IC_RXFLR`,
   sidestepping `IC_STATUS.RFNE` — the exact bit Zephyr's `i2c_dw` uses and the exact bit
   upstream QEMU never modelled. It even `#define`s `STATUS_RFNE` (:87) and never used it.
   That is why 0029 reported `pmic-ok` while every real I2C read returned `-EIO`.

2. **Every emulator run leaked a full-speed QEMU process.** `halo-emu` runs QEMU as a
   child (`subprocess.Popen`) whenever the REPL bridge or control socket is enabled, and
   the tests stop it with `proc.terminate()`. The default SIGTERM disposition killed the
   launcher outright, so its cleanup never ran: **QEMU was orphaned and kept running**, and
   the `/tmp/halo-emu-{ble,ctl}-*` dirs leaked. Measured after a handful of smoke runs: six
   orphaned emulators, the oldest alive 88 minutes.

3. **`smoke_audio.py` was flaky on "microphone amplitude matches the injection".** Not
   related to the leak above (it kept failing with zero orphans, 3 of 5 runs) — a genuine
   test bug, diagnosed under this ticket.

## Tasks

1. `rom-stub/test/fw_dispsmoke.c:130-134` — poll `IC_STATUS & STATUS_RFNE` instead of
   `IC_RXFLR`, matching `i2c_dw_data_read()`. (`:101` already polls `STATUS_TFE`, which
   upstream also never set — it passed only because the reset value is `0x6`; 0035 now
   pins `TFE`/`TFNF` high explicitly, so that poll stays valid.)
2. `halo-emu` — install SIGTERM/SIGHUP handlers that shut QEMU down and let the existing
   cleanup run:
   - `_install_term_handlers()` raises `SystemExit(128 + signum)` so the stack unwinds
     through `proc.wait()` into the `finally` blocks;
   - `_signal_child()` only *sends* the signal via `os.kill`. **`Popen.wait()` must not be
     called from the handler**: handlers run on the main thread, which is normally already
     inside `proc.wait()`, and `wait()` is not reentrant (it holds `_waitpid_lock`) — doing
     so deadlocks the launcher and leaves QEMU a zombie. This was hit and fixed during
     implementation; keep the comment that records it.
   - `_reap_child()` (terminate → wait 5 s → kill) runs from the `finally`, where no lock
     is held; `_set_child()` registers the child from both the normal path and
     `run_screenshot()`.
   - SIGINT is deliberately left alone: QEMU owns the terminal and receives Ctrl-C through
     the foreground process group.
   The tests need no change — fixing the launcher also fixes interactive use, where
   `kill`ing `halo-emu` previously left a stray emulator behind.
3. `tests/smoke_audio.py` — measure the microphone *after* the start-of-stream ramp.
   `frame.microphone.read()` drains a FIFO from the beginning of the stream rather than
   returning live audio, so the first 10 ms window always lands in the firmware's ramp-up
   no matter how long the test sleeps first. Consecutive windows measure
   4383 -> 7297 -> 10901 -> 12017 and then hold; the frequency is correct (1050 Hz)
   throughout, so only the level ramps. Drain windows until the level settles, then assert.
4. Note the outcome for ticket 0034 (CI), which runs the suite in sequence.

## Gate

- `tests/smoke_display.py` passes with the RFNE poll; **and with the 0035 overlay
  temporarily removed it must fail** — that is the proof the gate now covers the bug.
- The four smoke tests run back-to-back leaving zero orphaned QEMU processes and zero
  leaked temp dirs.
- `smoke_audio.py` passes repeatedly (5 consecutive runs), not 2 of 5.

## Outcome (done 2026-08-26)

1. `fw_dispsmoke.c` now polls `IC_STATUS & STATUS_RFNE`.
2. `halo-emu` gained `_set_child()` / `_signal_child()` / `_reap_child()` /
   `_install_term_handlers()` as described.
3. **Gate evidence:**
   - `smoke_display.py` passes with the fix in place; with
     `patches/files/hw/i2c/designware_i2c.c` moved aside and the pinned QEMU file restored
     (`git -C qemu checkout -- hw/i2c/designware_i2c.c`) and rebuilt, it fails with
     `FAIL: firmware reported pmic-FAIL`. Before this ticket the same revert still printed
     `pmic-ok`. Overlay restored and re-verified green afterwards.
   - `kill -TERM` on the launcher: exits 143, QEMU reaped, no zombie, temp dirs removed
     (previously: launcher survived, QEMU orphaned, dirs left behind).
   - `smoke_controls.py` → `smoke_audio.py` run as a pair 3× consecutively: all green,
     audio 21/21 each time, **0 orphans**. The full four-test suite in sequence: all green,
     0 orphans, 0 leaked temp dirs. `--screenshot` path re-checked (it shares `_set_child`).
3. `tests/smoke_audio.py` drains up to 8 microphone windows and measures the first one
   above 75 % of the injected amplitude.

**Correction:** the first draft of this ticket guessed the amplitude flake came from the
orphaned emulators competing for CPU. That was wrong — it kept failing (3 of 5 runs) after
the leak was fixed and with zero orphans running. Probing consecutive windows showed the
real cause: `read()` drains the stream from its start, so the measurement sat inside the
firmware's ramp-up and passed or failed on where the ramp fell in the window. Frequency was
correct in every window, which is what ruled out a resampling or timing fault in the
emulator's mic path. After the fix: 5 of 5 runs green (peaks 9090-11928, threshold 6000).
Both problems were real; only the causal link between them was imagined.
