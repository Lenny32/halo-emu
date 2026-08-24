# 0014 — Audio backend: audio_stream.h over SDL audio + liblc3 (M4)

**Phase:** 1 — core emulator
**Depends on:** 0005, 0009
**Effort:** L

## Story

`audio_stream.c` is the single speaker/mic HAL (`lua_speaker.c`/`lua_microphone.c`
never touch a device) but it hard-includes Alif's ROM LC3 codec and pokes the T5838 AAD
registers in ~6 places — faking below it means faking the ROM codec anyway. Decision:
compile an **alternate backend** implementing the full `halo/audio_stream.h` contract
(~25 functions), not fake dmic/i2s drivers.

## Tasks

1. `modules/halo/src/emu/audio_stream_emu.c` (replaces `audio_stream.c` in the emu
   build) implementing: `audio_speaker_init/start/write/set_volume/stop`,
   `audio_microphone_init/start/read/set_gain/stop`, LC3 encode/decode wrappers,
   `audio_stream_register_source/sink`, AAD entry points, owner arbitration.
2. **LC3 via liblc3** (already fetched by the manifest allowlist) — same
   frame-duration/sample-rate configs as the Alif ROM path; keep the bad-frame mute
   heuristic if cheap, else omit and note.
3. Speaker out: ring buffer → `modules/halo/src/emu/audio_host_bottom.c`
   (native-simulator context) → `SDL_QueueAudio` (SDL already linked). 16-bit mono,
   pace by the **host audio callback**, not k_timers (sim-time drift risk — plan risk
   #9).
4. Mic in: `SDL_OpenAudioDevice(capture)` for the host mic OR WAV loop; source selected
   by control plane `mic wav <path>|host|off` (0009). Naïve resample to 8/16 kHz mono.
5. AAD (`audio_stream_aad_*`): stubs returning "disabled" (`test_aad.py` stays
   never-green on emu).
6. Owner arbitration (`AUDIO_OWNER_LUA/SYSTEM/LE_AUDIO`): replicate the priority logic
   so `lua_sound.c` (sfxr — pure C, runs as-is), startup sound, and Lua speaker/mic
   coexist as on hardware.
7. Green: `test_speaker_pcm.py`, `test_speaker_lc3.py`, `test_speaker_lc3_2.py`
   (channel-1 audio in the pyshim, 0006), `test_microphone.py`,
   `test_microphone_lc3.py` (WAV source).

## Key points in code

- `modules/halo/include/halo/audio_stream.h` — the contract; signatures frozen
- `modules/halo/src/audio_stream.c:341/:642/:1008-1126` — device grabs and AAD pokes
  (why we cut above, not below); reference semantics for arbitration/format handling
- `modules/halo/src/sfxr.c`, `audio_eq.c` — pure software, compile unmodified
- Startup sound scheduling in `applications/halo/src/main.c:169-195` (speaker "ready
  before main()" on hw; emu init order must not deadlock the retry loop)

## Acceptance criteria

- [ ] `frame.sound.play('coin')` audible on host; `frame.speaker.play` PCM + LC3 work
- [ ] `frame.microphone.read` returns WAV-fed samples; LC3 encode path works
- [ ] M4 test subset green; hardware build untouched
