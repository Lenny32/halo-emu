/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr-context half of the TCP Lua transport (ticket 0005): the emulator's
 * provider of the <halo/ble_lua.h> symbol set (see <halo/lua_transport.h>
 * for the contract). Deliberately a structural copy of the hardware
 * provider, modules/halo/src/ble_lua.c — same ring buffer + semaphore
 * machinery, same first-byte RX demux, same chunk-to-MTU write loop — with
 * the GATT callbacks replaced by a pump thread draining the host-side
 * socket (transport_tcp_bottom.c).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include <halo/lua_transport.h>
#include <halo/lua_runtime.h>

#include "lua.h"
#include "nsi_cpu_if.h" /* NATIVE_SIMULATOR_IF */

#include "transport_tcp_bottom.h"

LOG_MODULE_REGISTER(emu_transport, CONFIG_HALO_LOG_LEVEL);

#define TCP_LUA_INIT_MAGIC 0x454D5554 /* 'EMUT' */

/* How long the pump waits for ring space before dropping a PDU — hardware
 * drops immediately (ATT_ERR_INSUFF_RESOURCE); TCP lets us absorb short
 * consumer stalls first, but a wedged consumer must not block control
 * codes arriving behind the stuck frame forever. */
#define RX_RING_WAIT_MS 100

/* Pump must outrank the REPL/data threads (CONFIG_HALO_LUA_*_TASK_PRIORITY
 * = 7) so 0x03 interrupts land while a Lua script busy-loops. */
#define PUMP_THREAD_PRIO       6
#define PUMP_THREAD_STACK_SIZE 4096

static struct {
	uint32_t initialized;

	/* REPL / data / audio RX rings, exactly as in ble_lua.c */
	struct ring_buf repl_rx_ring;
	struct k_sem repl_rx_sem;
	struct ring_buf data_rx_ring;
	struct k_sem data_rx_sem;
	struct ring_buf audio_rx_ring;
	struct k_sem audio_rx_sem;

	struct k_mutex write_lock;

	bool connected;
	halo_ble_lua_ctrl_handler_t ctrl_handler;
} tcp_ctx;

static uint8_t repl_rx_buf[CONFIG_HALO_LUA_MAX_REPL_SIZE + 1];
static uint8_t data_rx_buf[CONFIG_HALO_LUA_MAX_DATA_SIZE + 1];
static uint8_t audio_rx_buf[CONFIG_HALO_AUDIO_MAX_DATA_SIZE + 1];

/* Pump-thread frame scratch (single consumer of the bottom's mailbox) */
static uint8_t frame_buf[CONFIG_HALO_EMU_MTU];

/* --- ISR stand-in for control codes --------------------------------- *
 * On hardware a control code arrives in the BLE stack's context and can
 * always preempt a busy Lua script. On native_sim a CPU-bound Lua loop
 * freezes simulated time, so no Zephyr thread (the pump included) would
 * ever run to deliver it. The bottom's HOST thread therefore calls
 * emu_tcp_ctrl_notify() when it parses a control PDU: it installs a Lua
 * hook asynchronously (the one lua_sethook use-case the Lua manual
 * sanctions from signal handlers / other threads). The hook body then
 * runs in the REPL thread and k_sleep()s — idling the simulated CPU so
 * the pump can dispatch the control code through the normal path. */

static int ctrl_pending;

#ifdef CONFIG_HALO_LUA_RUNTIME
static void ctrl_wait_hook(lua_State *L, lua_Debug *ar)
{
	ARG_UNUSED(ar);

	for (int waited_ms = 0;
	     __atomic_load_n(&ctrl_pending, __ATOMIC_SEQ_CST) > 0 && waited_ms < 2000;
	     waited_ms++) {
		k_sleep(K_MSEC(1));
	}

	/* Uninstall only if the ctrl handler did not replace the hook with
	 * its own (e.g. the interrupt/restart break hook) in the meantime. */
	if (lua_gethook(L) == ctrl_wait_hook) {
		lua_sethook(L, NULL, 0, 0);
	}
}
#endif /* CONFIG_HALO_LUA_RUNTIME */

