/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_EMU_LUA_TRANSPORT_H
#define HALO_EMU_LUA_TRANSPORT_H

#include <stdbool.h>

#include <halo/ble_lua.h>

/**
 * @file lua_transport.h
 * @brief The Lua transport seam (ticket 0005)
 *
 * The Lua runtime does not talk to "BLE" — it talks to the symbol set
 * declared in <halo/ble_lua.h>, and exactly ONE provider of that set is
 * linked into any image (link-time polymorphism, no vtable, no Kconfig
 * indirection in the callers):
 *
 *   - hardware:  modules/halo/src/ble_lua.c        (Alif ROM GATT service,
 *                CONFIG_HALO_BLE_MANAGER)
 *   - emulator:  emulator/module/src/transport_tcp.c (this module,
 *                CONFIG_HALO_TRANSPORT_TCP)
 *   - phase 2:   a Zephyr-BT backend (tickets 0018+)
 *
 * Any future backend must honour the contract below — it is what
 * lua_runtime.c, lua_bluetooth.c and luaport.h (lua_writestring ->
 * halo_ble_lua_repl_write) are written against.
 *
 * ## The contract (semantics of <halo/ble_lua.h>)
 *
 * - PDU-oriented: the unit of transfer is one GATT-write/notification
 *   payload of at most MTU bytes. Reads/writes never merge PDUs across
 *   the wire; large writes are CHUNKED to MTU-sized PDUs by the provider
 *   (mirroring ble_lua.c's send_notification()).
 * - Blocking reads: *_read(data, len, timeout) blocks up to a k_timeout_t,
 *   returns bytes read, 0 on timeout, negative on error.
 * - First-byte demux of inbound Lua-channel PDUs (ble_lua.c:241..312):
 *     0x01         -> data channel (marker byte stripped)
 *     0x02..0x07   -> control codes: both RX rings are cleared, then the
 *                     registered ctrl handler runs (0x02 reboot, 0x03
 *                     interrupt, 0x04 restart VM, 0x05 reset, 0x06 exit,
 *                     0x07 remove-all — see LUA_RUNTIME.md)
 *     anything else-> REPL text; the provider appends '\n' per PDU
 * - One client at a time (BLE has a single connection; extra TCP clients
 *   are refused at accept()).
 * - Disconnect clears all RX rings and releases blocked readers/writers.
 * - Writes while disconnected return 0 (not an error), like an unsent
 *   notification.
 *
 * ## TCP wire framing (emulator backend only)
 *
 * GATT gave PDU boundaries for free; TCP is a byte stream, so each PDU is
 * framed as
 *
 *     [u8 channel][u16 little-endian length][payload]
 *
 * on port CONFIG_HALO_EMU_TCP_PORT (default 9563, loopback only).
 * Channels: 0 = Lua RX/TX (payload = the exact GATT PDU, demuxed as
 * above), 1 = audio, 2 = video (device->host only). One frame = one PDU;
 * a frame longer than CONFIG_HALO_EMU_MTU or with an unknown channel is a
 * protocol violation and drops the connection (a GATT write can never
 * exceed the negotiated MTU — hardware honesty).
 *
 * PROTOCOL.md stays the payload authority: with CONFIG_HALO_EMU_MTU=512,
 * frame.bluetooth.max_length() = 511, matching the hardware MTU table.
 */

/**
 * @brief Whether the transport currently has a client attached
 *
 * Backs halo_ble_is_connected() in ble_stubs.c.
 */
bool emu_transport_is_connected(void);

/**
 * @brief Drop the current client (if any)
 *
 * Backs halo_ble_conn_disconnect() / halo_ble_conn_prepare_reboot().
 * The listener stays up; the client may reconnect.
 *
 * @return 0 (idempotent)
 */
int emu_transport_disconnect(void);

/**
 * @brief Connection-state edge, fired by the transport's pump thread
 *
 * Implemented by ble_stubs.c: dispatches HALO_BLE_EVENT_CONNECTED /
 * HALO_BLE_EVENT_DISCONNECTED to callbacks registered through
 * halo_ble_register_callback().
 */
void emu_ble_stubs_conn_event(bool connected);

#endif /* HALO_EMU_LUA_TRANSPORT_H */
