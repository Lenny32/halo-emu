/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-side half of the TCP Lua transport (ticket 0005). This header is
 * shared by both build contexts, so it must stay Zephyr-free: the bottom
 * (transport_tcp_bottom.c) is compiled into the native_simulator runner
 * with the host libc and no Kconfig; the top (transport_tcp.c) passes any
 * configuration in through emu_tcp_bottom_start().
 *
 * Wire framing (documented in <halo/lua_transport.h>):
 *     [u8 channel][u16 little-endian length][payload]
 */

#ifndef EMU_TRANSPORT_TCP_BOTTOM_H
#define EMU_TRANSPORT_TCP_BOTTOM_H

#include <stdbool.h>
#include <stdint.h>

/* Frame channels */
#define EMU_TCP_CH_LUA   0x00 /* REPL text / 0x01-marked data / ctrl codes */
#define EMU_TCP_CH_AUDIO 0x01
#define EMU_TCP_CH_VIDEO 0x02 /* device -> host only */
#define EMU_TCP_CH_MAX   EMU_TCP_CH_VIDEO

#define EMU_TCP_FRAME_HDR_LEN 3

/**
 * Start the listener (loopback only) and its accept/read pthread.
 * @param max_payload largest accepted/sent frame payload (the emulated MTU)
 * @return 0 on success (idempotent), negative on socket/thread failure
 */
int emu_tcp_bottom_start(uint16_t port, uint32_t max_payload);

/** True while a client socket is attached. */
bool emu_tcp_bottom_is_connected(void);

/**
 * Peek the pending inbound frame, if any.
 * @return payload length (>= 0) with *channel set, or -1 if none pending
 */
int emu_tcp_bottom_rx_peek(uint8_t *channel);

/**
 * Consume the pending inbound frame into buf (unblocks the reader thread).
 * @return payload length, or -1 if none pending / buf too small
 */
int emu_tcp_bottom_rx_pop(uint8_t *buf, uint32_t maxlen);

/**
 * Send one framed PDU. Never merges or splits payloads (one call = one
 * frame = one PDU); the Zephyr side chunks to the MTU before calling.
 * @return len on success, -1 when disconnected or on send failure/timeout,
 *         -2 if len exceeds the max_payload given to start() (the caller
 *         maps this to -EMSGSIZE)
 */
int emu_tcp_bottom_tx(uint8_t channel, const uint8_t *data, uint32_t len);

/** Ask the transport thread to drop the current client (async, idempotent). */
void emu_tcp_bottom_disconnect(void);

/**
 * Implemented by the Zephyr side (transport_tcp.c), CALLED BY THE BOTTOM'S
 * HOST THREAD when a control-code PDU (first byte 0x02..0x07 on the Lua
 * channel) is parsed off the wire. This is the emulator's stand-in for the
 * BLE ISR: on native_sim a busy Lua loop freezes simulated time, so no
 * Zephyr thread could ever deliver the control code — the host thread
 * asynchronously installs a Lua hook (lua_sethook is async-safe by design)
 * that yields the CPU until the pump thread has dispatched it. Must only
 * do async-safe work (atomics + lua_sethook), no Zephyr calls.
 */
void emu_tcp_ctrl_notify(void);

#endif /* EMU_TRANSPORT_TCP_BOTTOM_H */