/* NATIVE_SIMULATOR_IF: called by the runner-side bottom, so this symbol
 * must survive the zephyr.elf symbol localization step. */
NATIVE_SIMULATOR_IF void emu_tcp_ctrl_notify(void)
{
	/* HOST-THREAD CONTEXT: atomics and lua_sethook only. */
	__atomic_add_fetch(&ctrl_pending, 1, __ATOMIC_SEQ_CST);

#ifdef CONFIG_HALO_LUA_RUNTIME
	lua_State *L = halo_lua_get_state();

	if (L != NULL) {
		lua_sethook(L, ctrl_wait_hook,
			    LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT, 1);
	}
#endif
}

bool emu_transport_is_connected(void)
{
	return tcp_ctx.initialized == TCP_LUA_INIT_MAGIC && emu_tcp_bottom_is_connected();
}

int emu_transport_disconnect(void)
{
	emu_tcp_bottom_disconnect();
	return 0;
}

/* Store one inbound PDU, waiting briefly for ring space (see RX_RING_WAIT_MS).
 * add_newline mirrors the hardware REPL path, which appends '\n' per PDU. */
static void ring_put_pdu(struct ring_buf *ring, struct k_sem *sem, const uint8_t *data,
			 uint32_t len, bool add_newline)
{
	uint32_t need = len + (add_newline ? 1 : 0);
	int waited_ms = 0;

	while (ring_buf_space_get(ring) < need) {
		if (!emu_tcp_bottom_is_connected() || waited_ms >= RX_RING_WAIT_MS) {
			LOG_WRN("RX ring full (available=%u, needed=%u) — dropping PDU",
				ring_buf_space_get(ring), need);
			return;
		}
		k_sleep(K_MSEC(1));
		waited_ms++;
	}

	ring_buf_put(ring, data, len);
	if (add_newline) {
		uint8_t newline = '\n';

		ring_buf_put(ring, &newline, 1);
	}
	k_sem_give(sem);
}

/* First-byte demux of a Lua-channel PDU — replica of the RX_VAL case in
 * ble_lua.c's on_att_val_set() (there is no write continuation over TCP:
 * one frame is always one whole PDU). */
static void route_lua_pdu(const uint8_t *data, uint32_t len)
{
	if (len == 0) {
		return;
	}

	if (data[0] >= HALO_LUA_CTRL_REBOOT && data[0] <= HALO_LUA_CTRL_REMOVE_ALL) {
		ring_buf_reset(&tcp_ctx.repl_rx_ring);
		ring_buf_reset(&tcp_ctx.data_rx_ring);
		if (tcp_ctx.ctrl_handler) {
			tcp_ctx.ctrl_handler(data[0]);
		} else {
			LOG_WRN("Control code 0x%02X with no handler registered", data[0]);
		}
		/* Matches emu_tcp_ctrl_notify()'s increment for this PDU;
		 * releases a ctrl_wait_hook parked in the REPL thread. */
		__atomic_sub_fetch(&ctrl_pending, 1, __ATOMIC_SEQ_CST);
		return;
	}

	if (data[0] == HALO_LUA_CTRL_DATA_MARKER && len >= 2) {
		ring_put_pdu(&tcp_ctx.data_rx_ring, &tcp_ctx.data_rx_sem, data + 1, len - 1,
			     false);
	} else {
		ring_put_pdu(&tcp_ctx.repl_rx_ring, &tcp_ctx.repl_rx_sem, data, len, true);
	}
}

static void on_disconnected(void)
{
	/* Mirror ble_lua_on_disconnected(): stale input dies with the
	 * connection, blocked writers get released. */
	ring_buf_reset(&tcp_ctx.repl_rx_ring);
	ring_buf_reset(&tcp_ctx.data_rx_ring);
	ring_buf_reset(&tcp_ctx.audio_rx_ring);
}

