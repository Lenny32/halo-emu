# 0015 — AEC enabled in the emulator build

**Phase:** 1 — core emulator
**Depends on:** 0014
**Effort:** M

## Story

The acoustic echo canceller already builds on host: `applications/halo/tests/aec/host/`
compiles the unmodified `audio_aec.c` with gcc, stub Zephyr headers, and a plain-C FFT.
In the firmware source, CMSIS-DSP and ITCM placement are behind
`#if defined(CONFIG_HALO_AUDIO_AEC_FDAF) && defined(CONFIG_CMSIS_DSP)` — so on
native_sim, enable AEC with CMSIS off and the portable path drops in.

## Tasks

1. Flip `CONFIG_HALO_AUDIO_AEC=y` (with `CONFIG_CMSIS_DSP=n`) in `native_sim.conf`;
   keep `CONFIG_HALO_AUDIO_AEC_TAPS=1024` parity with `prj.conf`.
2. Fix fallout: printf formats, `-O3;-ffast-math` flags on posix, section attributes
   (ITCM attribute must compile out — verify the guard covers all placements).
3. Wire the speaker reference tap: hardware uses `max98357a_audio_set_tx_tap()`; the
   emu audio backend (0014) must expose the equivalent tap feeding the AEC's far-end
   input. Follow the stub in `applications/halo/tests/aec/host/max98357a_audio.h`.
4. Validation: the offline suite `tests/aec/host/test_aec.c` (16-check regression) is
   the algorithm reference; in-emulator check = `frame.microphone.aec` toggles and
   `frame.microphone.diag('stats')` returns sane counters during a WAV-mic +
   speaker-playback session.

## Key points in code

- `modules/halo/src/audio_aec.c:19/:922` — the CMSIS/portable split; ITCM BUILD_ASSERT
  near the top must be POSIX-safe
- `applications/halo/tests/aec/host/` — proven stub pattern (virtual clock, plain-C FFT
  with identical packing)
- `modules/halo/src/lua_microphone.c` `diag('stats')` — ~55-field counter table for
  assertions

## Acceptance criteria

- [ ] native_sim build green with AEC on; no ITCM/CMSIS symbols leak in
- [ ] `diag('stats')` counters advance during an emulated session
- [ ] Hardware build unaffected (still CMSIS/ITCM path)
