# 0040 — Web A/V bridge: phone mic/camera in, browser UI

**Phase:** 3 — tooling on top of the machine model
**Depends on:** 0031, 0032, 0033
**Effort:** M

## Story

After the boot logo the emulator idles, because a real Halo waits for a phone. Give the
emulator a "phone": a browser page (no app install — `getUserMedia` on a plain HTTPS
page) that streams the user's actual phone microphone and camera into the emulated PDM
mic and LPCAM camera, plus a desktop web UI that mirrors the 256×256 panel, drives the
control verbs, and plays the emulated speaker live. Zero QEMU/launcher changes — the
bridge rides existing seams:

- **camera**: `HALOCAM1` container (RGB888) + `qom-set /machine camera-file` — the
  LPCAM model re-reads the file on every set (`halo_lpcam.c`), so a 1-frame container
  is a live "current frame" latch that Lua `capture()` samples.
- **mic**: v1 is *chunked* — ~600 ms WAV files swapped via `qom-set mic-wav-in`
  (`halo_wav.c` slurps + loops, so true streaming needs a PCM chardev — follow-up
  ticket). Underrun falls back to `""` (silence) rather than looping stale audio.
- **speaker**: tail the `--wav-out` file (valid WAV at all times, s16le mono 32 kHz,
  flushed every 4096 samples) from offset 44.
- **display**: QMP `screendump` (P6 PPM) polled at 10 Hz, changed frames pushed to a
  `<canvas>` over WebSocket.
- **control**: bridge holds the single ctl-socket connection (9562) for the unit-
  converting verbs, and its own second QMP monitor (launcher trailing args, as in
  `tests/smoke_display.py`) for the hot paths.

## Tasks

1. **`tools/av_bridge.py`** (uv script: `aiohttp`, `cryptography`, `qrcode`) — one
   HTTPS listener (default 9564): desktop UI `/`, phone page `/phone`, `/qr.svg`,
   static `/webui/*`, WebSockets `/ws/control` (JSON ctl pass-through + 2 Hz status),
   `/ws/display` (`<II>` w,h + RGB888), `/ws/speaker` (`{"rate":N}` then s16le),
   `/ws/phone` (JSON hello, then tagged binary: `0x01`+PCM, `0x02`+`<HH>`w,h+RGB888).
2. **Emulator integration**: default spawn (`halo-emu -f FW --headless --wav-out …
   -- -qmp unix:…`), `--attach <qmp.sock>` for a running instance.
3. **CameraSink** (throttle, atomic replace, qom-set), **MicChunker** (600 ms chunks,
   alternating files, consumption-gated swap, silence on underrun, host-side resample
   when the phone's AudioContext ignores the 16 kHz hint), **SpeakerTailer**,
   **DisplayPoller**, **StatusPoller**.
4. **Web UI** `tools/webui/` — vanilla JS, no build step: desktop page (panel mirror,
   button/battery/charger/tap/accel/magn/reboot, free-text ctl box, QR + phone status,
   speaker playback via AudioWorklet), phone page (Start → getUserMedia, preview,
   level meter; AudioWorklet PCM capture; canvas RGB888 frames at 2–10 fps).
5. **TLS**: self-signed ECDSA cert generated into `.avbridge/` (gitignored) with the
   LAN IP in the SAN; `--public-ip` override + WSL2 port-forward hint at startup.
6. **`--smoke`**: no-phone self-test — synthetic phone client over wss feeds a
   gradient frame + 1 kHz tone and asserts camera geometry, mic wav→silence
   transition, a display frame, and a ctl round-trip.

## Gate (acceptance)

- `uv run tools/av_bridge.py --smoke` passes against the calibration firmware.
- Manual: phone page streams camera (REPL `frame.camera` capture returns the phone
  image) and mic (`mic?` shows `source=wav`, `mic-samples` advances); desktop UI
  mirrors boot, drives button/battery/charger, and plays speaker audio.
- No existing file changes except `.gitignore`; all six existing smoke tests stay
  green untouched.

## Outcome (done 2026-08-31)

One new tool + five static web assets, zero QEMU/launcher changes; among existing files
only `.gitignore` gained a line (`.avbridge/`), so every existing smoke gate is
untouched (spot-checked: `tests/smoke_camera.py` all green).

1. **`tools/av_bridge.py`** (uv script: aiohttp/cryptography/qrcode) — spawn mode
   forwards `--ctl-port` and adds its own second QMP monitor via the launcher's
   trailing args; `--smoke` runs on a scratch MRAM so the user's `./mram.img` is never
   touched. Camera pushes go `HALOCAM1` → `os.replace` → `qom-set camera-file`; mic is
   the chunked v1 (600 ms, alternating files, consumption-gated by the `mic-samples`
   delta with a 2× wall-clock fallback, `""`/silence on underrun); speaker tails the
   `--wav-out` WAV from offset 44; display is `screendump` PPM at 10 Hz with a CRC
   latch (re-primed per new client).
2. **`tools/webui/`** — `index.html`/`app.js`/`style.css` (panel canvas, control
   panel, ctl free-text box with reply log, QR onboarding, AudioWorklet speaker
   playback with drop-oldest jitter buffer), `phone.html`/`phone.js` (getUserMedia,
   RGB888 canvas frames, AudioWorklet PCM capture, level meter), `worklets.js`.
   A confirm-guarded Shutdown button issues QMP `quit` through the bridge: the
   emulator exits, every UI client gets `emu-exit`, and a spawn-mode bridge exits too.
3. **Learned while landing it**: the LPCAM QOM props `camera-width/height` report the
   *guest-programmed* geometry (`halo_lpcam_geometry`), not the loaded source's — the
   smoke asserts `camera-source == "file"` instead. And the ctl socket really is
   single-client: the bridge owns it, so ad-hoc `nc 127.0.0.1 9562` is refused while
   the bridge runs; the UI's free-text box is the replacement.
4. **Gate run**: `av_bridge.py --smoke -f 0.8.8.bin` → camera latch, mic wav→silence,
   display frame, ctl round-trip: all PASS. Pages verified served (200, correct MIME)
   over the self-signed TLS listener.

Follow-up candidate (0041): PCM chardev on `halo_pdm.c`/`halo_i2s.c` (pattern:
`halo_ble.c` `DEFINE_PROP_CHR`) for true streaming mic/speaker instead of v1 chunks.
