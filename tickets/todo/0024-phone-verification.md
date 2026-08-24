# 0024 — Phone verification ladder (nRF Connect → test suite → Brilliant app)

**Phase:** 2 — real BLE
**Depends on:** 0021, 0022, 0023
**Effort:** M (mostly testing/fixing)

## Story

Prove "the phone sees the real thing". Climb from raw GATT inspection to the actual
Brilliant Labs iOS app, fixing gaps as they surface. This ticket also decides whether
the descoped services (ANCS, LE Audio) must move back into scope.

## Tasks

1. **nRF Connect (Android first, iOS second):** scan-record byte-diff vs a real Halo;
   GATT database dump diff; pair/bond/reconnect cycles; MTU 247; notification
   subscribe on all notify chars.
2. **Python suite over real BLE:** `applications/halo/tests/run_tests.py` with stock
   `brilliant-ble` (no pyshim) against the emulator — full phase-1-green set. Compare
   BLE throughput numbers to hardware for reference (not a gate).
3. **OTA:** `ota_flash.py` + `verify.py` ladder from 0023.
4. **Brilliant Labs app (iOS):** discovery filter match, REPL traffic, battery display,
   OTA screen. Watch specifically for:
   - iOS GATT cache staleness after service-layout iterations (toggle BT / re-pair;
     consider Service Changed indication support)
   - hard dependency on ANCS or LE-Audio presence — if the app refuses a device
     without ASCS/PACS or ANCS solicitation, escalate: ANCS moves from optional
     phase 2.5 (clean-room GATT client, ~800–1200 lines) into scope, and LE Audio
     gets GATT-presence-only stubs as a last resort
5. Record every deviation + fix in `EMULATOR.md`; file follow-up tickets for gaps.

## Key points in code

- `applications/halo/BLE_SERVICES.md`, `PROTOCOL.md`, `PAIRING.md` — the specs being
  proven
- Known iOS quirks: RPA phone vs privacy-off peripheral (same as real hardware —
  works today); re-pair after Forget Device (0022 verify item)

## Acceptance criteria

- [ ] GATT/advertising diff vs real Halo: empty (minus documented descopes)
- [ ] Python suite green over real BLE
- [ ] Brilliant app connects, REPL + battery + OTA screens function
- [ ] Descope decision (ANCS/LE Audio) confirmed or escalated with follow-up tickets
