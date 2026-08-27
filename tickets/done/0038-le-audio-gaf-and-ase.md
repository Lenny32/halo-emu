# 0038 — LE Audio: the GAF profile layer and the ASE state machine

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0028 (ROM stub), 0032 (LC3 glue)
**Effort:** XL

## Story

Ticket 0028 scoped the whole Generic Audio Framework as the "declared-unsupported tier":
`stub_misc.c` answered every BAP/CAP/TMAP/ARC entry point with `0xFF`, so
`halo_ble_audio_init()` aborted at its first step and every boot logged

```
<err> halo_ble_audio: Unable to configure BAP unicast server! Error 255 (0xFF)
<wrn> halo_ble: Failed to initialize LE Audio service: -1 (continuing)
```

That was a deliberate decision, not a defect — the product's own audio path (LC3 over the
proprietary Lua GATT service, ticket 0032) does not need it. This ticket implements the
profile layer for real, so standards-based LE Audio works in the emulator.

**The hard constraint:** there is no HCI and no ES0 controller here — the ROM *is* the stub
(`EMULATOR.md`), so nothing can be hooked in at the controller level and no central can
discover the device over the air. The peer has to be fabricated host-side.

## Tasks

1. Vendor the GAF headers (declarations only, as `rom-stub/README.md:52-53` already does
   for the rest of the Alif API): `ip/gaf/{gaf,gaf_cfg}.h`, `bap/`, `cap/`, `tmap/`,
   `arc/`. Their dependencies (`gap.h`, `gapi.h`, `gapm_le.h`, `prf_types.h`) are already
   vendored. Add the include paths to `rom-stub/Makefile`.
2. `rom-stub/src/stub_gaf.c` — implement the eight configure steps
   `halo_ble_audio_init()` walks (`ble_audio.c:2299-2620`): `bap_uc_srv_configure` +
   `is_configured`, `tmap_tmas_configure`, `bap_capa_srv_configure` + `set_record` +
   `remove_record` + `is_configured`, `arc_vcs_configure`, `cap_configure`,
   `arc_aics_configure` + `add`, `arc_mics_configure`. Keep the caller's callback tables
   and PAC records by pointer — the API docs make the upper layer their owner.
3. `rom-stub/src/stub_ase.c` — the ASE state machine
   (IDLE → CODEC_CONFIGURED → QOS_CONFIGURED → ENABLING → STREAMING → DISABLING →
   RELEASING → IDLE, `bap_uc.h:124-141`), driving the firmware's `cb_configure_codec_req`,
   `cb_configure_qos_req`, `cb_enable_req`, `cb_release_req`, `cb_dp_update_req` and
   `cb_ase_state` (registered at `ble_audio.c:2102-2115`), with the transition applied
   inside the matching `bap_uc_srv_*_cfm()` so a refusal stops it as on hardware.
4. New doorbell ops/events (`halo_rom_ipc.h`, extending the contiguous op space, not
   renumbering it): `OP_ASE_CODEC/QOS/ENABLE/START/DISABLE/RELEASE/DP` and
   `EVT_ASE_STATE`, dispatched from `rwip_process` so callbacks run on the BLE task.
5. Host side: the fabricated central in `tools/ble_bridge.py` (`ase_*` methods, ASE-state
   tracking) and `ase` verbs on the control socket, which reach the bridge rather than QMP.

## Gate

- `halo_ble_audio_init()` completes: the `Error 255` / `Failed to initialize LE Audio
  service` pair is gone from the boot log and nothing traps.
- An ASE walks to STREAMING under host control, every transition observable.
- The existing smoke tests still pass.

## Outcome (done 2026-08-26)

1. Implemented as above. `stub_misc.c` is **deleted**: with the GAF tier implemented here
   and the data path in 0039, nothing was left in the "declared unsupported" file.
2. Details worth keeping:
   - `arc_aics_set_gain/_gain_mode/_status` are called by `ble_audio.c` but are **not**
     pinned ROM symbols, so the firmware links its own — implementing them would be wrong.
     `gen_rom_layout.py` rejects any `hstub_` with no pinned address, which caught it.
   - The PAC record table must hold 64 entries (`MAX_PACS_RECORDS`, `ble_audio.c:194`);
     an 8-entry table failed init at source record 4 with `GAF_ERR_INSUFFICIENT_RESOURCES`.
   - `stub_ipc_send()` does not ring the doorbell — every sender calls `stub_ipc_kick()`
     after it (see `stub_gapc.c:68-69`). Without the kick the state machine ran correctly
     but the host never saw an `EVT_ASE_STATE`, which looked like a dead state machine.
   - The codec configuration must fall inside what the firmware advertises:
     `frame_octet_within_capa()` (`ble_audio.c:2204-2236`) accepts only the entries of
     `sink_capas`/`source_capas`, so the host asks for BAP 16_2 (16 kHz, 10 ms, 40 octets),
     the one configuration valid for both directions in this build.
3. **Gate evidence:** boot log clean (no BAP error, no warning, no trap); ASE 2 walks
   idle → codec-configured → qos-configured → enabling → streaming and back to idle under
   `ase` verbs; `tests/smoke_le_audio.py` covers it (see 0039 for the streaming half);
   `smoke_ble`, `smoke_display`, `smoke_controls`, `smoke_sensors`, `smoke_audio` pass.