static void pump_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (tcp_ctx.initialized != TCP_LUA_INIT_MAGIC) {
		k_sleep(K_MSEC(10));
	}

	for (;;) {
		bool conn = emu_tcp_bottom_is_connected();

		if (conn != tcp_ctx.connected) {
			tcp_ctx.connected = conn;
			LOG_INF("Client %s", conn ? "connected" : "disconnected");
			if (!conn) {
				on_disconnected();
			}
			emu_ble_stubs_conn_event(conn);
		}

		uint8_t channel;
		int len = emu_tcp_bottom_rx_peek(&channel);

		if (len < 0) {
			k_sleep(K_MSEC(1));
			continue;
		}

		len = emu_tcp_bottom_rx_pop(frame_buf, sizeof(frame_buf));
		if (len < 0) {
			continue;
		}

		switch (channel) {
		case EMU_TCP_CH_LUA:
			route_lua_pdu(frame_buf, (uint32_t)len);
			break;
		case EMU_TCP_CH_AUDIO:
			ring_put_pdu(&tcp_ctx.audio_rx_ring, &tcp_ctx.audio_rx_sem, frame_buf,
				     (uint32_t)len, false);
			break;
		case EMU_TCP_CH_VIDEO:
		default:
			/* Bottom validates channels; video is TX-only */
			LOG_WRN("Dropping %d bytes on non-writable channel %u", len, channel);
			break;
		}
	}
}

K_THREAD_DEFINE(emu_tcp_pump, PUMP_THREAD_STACK_SIZE, pump_thread_fn, NULL, NULL, NULL,
		PUMP_THREAD_PRIO, 0, 0);

/* Shared PDU-write path: chunk to the emulated MTU exactly like ble_lua.c's
 * send_notification() chunks to the negotiated ATT payload; each chunk goes
 * out as one frame (= one notification PDU). */
static int32_t transport_tx(uint8_t channel, const uint8_t *data, size_t len)
{
	if (tcp_ctx.initialized != TCP_LUA_INIT_MAGIC) {
		return 0;
	}

	if (!emu_tcp_bottom_is_connected()) {
		/* Same shape as the hardware's "no subscriber" path */
		k_sleep(K_MSEC(10));
		return 0;
	}

	k_mutex_lock(&tcp_ctx.write_lock, K_FOREVER);

	const uint8_t *p_data = data;
	size_t remaining = len;
	int32_t sent = 0;

	while (remaining > 0) {
		uint32_t chunk_len =
			remaining > CONFIG_HALO_EMU_MTU ? CONFIG_HALO_EMU_MTU : remaining;
		int ret = emu_tcp_bottom_tx(channel, p_data, chunk_len);

		if (ret == -2) {
			k_mutex_unlock(&tcp_ctx.write_lock);
			return -EMSGSIZE;
		}
		if (ret != (int)chunk_len) {
			LOG_WRN("TX failed on channel %u (%d)", channel, ret);
			break;
		}

		p_data += chunk_len;
		remaining -= chunk_len;
		sent += chunk_len;
	}

	k_mutex_unlock(&tcp_ctx.write_lock);

	return sent;
}

/* Blocking ring read with the exact ble_lua.c semantics (0 on timeout,
 * semaphore re-armed while the ring still holds data). */
static int32_t ring_read(struct ring_buf *ring, struct k_sem *sem, uint8_t *data, size_t len,
			 k_timeout_t timeout)
{
	if (tcp_ctx.initialized != TCP_LUA_INIT_MAGIC) {
		return 0;
	}

	if (k_sem_take(sem, timeout) != 0) {
		return 0;
	}

	if (tcp_ctx.initialized != TCP_LUA_INIT_MAGIC) {
		return 0;
	}

	uint32_t read_len = ring_buf_get(ring, data, len);

	if (!ring_buf_is_empty(ring)) {
		k_sem_give(sem);
	}

	return read_len;
}

/* ------------------------------------------------------------------ */
/* Public API: the <halo/ble_lua.h> provider                           */
/* ------------------------------------------------------------------ */

