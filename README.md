# Halo emulator

Desktop emulator for the Halo firmware: the **real firmware C code** (Lua runtime,
canvas renderer, managers, DSP) compiled for Linux on Zephyr's `native_sim` board.
SDL window as display, host audio for speaker/mic, fake IMU/camera/battery/button.
Device connection: TCP socket speaking the existing REPL wire protocol (phase 1),
then real BLE via the host's Bluetooth adapter so a phone connects as if to real
glasses (phase 2).

Full design rationale lives in the plan that produced these tickets; the tickets are
self-contained. This folder will be moved later — keep paths inside tickets relative
to the repo root.

**Hard rule (see AGENTS.md): the emulator never touches anything outside
`emulator/`.** The firmware builds unmodified; emulator Kconfig/C/config is
injected at build time via an out-of-tree Zephyr module (`emulator/module/`)
plus per-build fragments (`emulator/boards/`), wrapped by `emulator/build.sh`
(ticket 0002). Build/run doc: `emulator/EMULATOR.md`.

## Why this is feasible

- Lua bindings sit on mostly-clean seams (`halo/audio_stream.h`, the `halo_*` manager
  headers); `modules/canvas/canvas.c` is pure software.
- The AEC already builds on host (`applications/halo/tests/aec/host/`).
- The ~30 Python BLE tests in `applications/halo/tests/` are a ready-made conformance
  suite.
- The one thing that can never run off-silicon: Alif's proprietary ROM BLE stack
  (`CONFIG_BT_CUSTOM`, ~8700 lines in `modules/halo/src/ble_*.c`). It is replaced
  behind the existing `halo/ble_*.h` header APIs, which every consumer already uses
  exclusively.

## Workspace & health gate (ticket 0001 findings)

`./init.sh` reproduces the whole workspace from scratch (host packages, venv,
`alif` clone, `west update`, Zephyr SDK) and is safe to re-run;
`./init.sh --check` re-runs the health gate below. See `./init.sh --help`.

- West workspace: `emulator/src/halo-ws/` (confirmed by the user; confined —
  nothing written outside it). The firmware is materialized there as `alif`,
  a standalone clone of `HALO_FW_URL` pinned at `HALO_FW_REV` (defaults in
  `init.sh`) — the self-contained default, so the emulator keeps working
  with no other checkout on the machine, wherever `emulator/` lives or moves.
  For local firmware development, `build.sh --fw-ws <path>` builds from an
  explicit external west workspace instead (see `EMULATOR.md`).
  Python venv at `emulator/src/halo-ws/.venv`.
- Fork board path: `zephyr/boards/posix/native_sim/` (Zephyr 3.6-era hwmv1
  layout, as expected).
- **Primary target: `native_sim_64`** (user decision — 32-bit multilib/i386
  SDL packages not installed; only `libsdl2-dev` amd64). The 32-bit pointer
  model caveats (Lua lightuserdata, mem_manager address arithmetic) move to
  testing on 64-bit; revisit if it bites.
- Toolchain: host gcc 13.3 with `ZEPHYR_TOOLCHAIN_VARIANT=host`; SDL 2.30.
  `zephyr-sdk-0.16.5/` (minimal + arm-zephyr-eabi) lives in the workspace
  only for `-b halo` regression configure checks.
- Health gate: `hello_world` (prints, runs), `subsys/fs/littlefs`
  (flash_sim + littlefs format/mount/boot-count OK) and `drivers/display`
  (sdl_dc driver init OK) all build and run on `native_sim_64`.

## Tickets

Sequential; a ticket assumes all lower-numbered tickets in its dependency line are in
`tickets/done/`. Workflow rules: see `AGENTS.md`.

### Phase 1 — core emulator (TCP transport)
| # | Ticket | Milestone |
|---|---|---|
| 0001 | workspace-and-native-sim-gate | M0 gate |
| 0002 | emulator-build-skeleton | |
| 0003 | platform-stubs | |
| 0004 | filesystem-flash-sim | |
| 0005 | tcp-repl-transport | M1 |
| 0006 | python-test-shim | M1 green |
| 0007 | display-hal-extraction | |
| 0008 | sdl-display-backend | M2 |
| 0009 | emu-control-plane | |
| 0010 | button-fake | M3 |
| 0011 | battery-fake | M3 |
| 0012 | imu-fake-sensors | M3 |
| 0013 | led-fake | |
| 0014 | audio-stream-emu | M4 |
| 0015 | aec-on-host | M4 |
| 0016 | camera-video-emu | M5 |
| 0017 | ci-emulator-build | |

### Phase 2 — real BLE (phone connects)
| # | Ticket | Milestone |
|---|---|---|
| 0018 | ble-userchan-spike | gate |
| 0019 | ble-backend-kconfig-choice | |
| 0020 | zephyr-bt-core-advertising | |
| 0021 | zephyr-bt-lua-service | |
| 0022 | zephyr-bt-battery-and-security | |
| 0023 | mcumgr-ota-transport | |
| 0024 | phone-verification | phase-2 done |

Descoped (never green on emulator): LE Audio over ISO (USB-dongle hardware wall),
ANCS (optional phase 2.5 — clean-room GATT client), standby/light-sleep, AAD,
hardware BLE throughput parity.
