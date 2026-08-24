# Halo desktop emulator — building and running

The real firmware, compiled for Linux on Zephyr's `native_sim` board: same app
(`applications/halo`), same boot order in `main.c`, same `modules/halo` code —
**with zero changes to the firmware tree**. Everything emulator-specific lives
under `emulator/` and is injected at build time:

- `emulator/module/` — out-of-tree Zephyr module (`-DZEPHYR_EXTRA_MODULES`):
  emulator Kconfig symbols; from ticket 0003 on, all emulator C code (stub
  headers/implementations, TCP transport, fakes).
- `emulator/boards/native_sim.conf` — config fragment, merged right after
  `prj.conf` via `-DCONF_FILE` (the app force-caches `EXTRA_CONF_FILE` for its
  debug/release switch, so that channel is unavailable).
- `emulator/boards/native_sim.overlay` — devicetree overlay
  (`-DEXTRA_DTC_OVERLAY_FILE`).
- `emulator/build.sh` — wraps the whole invocation.

Design + ticket queue: `emulator/README.md`.

**Status (ticket 0006):** the emulator links, boots to the Lua REPL and
serves the full REPL/data wire protocol on a TCP socket
(`127.0.0.1:9563`). `/lfs` is wired to the flash simulator with
hardware-identical geometry and persists to a host file.
`emulator/tools/repl_smoke.py` exercises the acceptance criteria
end-to-end (REPL echo, MTU 512/`max_length()` 511, data channel,
single-client policy, control codes 0x02–0x06). The unmodified device
tests in `applications/halo/tests/` run against it through the
`brilliant_ble` shim (`emulator/pyshim`); the M1 subset is green via
`emulator/tools/run_emu_tests.py`.

## Build

```sh
emulator/build.sh                    # native_sim_64 (primary)
BOARD=native_sim emulator/build.sh   # 32-bit (needs multilib + i386 SDL)
# artifacts: emulator/build/halo-emu (symlink to build/zephyr/halo-emu.exe)
```

## Building against your local firmware checkout

To test firmware you are developing (including uncommitted changes), point
the build at your west workspace — the one you build the device firmware
from:

```sh
emulator/build.sh --fw-ws ~/halo-firmware   # remembered in emulator/.fw-ws
emulator/build.sh                           # keeps using it
emulator/build.sh --fw-ws builtin           # back to the pinned internal clone
HALO_FW_WS=~/other-ws emulator/build.sh     # one-shot, nothing remembered
```

The workspace must contain `.west`, `alif/applications/halo` and
`zephyr/boards/posix/native_sim`. Its manifest pins are used as-is (the
emulator never runs `west update` on your workspace) and its sources are
consumed read-only — except that the app's own CMake rewrites
`applications/halo/VERSION` EXTRAVERSION at configure time, exactly as your
hardware builds do. The final "Built:" line names the tree and its
`git describe` (a `-dirty` suffix means local edits went in). Toolchain
(west/cmake/ninja) always comes from the emulator's internal venv, and
`emulator/tools/run_emu_tests.py` takes the device tests from the same
workspace. Switching workspaces pristines `build/` automatically.

Prerequisites: run `emulator/init.sh` once — it installs the host packages
(`build-essential libsdl2-dev pkg-config` …), creates the venv, clones the
firmware as `alif` (standalone clone of `HALO_FW_URL` pinned at `HALO_FW_REV`,
independent of any other checkout) and runs `west update`, all inside
`emulator/src/halo-ws`. No Zephyr SDK is needed for the emulator itself
(`ZEPHYR_TOOLCHAIN_VARIANT=host`); `init.sh --skip-sdk` leaves it out.

Fragments are read from `emulator/` itself (`emulator/boards/`,
`emulator/module/`), so emulator changes take effect without touching the
workspace; the firmware sources compile from the workspace's pinned `alif`
clone — re-point it (`git -C emulator/src/halo-ws/alif fetch && git -C
emulator/src/halo-ws/alif checkout --detach <rev>`) when the firmware side
moves. The app's CMake rewrites its `VERSION` EXTRAVERSION at configure time;
that shows up as a dirty `VERSION` in the clone and is harmless.

## Kconfig seams (all inside emulator/module/Kconfig)

- `HALO_EMULATOR` selects `HAS_PM`: `HALO_PM_MANAGER` unconditionally selects
  `PM`, which is a **fatal** y-select on POSIX without it. The PM hooks
  (`pm_state_set`, ...) are link errors until 0003 stubs them.