int halo_ble_lua_init(bool reset)
{
	if (tcp_ctx.initialized == TCP_LUA_INIT_MAGIC) {
		return 0;
	}

	if (reset) {
		tcp_ctx.ctrl_handler = NULL;
	}

	ring_buf_init(&tcp_ctx.repl_rx_ring, sizeof(repl_rx_buf), repl_rx_buf);
	ring_buf_init(&tcp_ctx.data_rx_ring, sizeof(data_rx_buf), data_rx_buf);
	ring_buf_init(&tcp_ctx.audio_rx_ring, sizeof(audio_rx_buf), audio_rx_buf);
	k_sem_init(&tcp_ctx.repl_rx_sem, 0, 1);
	k_sem_init(&tcp_ctx.data_rx_sem, 0, 1);
	k_sem_init(&tcp_ctx.audio_rx_sem, 0, 1);
	k_mutex_init(&tcp_ctx.write_lock);
	tcp_ctx.connected = false;

	if (emu_tcp_bottom_start(CONFIG_HALO_EMU_TCP_PORT, CONFIG_HALO_EMU_MTU) != 0) {
		LOG_ERR("Failed to start TCP listener on port %d", CONFIG_HALO_EMU_TCP_PORT);
		return -EIO;
	}

	tcp_ctx.initialized = TCP_LUA_INIT_MAGIC;
	LOG_INF("Lua transport on tcp://127.0.0.1:%d (MTU %d)", CONFIG_HALO_EMU_TCP_PORT,
		CONFIG_HALO_EMU_MTU);

	return 0;
}

int halo_ble_lua_deinit(void)
{
	if (tcp_ctx.initialized != TCP_LUA_INIT_MAGIC) {
		return 0;
	}

	tcp_ctx.initialized = 0;
	emu_tcp_bottom_disconnect();

	/* Release blocked readers; they re-check initialized on wake */
	k_sem_give(&tcp_ctx.repl_rx_sem);
	k_sem_give(&tcp_ctx.data_rx_sem);
	k_sem_give(&tcp_ctx.audio_rx_sem);

	return 0;
}

int32_t halo_ble_lua_repl_read(uint8_t *data, size_t len, k_timeout_t timeout)
{
	return ring_read(&tcp_ctx.repl_rx_ring, &tcp_ctx.repl_rx_sem, data, len, timeout);
}

int32_t halo_ble_lua_repl_write(const uint8_t *data, size_t len)
{
	return transport_tx(EMU_TCP_CH_LUA, data, len);
}

int32_t halo_ble_lua_data_read(uint8_t *data, size_t len, k_timeout_t timeout)
{
	return ring_read(&tcp_ctx.data_rx_ring, &tcp_ctx.data_rx_sem, data, len, timeout);
}

int32_t halo_ble_lua_data_write(const uint8_t *data, size_t len)
{
	/* Hardware sends data (0x01-marked by the caller) as TX-characteristic
	 * notifications, i.e. the same channel as REPL output. */
	return transport_tx(EMU_TCP_CH_LUA, data, len);
}

int32_t halo_ble_lua_video_write(const uint8_t *data, size_t len)
{
	return transport_tx(EMU_TCP_CH_VIDEO, data, len);
}

int32_t halo_ble_lua_audio_read(uint8_t *data, size_t len, k_timeout_t timeout)
{
	return ring_read(&tcp_ctx.audio_rx_ring, &tcp_ctx.audio_rx_sem, data, len, timeout);
}

int32_t halo_ble_lua_audio_write(const uint8_t *data, size_t len)
{
	return transport_tx(EMU_TCP_CH_AUDIO, data, len);
}

void halo_ble_lua_register_ctrl_handler(halo_ble_lua_ctrl_handler_t handler)
{
	tcp_ctx.ctrl_handler = handler;
}

/* On hardware halo_ble_init() -> ble_manager -> halo_ble_lua_init(); with
 * CONFIG_HALO_BLE_MANAGER=n nobody makes that call chain, so the transport
 * brings itself up before main() runs (static buffers — no dependency on
 * halo_mem_init()). */
static int emu_transport_sys_init(void)
{
	return halo_ble_lua_init(true);
}

SYS_INIT(emu_transport_sys_init, APPLICATION, 90);
