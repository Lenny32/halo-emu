# 0003 — Platform stubs: Secure Enclave, PM, watchdog, shutdown, mem_manager audit

**Phase:** 1 — core emulator
**Depends on:** 0002
**Effort:** M
**Status:** DONE

## Story

`main.c` and several Lua bindings include Alif-silicon services unconditionally. To get
the app compiling on native_sim, stub the Secure Enclave, make PM a no-op that still
fires callbacks, keep the watchdog out, and satisfy the ship-mode/shutdown driver
lookups.

> **Reworked before implementation (AGENTS.md hard rule):** the original tasks placed
> stubs in `modules/halo/src/emu/` and added `#ifdef CONFIG_HALO_EMULATOR` guards inside
> `pm_manager.c` / `lua_system.c` / `lua_button.c` — all outside `emulator/`. Everything
> below is injected from `emulator/module/` + `emulator/boards/` instead; **zero firmware
> files were edited**.

## Tasks (as implemented)

1. **SE stubs** — `module/include/se_service.h` (stand-in header; the real one is only
   on the include path for Alif builds) + `module/src/se_stubs.c`:
   - `se_service_get_se_revision()` → `"EMU"` (surfaces as `frame.get_se_revision()`)
   - `se_system_get_eui_extension()` → fixed extension → stable EUI-64
     `2C:F7:F1:E3:00:00:00:01` via `frame.get_eui()`
   - `se_service_get_rnd_num()` → xorshift (nothing security-relevant links it on emu)
   - DCDC/clock profile APIs deliberately **absent**: their only users (se_mgmt.c, BLE
     manager) are compiled out; a new user should fail loudly at the header.
2. **PM manager** — `pm_manager.c` compiles and runs **UNMODIFIED**. Its only silicon
   binding is `<power_mgr.h>` (this repo's `subsys/powermgr/pm/`, include path only
   added under `HAS_ALIF_POWER_MANAGER`): `module/include/power_mgr.h` +
   `module/src/pm_stubs.c` provide the API as no-op successes (`cold_boot`→true, wakeup
   sources accepted). Callback registry, priority-ordered suspend/resume chains and the
   light/standby wakeup handshake are pure Zephyr and stay fully functional.
   `pm_stubs.c` also supplies the soc PM hooks the POSIX arch lacks (`pm_state_set`,
   `pm_state_exit_post_ops` — the link errors 0002's `select HAS_PM` comment promised):
   only SOFT_OFF can reach them (no DT cpu-power-states + it's the one state pm_manager
   forces), and SOFT_OFF = power-off = `posix_exit(0)`.
3. **Shutdown / ship-mode** — no source guards: `native_sim.overlay` adds an `sm` node
   (the firmware's own `sm-gpio` binding, wired to `gpio_emul`) plus the `shutdown`
   alias, so `lua_system.c:332` / `lua_button.c:264` resolve as on hardware.
   `module/src/shutdown_emu.c` claims the compatible and implements
   `<zephyr/drivers/sm/sm.h>`'s `shutdown()` as `posix_exit(0)`; the real driver is
   excluded (`CONFIG_SM_GPIO=n`) because both define the global `shutdown()`.
4. **Watchdog** — verified: every watchdog reference in `applications/halo/src/main.c`
   (fired-check at :79, init at :111) is inside `#ifdef CONFIG_HALO_WATCHDOG_MANAGER`;
   `=n` from 0002 needs nothing further.
5. **mem_manager audit** — zero code change, as predicted. On the 64-bit host the only
   finding is a cosmetic `(uint32_t)internal_mem_pool` cast in a `LOG_DBG`
   (`mem_manager.c:49`, `-Wpointer-to-int-cast`, log-only). The fixed ITCM external-SRAM
   constants are inside `#if defined(CONFIG_HALO_MEM_USE_EXTERNAL_SRAM)` — compiled out,
   never dereferenced on host.

### Scope pulled forward (files that compile unconditionally under `HALO_LUA_RUNTIME`)

The acceptance criterion ("everything compiles except BLE/transport") forces these in:

- **BLE ROM header stand-ins** — `module/include/{gap_le,gapc,gapc_sec}.h`, parse-only
  (`halo/ble_security.h` includes them unconditionally; lua_button.c includes that).
  Layouts intentionally differ from silicon: bond storage stays `=n` on emu.
- **Fake sensor skeletons** — `module/src/fake_sensors.c` + `module/dts/bindings/` +
  overlay nodes: `vbat` nodelabel (battery_manager.c:361), chosen `zephyr,accel`/
  `zephyr,magn` (lua_imu.c:26). Fixed values (4000 mV / 80% / not charging / level
  gravity); attr_set/get swallowed (BMA580-private attrs tolerated); triggers recorded,
  never fired. Tickets 0011/0012 extend these — **not** new firmware-tree drivers.
- **Button** — overlay `button` node (`gpio-button` on `gpio_emul`, production timings,
  ACTIVE_HIGH so gpio_emul's boot-low reads "released") + `sw0` alias
  (lua_button.c:465); the repo's own `input_gpio_button.c` builds for native_sim as-is
  (`CONFIG_GPIO/INPUT=y` — board-defconfig symbols on halo). 0010 keeps injection+tests.
- **Display odds** — `CONFIG_CANVAS=y` (pure software; halo enables it in the board
  defconfig), chosen `zephyr,panel = &sdl_dc` (lua_display.c:317; PM calls return
  -ENOSYS which it tolerates), `vga020_set_pan` no-op in `module/src/misc_stubs.c`
  (pan is a hardware border register). Real seam lands with 0007/0008.
- **Reboot** — `CONFIG_REBOOT=y` + `sys_arch_reboot` → `posix_exit(0)` in
  `misc_stubs.c` (pm_manager failsafes and the VM reset path call `sys_reboot()`).
- **LED** — `CONFIG_HALO_LED_MANAGER=n` per 0013; `halo_led_clear_state` no-op in
  `misc_stubs.c` under `#ifndef CONFIG_HALO_LED_MANAGER` (lua_button.c:258 calls it
  unconditionally). **0013 removes the stub when re-enabling.**
- **Audio** — `CONFIG_HALO_AUDIO_STREAM=n`: audio_stream.c hard-includes `t5838.h` and
  the ROM LC3 codec; 0014's decision is an alternate backend, so no t5838 stand-in.

## Key points in code

- `emulator/module/CMakeLists.txt` / `Kconfig` / `zephyr/module.yml` (`dts_root: .`
  for `module/dts/bindings/`) — the injection mechanism
- `emulator/boards/native_sim.conf` / `native_sim.overlay` — config/DT wiring
- `modules/halo/src/pm_manager.c` — untouched; reread its header comment before ever
  "simplifying" the stubs: the callback chains are load-bearing for lua_display & co.
- `modules/halo/src/se_mgmt.c` — stays hardware-only (`CONFIG_HALO_SE_MGMT=n` on emu)

## Acceptance criteria

- [x] `emulator/build.sh` (native_sim_64) compiles **all 1115 objects**; the sole
      failure is the final link, undefined = ticket 0005's list:
      `halo_ble_lua_{repl_read,repl_write,data_read,data_write,register_ctrl_handler}`,
      `halo_ble_{is_connected,get_mtu,get_address,conn_disconnect}`,
      `halo_ble_sec_pairing_window_open`
- [x] `west build -b halo applications/halo` (SDK in the emu workspace) builds clean;
      hardware paths untouched — the emulator module/fragments are only injected by
      `emulator/build.sh`, so the hardware binary is unchanged by construction
