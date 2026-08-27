# 0039 — LE Audio: the isochronous data path and streaming

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0038 (the ASE state machine gates the data path)
**Effort:** L

## Story

0038 walks an ASE to STREAMING, but no audio moves: `gapi_isooshm_dp_*` was still stubbed,
so `iso_datapath_ctoh_init()` failed and the firmware logged "Failed to create ISO
datapath" / "Failed to create sink channel" and refused the enable. This ticket carries
LC3 SDUs in both directions.

The contract is small and lives firmware-side (`alif/modules/halo/src/iso_datapath_{ctoh,
htoc}.c`): `dp_init(dp, cb)` registers a completion callback, `dp_bind(dp, stream_lid,
dir)`, `dp_set_buf(dp, buf)` hands over the next SDU buffer, and the ROM invokes `cb` when
that buffer has been filled (OUTPUT) or transmitted (INPUT); `dp_get_sync` supplies
presentation timing. Direction is named from the controller's point of view, so INPUT is
the firmware *sending*.

## Tasks

1. `rom-stub/src/stub_iso.c` — implement `gapi_isooshm_dp_{init,bind,unbind,set_buf,
   get_sync,get_local_time}`. INPUT completes synchronously: the SDU goes out as
   `EVT_ISO_SDU` and the buffer is released immediately. OUTPUT parks the buffer until the
   host injects `OP_ISO_SDU`. Vendor `gapi_isooshm.h`.
2. Host side: SDU capture and injection in `tools/ble_bridge.py`, `iso?` / `iso replay` /
   `iso clear` on the control socket. Replay must pace one SDU per interval — the guest
   posts a single buffer at a time, so a burst would drop all but the first.
3. `tests/smoke_le_audio.py` — a full round trip: the firmware encodes microphone audio to
   LC3 on a source ASE, and those SDUs replayed into a sink ASE decode and reach the
   speaker.

## Gate

- The firmware streams LC3 SDUs of the configured frame size on a source ASE.
- SDUs injected into a sink ASE are decoded and reach the speaker.
- The guest survives it, and the other smoke tests still pass.

## Outcome (done 2026-08-26)

1. Implemented as above.
2. **liblc3 had to be built `-DLC3_PLUS=0 -DLC3_PLUS_HR=0`** (`rom-stub/Makefile`). This
   was the hard part and is worth recording:
   - Both default to 1 in liblc3, which sets `LC3_MAX_SRATE_HZ` to 96000 and sizes every
     work buffer for 960-sample frames (`deps/liblc3/src/tables.h:68-70`). Those buffers
     are stack.
   - `lc3_decode()` then overflowed the firmware's **3072-byte** `ble_audio_dec` thread
     (`CONFIG_LC3_DECODER_STACK_SIZE`, `alif/subsys/bluetooth/le_audio/Kconfig.lc3:27-31`)
     on the very first received SDU and **halted the guest**: "ZEPHYR FATAL ERROR 2: Stack
     overflow". The encoder thread gets 6144 bytes, which is why the source direction
     streamed cleanly while the sink direction crashed — a useful asymmetry when
     diagnosing.
   - The firmware only ever asks for standard LC3 at 8-48 kHz (BAP advertises at most
     48 kHz, and this build 16 kHz), so building for that profile is not a workaround but
     the correct configuration; it brings the decode inside the budget the firmware sizes
     for the real ROM.
   - **Rejected alternative:** running the decode on a private stack inside the stub. It
     fails at the `mov sp` itself — Zephyr's MPU-based stack protection reports "Stack
     overflow (context area not valid)" as soon as SP leaves the thread stack. Do not
     retry this; shrink the stack demand instead.
3. **Gate evidence** (`tests/smoke_le_audio.py`, 20/20 green):
   - source ASE streaming: 40-octet SDUs (the configured BAP 16_2 size) at the 10 ms
     interval, byte accounting consistent;
   - 100 replayed SDUs decode and reach the speaker: `speaker?` goes from
     `samples=14539` to `samples=100811`, `playing=1`;
   - no `ZEPHYR FATAL ERROR` and the REPL still answers afterwards;
   - both ASEs release back to IDLE.
   - Full suite green: `smoke_audio` (its own LC3 round trip still passes on the
     re-configured liblc3), `smoke_ble`, `smoke_display`, `smoke_controls`,
     `smoke_sensors`, `smoke_le_audio`; 0 orphaned processes.

## Not done

- No CIS/CIG modelling: `cb_cis_state` is never invoked, because there is no link layer to
  establish a CIS on. The host's `ase start` stands in for "CIS established".
- SDU pacing is host-driven rather than clock-driven: `gapi_isooshm_dp_get_local_time()`
  returns a counter that advances one SDU interval per exchanged SDU. It is monotonic and
  reproducible, which is what the firmware's presentation-delay arithmetic needs, but it is
  not a real clock.
- Only the sink path was exercised with round-tripped frames (the firmware's own LC3), as
  the host has no LC3 encoder. Injecting externally-encoded LC3 would need one.
