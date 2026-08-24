# 0009 — Emulator control plane (inject button/IMU/battery/mic/camera)

**Phase:** 1 — core emulator
**Depends on:** 0005
**Effort:** M

## Story

The developer (and pytest) needs to poke the virtual hardware: press the button, tilt
the glasses, set the battery, feed mic audio and camera frames. A second TCP port with
a line-oriented text protocol is scriptable and `nc`-friendly. The fakes (0010–0016)
each register their handler here.

## Tasks

1. `modules/halo/src/emu/emu_control.c` (+ `emu_control_bottom.c`, host sockets) on
   `CONFIG_HALO_EMU_CTRL_PORT` (9564). Line protocol:
   ```
   button press|release|click|double|hold <ms>
   accel <x> <y> <z>          magn <x> <y> <z>       tap <1|2>
   battery <mv>|<pct>         charging on|off
   mic wav <path>|host|off    camera load <path>|pattern
   led? battery? state?       # state queries for assertions
   ```
2. Registration API so each fake plugs in:
   `emu_control_register(cmd_prefix, handler)` — control plane owns parsing/transport,
   fakes own semantics. Commands for not-yet-loaded fakes answer `ERR unsupported`.
3. `applications/halo/tools/emu/emuctl.py` — tiny CLI + importable py API used by test
   wrappers (0011–0016 acceptance runs).
4. Stretch (optional, time-boxed): SDL keyboard mapping via `SDL_AddEventWatch` in a
   bottom-half file — SPACE = button, arrows = tilt, B = battery step. The TCP plane
   remains the tested path.
5. Document the protocol in `EMULATOR.md`.

## Key points in code

- Same nsi top/bottom split as 0005 (`transport_tcp_bottom.c` is the pattern)
- Button injection ultimately drives `gpio_emul_input_set` (ticket 0010) so the real
  debounce/multi-press driver state machine runs — control plane emits edges with
  correct sim-time spacing for `click`/`double`/`hold`

## Acceptance criteria

- [ ] `echo "battery 3900" | nc localhost 9564` → `OK`; queries return state
- [ ] `emuctl.py` round-trips every command; unknown command → `ERR`
- [ ] Protocol documented
