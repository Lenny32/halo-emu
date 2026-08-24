# Copyright (c) 2025 Brilliant Labs
# SPDX-License-Identifier: Apache-2.0
#
# Out-of-tree emulator module. All emulator C code lives here, under
# emulator/module/src/ — never inside the firmware tree.
#
# Two build contexts (native-simulator top/bottom pattern — see how the
# fork's display_sdl driver does it):
#   - "top" halves run in the Zephyr context:
#         zephyr_library_sources(src/<file>.c)
#   - "bottom" halves run in the host context (raw sockets, SDL audio, ...):
#         target_sources(native_simulator INTERFACE src/<file>_bottom.c)
#
# Sources: 0003 (platform stubs), 0005 (TCP transport implementing the
# halo/ble_lua.h API), 0009 (control plane) and the fake-peripheral tickets.

if(CONFIG_HALO_EMULATOR)
    # Stand-ins for Alif-silicon headers that firmware sources include
    # unconditionally (se_service.h, power_mgr.h) — global, so the firmware
    # tree compiles against them unmodified.
    zephyr_include_directories(include)

    zephyr_library()
    zephyr_library_sources(
        src/se_stubs.c       # SE services: "EMU" revision, fixed EUI, rnd
        src/pm_stubs.c       # Alif power_mgr HAL no-ops + posix soc PM hooks
        src/shutdown_emu.c   # sm-gpio ship-mode driver -> posix_exit()
        src/fake_sensors.c   # vbat/accel/magn skeletons (0011/0012 extend)
        src/misc_stubs.c     # sys_arch_reboot, LED stand-in
        src/display_fake.c   # cdc200 fb/enable + vga020 pan fakes (0007)
    )

    # Display (ticket 0007, reworked): lua_display.c's whole canvas path
    # (fonts, 23 draw functions, boot logo) is gated #ifdef CONFIG_CDC200.
    # The Kconfig symbol has to stay n — setting it would compile the real
    # Alif driver (zephyr_library_sources_ifdef in zephyr/drivers/display)
    # whose cdc200_* symbols would collide with display_fake.c and poke MMIO.
    # So inject the macro at the compiler level instead: Kconfig and CMake
    # never see it, only the preprocessor does, and display_fake.c provides
    # the three entry points the now-live code calls.
    if(CONFIG_HALO_EMU_DISPLAY)
        zephyr_compile_definitions(CONFIG_CDC200=1)
        # SDL presenter (ticket 0008): ~30 Hz thread blitting the fake
        # scanout buffer to the sdl_dc window while scanout is enabled
        # (RGB888 -> ARGB8888, pan applied at blit time).
        if(CONFIG_SDL_DISPLAY)
            zephyr_library_sources(src/display_present.c)
        endif()
    endif()

    # picolibc ships no x86-64 setjmp: without this the link silently binds
    # the HOST glibc setjmp, whose jmp_buf is ~3x picolibc's 64 bytes — it
    # smashes the caller's stack (Lua then hangs in lua_newstate). See the
    # header comment in the .S file.
    zephyr_library_sources(src/setjmp_x86_64.S)

    # TCP Lua transport (ticket 0005): the emulator's provider of the
    # halo/ble_lua.h symbol set. Top half in the Zephyr context, bottom
    # half (BSD sockets + pthread) in the native_simulator runner.
    if(CONFIG_HALO_TRANSPORT_TCP)
        zephyr_library_sources(src/transport_tcp.c)
        target_sources(native_simulator INTERFACE src/transport_tcp_bottom.c)
    endif()

    # halo_ble_* manager surface for CONFIG_HALO_BLE_MANAGER=n
    # (lua_bluetooth/lua_button/main link these unconditionally).
    if(NOT CONFIG_HALO_BLE_MANAGER)
        zephyr_library_sources(src/ble_stubs.c)
    endif()
endif()
