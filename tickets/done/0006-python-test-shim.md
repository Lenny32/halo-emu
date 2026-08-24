# 0006 — Python transport shim: run the existing test suite against the emulator (M1)

**Phase:** 1 — core emulator
**Depends on:** 0005
**Effort:** M

## Story

`applications/halo/tests/` holds ~30 Python tests that drive a real device over BLE via
the `brilliant-ble` package. A drop-in shim speaking the 0005 TCP framing makes them the
emulator's conformance suite — unmodified.

## Tasks

> Paths reworked from the original ticket text (`applications/halo/tools/emu/pyshim/…`):
> the AGENTS.md hard rule keeps all emulator files inside `emulator/`, and PYTHONPATH
> activation makes the shim location immaterial to the tests.

1. `emulator/pyshim/brilliant_ble/__init__.py` — API-compatible
   class: async `connect(name=…)`, `send_lua(…, await_print=True)`, `send_data`,
   `print_response_handler`, control-signal helpers (break/reset), speaking
   `[u8 channel][u16le len][payload]` to `HALO_EMU_ADDR` (default `localhost:9563`).
2. Activation: `PYTHONPATH=emulator/pyshim HALO_EMU_ADDR=… python
   run_tests.py …` — document in `EMULATOR.md`. No test-file edits.
3. Channel 1/2 (audio/video) receive paths included — needed by tickets 0014/0016.
4. Mark the M1 green subset and wire a convenience runner (make target or script):
   `test_version.py`, `test_time.py`, `test_compression.py`, `test_file_api.py`,
   `test_file_execution.py`, `test_bluetooth_callback_api.py`.

## Key points in code

- `applications/halo/tests/run_tests.py` — the runner; tests import `brilliant_ble`
- `applications/halo/tools/verify.py` — reference for break-signal/VM-reset etiquette
  (send break before REPL commands, reset after so main.lua resumes)
- `applications/halo/tests/` — `preserve_main_lua()` pattern: tests that overwrite
  `main.lua` must save/restore; the shim must not break this

## Acceptance criteria

- [x] M1 subset green against the emulator, zero test-file modifications
      (6/6 via `emulator/tools/run_emu_tests.py`, 2026-08-24; only pre-existing
      CRLF churn in `applications/halo/tests/`, no content changes)
- [x] Same subset still green against real hardware without the shim (no regression)
      — holds by construction: no file outside `emulator/` was touched; hardware runs
      use the real PyPI package unless `emulator/pyshim` is explicitly on PYTHONPATH.
      (Not re-run on a device this session — no dev kit attached.)

## Implementation notes (done 2026-08-24)

- **API surface** was reverse-engineered from the tests themselves (the real
  `brilliant-ble` isn't installed here); everything they touch is covered:
  `connect(name, print/data_response_handler, …)` (returns `"Halo 00"` — 4th
  EUI-48 byte of the transport's fixed `2C:F7:F1:00:00:01`), `disconnect`,
  `is_connected`, `send_lua(show_me, await_print)`, `send_data(await_data)`,
  `send_audio`, `send_break/reset/remove/exit/reboot_signal` (0x03/0x04/0x05/
  0x06/0x02), `upload_file_from_string`/`upload_file`,
  `max_lua_payload()`/`max_data_payload()` (MTU−3/−4, MTU 512 overridable via
  `HALO_EMU_MTU`).
- **Semantics that had to match hardware exactly**:
  - `_user_print_response_handler` is a plain attribute assigned after
    construction — `halo_device_file._Fenced` and the test subclasses swap it
    at runtime and restore the previous value.
  - `await_print`/`await_data` return only the FIRST following notification
    (the fenced-probe helpers in the tests depend on draining the rest).
  - Print payloads carry no trailing newline (`luaport.h` defines
    `lua_writeline()` empty — one `print()` = one notification).
  - The data handler receives a buffer without `.decode()` (bytearray here;
    see the memoryview comment in test_bluetooth_callback_api.py).
  - `send_audio` silently drops over-MTU packets (test_speaker_pcm.py relies
    on it); everything else raises, so oversize frames can never hit the
    transport's drop-the-connection protocol check.
- **`upload_file_from_string`** escapes every byte (incl. `\ddd` padded
  decimal escapes for non-printables, per-UTF-8-byte) and fences each
  `f:write()` chunk with an awaited `print(0)` — the transport's RX ring
  drops PDUs after a 100 ms stall (`RX_RING_WAIT_MS`), so unpaced back-to-back
  writes could lose chunks. `preserve_main_lua()` works unchanged on top.
- **Runner**: `emulator/tools/run_emu_tests.py` (python3-only — no uv, no
  PyPI): launches `zephyr.exe --flash=<fresh tmp>` (or `--no-launch`,
  `--flash` to persist), waits for the port, runs the M1 subset with
  run_tests.py's timeout/failure-marker semantics, relaunches the emulator if
  it exits mid-run (reboot semantics), non-zero exit on failure.
  `pyshim/_stubs/luaparser/` satisfies test_time.py's declared-but-unused
  `luaparser` import when the real package is absent (added to PYTHONPATH
  only after an import probe fails).
- **Verified end-to-end**: 6/6 green — test_file_api 41/41 assertions, LZ4
  round-trip decode in test_compression, both data-channel echoes in
  test_bluetooth_callback_api, require()+Ctrl-D main.lua execution with
  break/silence checks in test_file_execution.
- Known cosmetic quirk (firmware-side, pre-existing): `print(frame.battery_level())`
  emits `*float*` on native_sim — picolibc's float printf isn't enabled, so
  Lua can't format floats. None of the M1 tests assert on the value.