- `HALO_BLE_LUA_SERVICE` + its three buffer ints are **re-declared** (Kconfig
  ORs deps across definition locations) so the Lua runtime stays enabled
  without the Alif BLE manager; the TCP transport (0005) implements the
  `halo/ble_lua.h` API the firmware already calls.
- ~21 benign warnings remain: prj.conf values for hardware-gated symbols
  (AEC taps, external-SRAM window, watchdog timings, sound cues,
  `PRIVILEGED_STACK_SIZE`, `REGULATOR_FIXED_INIT_PRIORITY`,
  `MAX98357A_AUDIO_PROTECT_BUDGET_PERCENT`, `HALO_BLE_MAX_SERVICES`, MPIX log
  choice) whose menus are off on native_sim. Non-fatal ("assigned but got
  ''"); silencing them would require editing `prj.conf`, which emulator work
  must not do.

## Platform stubs (ticket 0003) — what stands in for silicon

All in `emulator/module/`; the firmware tree compiles **unmodified**.

- `include/se_service.h` + `src/se_stubs.c` — SE revision `"EMU"`
  (`frame.get_se_revision()`), fixed EUI-64 `2C:F7:F1:E3:00:00:00:01`.
- `include/power_mgr.h` + `src/pm_stubs.c` — Alif power_mgr HAL no-ops
  (pm_manager.c runs unmodified: callback registry, light/standby park +
  suspend/resume chains all real) plus the POSIX soc PM hooks; SOFT_OFF
  (deep sleep) exits the process.
- `src/shutdown_emu.c` + overlay `sm` node/`shutdown` alias — ship mode
  exits the process (real `sm-gpio` driver excluded, `CONFIG_SM_GPIO=n`).
- `include/gap_le.h`/`gapc.h`/`gapc_sec.h` — parse-only BLE ROM header
  stand-ins (layouts deliberately differ; bond storage must stay off).
- `src/fake_sensors.c` + overlay `vbat`/`accel`/`magn` nodes — fixed-value
  sensor skeletons (4000 mV / 80% / not charging / level gravity); tickets
  0011/0012 make them settable and fire the recorded triggers.
- `src/misc_stubs.c` — `sys_arch_reboot` (reboot = exit; relaunch to
  "reboot"), `halo_led_clear_state` until 0013 (`vga020_set_pan` moved to
  the display fake, ticket 0007).
- Overlay wires `sw0`→ real `gpio-button` driver on `gpio_emul` (0010 adds
  injection), `zephyr,panel`→`sdl_dc`, canvas enabled (pure software).
- Off until their tickets: audio stream (0014 backend), LED manager (0013).

## Filesystem: `/lfs` on the flash simulator (ticket 0004)

`modules/halo/src/file_manager.c` runs unmodified: it mounts littlefs on
`FIXED_PARTITION(storage_partition)`, auto-formatting on first mount failure,
and `lua_file.c` creates an empty `main.lua` if none exists — an empty flash
image boots clean. The overlay reshapes native_sim's simulated flash to
mirror the device MRAM so littlefs computes the identical layout as on
hardware: erase-block 1024 / write-block 16 (Balletto `mram_storage`) and a
128 KB `storage_partition` (vs the stock 16 KB), still at 0xfc000 inside the
2 MB `flash0`. littlefs Kconfig defaults satisfy every geometry constraint
(prog/read 16 | cache 64 | block 1024 | partition), so the boot log carries
no geometry warnings.

**Persistence is on by default.** The simulator mmaps a 2 MB host file —
`./flash.bin` in the launch directory, created blank on first run — so
`/lfs` (installed `main.lua`, `/lfs/settings` with brightness/pan/volume,
persisted logs) survives restarts, mirroring MRAM. Runtime flags:

```sh
./build/halo-emu --flash=halo_flash.bin   # name/locate the backing file
./build/halo-emu --flash_erase            # factory reset: blank the file at boot
./build/halo-emu --flash_in_ram           # throwaway run, nothing persists
```

Seeding: not needed for boot (see auto-create above); once the TCP transport
(0005) lands, the normal file API over the socket can install a `main.lua`.
A littlefs-python `mklfs.py` helper (build a pre-seeded image at offset
0xfc000, block size 1024) remains optional and unimplemented.

## Display: canvas on a fake CDC200 scanout buffer (ticket 0007)

`lua_display.c` runs **unmodified** — fonts, all 23 `frame.display.*`
functions, boot logo, power-save semantics. Its hardware path is gated
`#ifdef CONFIG_CDC200`, and the Kconfig symbol must stay `n` (setting it
would compile the real Alif driver, whose `cdc200_*` symbols poke MMIO).
So the module CMakeLists injects the **macro** at the compiler level
(`zephyr_compile_definitions(CONFIG_CDC200=1)`, gated by
`CONFIG_HALO_EMU_DISPLAY`) — Kconfig and CMake never see it — and
`src/display_fake.c` provides the three entry points the now-live code
calls:

- `cdc200_get_framebuffer()` — hands canvas a static 256×256 RGB888 host
  buffer (zero-copy, like the hardware layer-0 scanout buffer; dimensions
  from chosen `zephyr,display`, matching the hardware vga020 panel).
- `cdc200_set_enable()` — records scanout on/off (power-save semantics:
  off at boot, on only between resume and suspend).
- `vga020_set_pan()` — records the border-register offset (whole-frame
  shift at presentation time, never draw coordinates).

Blanking/PM still target the `sdl_dc` device (chosen `zephyr,panel`);
`CONFIG_SDL_DISPLAY_USE_HARDWARE_ACCELERATOR=n` so the device also comes
up under `SDL_VIDEODRIVER=dummy` (headless/CI). The boot logo splash is
back on (`CONFIG_HALO_BOOT_LOGO=y` via a module Kconfig re-declaration
that drops the `depends on CDC200`), so every boot exercises
init→resume→LZ4 decode→draw→suspend and holds the hardware ~3 s splash.

Presentation (ticket 0008) is `src/display_present.c`: a ~30 Hz thread
(`CONFIG_HALO_EMU_DISPLAY_FPS`) standing in for continuous hardware
scanout. While scanout is enabled it converts the RGB888 buffer to
ARGB8888 and `display_write()`s the full frame to `sdl_dc` — so the boot
logo and anything a script draws appear in the SDL window without an
explicit `show()`, exactly like on the device. Pan is applied at blit
time as a whole-frame shift (sign/clamp semantics mirror
`display_vga020.c`: ±50, +x right, +y down, black border shifts in);
idle frames are change-detected and skipped. Blanking needs no presenter
code: `lua_display.c` drives `display_blanking_on/off` on the panel
itself, and the presenter simply stops blitting while scanout is off.
Torn frames on a mid-draw blit are accepted — continuous hardware
scanout has the same property.

Headless (CI): `SDL_VIDEODRIVER=dummy build/halo-emu` — the same code runs
against SDL's dummy backend (`run_emu_tests.py` sets it automatically).

Read-side seam (shared with the 0009 screenshot):
`module/include/halo/emu_display.h` — `halo_emu_display_fb()`,
`halo_emu_display_scanout_enabled()`, `halo_emu_display_get_pan()`.

## TCP Lua transport (ticket 0005)

The REPL is BLE-only on hardware; the emulator serves the same PDU
semantics on `tcp://127.0.0.1:9563` (`CONFIG_HALO_EMU_TCP_PORT`, loopback
only — the REPL is arbitrary code execution). The seam is the existing
`halo/ble_lua.h` symbol set, cut by link-time polymorphism: exactly one
provider is linked — hardware `ble_lua.c`, this module's
`transport_tcp.c`, or (phase 2) a Zephyr-BT backend. The documented
contract lives in `emulator/module/include/halo/lua_transport.h`.

- **Wire framing** (TCP is a stream; GATT gave PDU boundaries):
  `[u8 channel][u16 le length][payload]`. Channel 0 = Lua RX/TX (payload =
  the exact GATT PDU: 0x01-marked data, 0x02–0x07 control codes, else REPL
  text), 1 = audio, 2 = video (device→host). One frame = one PDU; frames
  longer than `CONFIG_HALO_EMU_MTU` (512) or on unknown channels drop the
  connection. A second client is refused (single-connection BLE policy).
  `halo_ble_get_mtu()` reports 512 when connected → Lua
  `frame.bluetooth.max_length()` = 511, matching PROTOCOL.md.
- **Files**: `module/src/transport_tcp.c` (Zephyr half: the
  `halo_ble_lua_*` provider, ble_lua.c's ring/semaphore machinery, a pump
  thread) · `module/src/transport_tcp_bottom.c` (host half: BSD sockets +
  accept/read pthread, one-frame mailbox with TCP backpressure) ·
  `module/src/ble_stubs.c` (the `halo_ble_*` manager surface with
  `CONFIG_HALO_BLE_MANAGER=n`: fixed EUI-48 `2C:F7:F1:00:00:01`,
  connect/disconnect event callbacks, pairing-window no-op).
- **Control codes while Lua busy-loops**: on hardware the BLE ISR always
  gets CPU; on native_sim a busy loop freezes simulated time. The host
  thread therefore installs a Lua hook asynchronously on ctrl arrival
  (`emu_tcp_ctrl_notify`, the async-safe `lua_sethook` pattern); the hook
  idles the CPU so the pump can dispatch 0x03/0x04/… normally.
- **Reboot (0x02) = process exit** (`sys_reboot` → `posix_exit`). Relaunch
  to "reboot"; persistence lives in `flash.bin`, so a trivial wrapper
  restores hardware semantics:
  `while ./build/halo-emu --flash=halo_flash.bin; do :; done`
- **picolibc setjmp gotcha** (`module/src/setjmp_x86_64.S`): picolibc has
  no x86-64 setjmp, so the link silently bound host glibc's — whose
  jmp_buf is ~3× picolibc's 64 bytes. glibc setjmp smashed the caller's
  stack (Lua's `lua_longjmp.status`), hanging `lua_newstate()` at boot.
  The module ships a matching 64-byte-layout implementation.

Smoke test against a running emulator:

```sh
emulator/tools/repl_smoke.py            # framed REPL/data/ctrl checks
emulator/tools/repl_smoke.py --reboot   # also 0x02 (process exits)
```

Runtime gaps behind the transport: control plane (0009), fake-value
injection (0010–0016), audio/video backends (0014/0016).

## Python test shim: the device tests as conformance suite (ticket 0006)

`applications/halo/tests/` drives a real Halo over BLE via the
`brilliant-ble` PyPI package. `emulator/pyshim/brilliant_ble/` is an
API-compatible stand-in that speaks the 0005 TCP framing instead, so the
**unmodified** tests double as the emulator's conformance suite. Activation
is purely environmental — PYTHONPATH shadows the real package (it wins over
site-packages, so this also works under `uv run`, which would otherwise
install the real one):

```sh
PYTHONPATH=emulator/pyshim HALO_EMU_ADDR=127.0.0.1:9563 \
    python3 applications/halo/tests/test_version.py --name "Halo 00"
# or, with uv on PATH, the stock runner:
PYTHONPATH=$PWD/emulator/pyshim HALO_EMU_ADDR=127.0.0.1:9563 \
    applications/halo/tests/run_tests.py --name "Halo 00" --only test_time.py
```

The shim covers everything the tests use: `connect(name=…, …handlers)`,
`send_lua(…, await_print=True)` (first-notification semantics preserved),
`send_data(…, await_data=True)`, `send_audio` (TCP channel 1),
`upload_file_from_string` (escaped, chunked, print-fenced),
break/reset/remove control signals (0x03/0x04/0x05),
`max_lua_payload()`/`max_data_payload()` (MTU−3/−4), and the private
`_user_print_response_handler` attribute the fenced-probe helpers swap out.
Channels 1/2 (audio/video) surface as `audio_response_handler` /
`video_response_handler` for tickets 0014/0016.

**Green subset** — proven against the emulator, run it with:

```sh
emulator/tools/run_emu_tests.py             # launches halo-emu, fresh flash
emulator/tools/run_emu_tests.py --only test_file_api.py -v
emulator/tools/run_emu_tests.py --no-launch # reuse a running emulator
```

`test_version.py`, `test_time.py`, `test_compression.py`,
`test_file_api.py`, `test_file_execution.py`,
`test_bluetooth_callback_api.py`, and since ticket 0007 the display set
`test_display.py`, `test_display_bitmap.py`, `test_display_palette.py`,
`test_text_api.py` — needs only python3 (no uv/BLE/PyPI;
`pyshim/_stubs/` satisfies test_time.py's unused `luaparser` import when
the real package is absent). The runner scans for run_tests.py's failure
markers, relaunches the emulator if it exits mid-run (reboot semantics),
and exits non-zero on any failure. The remaining tests wait on their
subsystems: fakes (0010–0013), audio/mic (0014/0015), camera (0016),
BLE-specific behaviour (0018+).
