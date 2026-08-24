/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE-manager surface for CONFIG_HALO_BLE_MANAGER=n (ticket 0005): the
 * halo_ble_* symbols that lua_bluetooth.c, lua_button.c, main.c and future
 * audio tickets link against when the Alif BLE stack is compiled out.
 * Connection state comes from the TCP transport (transport_tcp.c); identity
 * is fixed so tests can assert on it; security is a paired-by-wire no-op.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/dlist.h>

#include <halo/ble_connection.h>
#include <halo/ble_manager.h>
#include <halo/ble_security.h>
#include <halo/lua_transport.h>

LOG_MODULE_REGISTER(emu_ble_stubs, CONFIG_HALO_LOG_LEVEL);

/* EUI-48 derived from se_stubs.c's fixed EUI-64 (OUI 2C:F7:F1 + extension
 * 00:00:01) in BLE little-endian order, so frame.bluetooth.address()
 * prints "2C:F7:F1:00:00:01" — a stable identity for the "Halo XX" name
 * convention (last address byte -> "Halo 01"). */
static const uint8_t emu_ble_addr[6] = {0x01, 0x00, 0x00, 0xF1, 0xF7, 0x2C};

static sys_dlist_t cb_list = SYS_DLIST_STATIC_INIT(&cb_list);
static K_MUTEX_DEFINE(cb_lock);

int halo_ble_init(const char *device_name)
{
	ARG_UNUSED(device_name);
	/* The TCP listener normally self-starts via SYS_INIT (nothing calls
	 * halo_ble_init() with CONFIG_HALO_BLE_MANAGER=n); idempotent. */
#ifdef CONFIG_HALO_TRANSPORT_TCP
	return halo_ble_lua_init(true);
#else
	return 0;
#endif
}

bool halo_ble_is_connected(void)
{
#ifdef CONFIG_HALO_TRANSPORT_TCP
	return emu_transport_is_connected();
#else
	return false;
#endif
}

uint16_t halo_ble_get_mtu(void)
{
	/* CONFIG_HALO_EMU_MTU (512) when connected, 0 otherwise — so
	 * frame.bluetooth.max_length() = 511, matching PROTOCOL.md. */
#ifdef CONFIG_HALO_TRANSPORT_TCP
	return emu_transport_is_connected() ? CONFIG_HALO_EMU_MTU : 0;
#else
	return 0;
#endif
}

int halo_ble_get_address(uint8_t addr[6])
{
	if (addr == NULL) {
		return -EINVAL;
	}
	memcpy(addr, emu_ble_addr, sizeof(emu_ble_addr));
	return 0;
}

int halo_ble_conn_disconnect(void)
{
#ifdef CONFIG_HALO_TRANSPORT_TCP
	return emu_transport_disconnect();
#else
	return 0;
#endif
}

void halo_ble_conn_prepare_reboot(void)
{
	/* Hardware gives the link a clean shutdown before sys_reboot();
	 * dropping the socket is the TCP equivalent. */
#ifdef CONFIG_HALO_TRANSPORT_TCP
	(void)emu_transport_disconnect();
#endif
}

int halo_ble_sec_pairing_window_open(void)
{
	/* lua_button.c's pairing gesture: there is no pairing over loopback
	 * TCP — succeed so the gesture flow completes (real BLE: phase 2). */
	LOG_INF("Pairing window requested — no-op on the TCP transport");
	return 0;
}

int halo_ble_register_callback(struct halo_ble_callback *cb, halo_ble_event_cb_t callback,
			       uint32_t event_mask, void *user_data, const char *name,
			       int priority)
{
	if (cb == NULL || callback == NULL) {
		return -EINVAL;
	}

	cb->callback = callback;
	cb->event_mask = event_mask;
	cb->user_data = user_data;
	cb->name = name;
	cb->priority = priority;

	k_mutex_lock(&cb_lock, K_FOREVER);

	/* Highest priority first, like ble_manager.c */
	struct halo_ble_callback *item;
	bool inserted = false;

	SYS_DLIST_FOR_EACH_CONTAINER(&cb_list, item, node) {
		if (priority > item->priority) {
			sys_dlist_insert(&item->node, &cb->node);
			inserted = true;
			break;
		}
	}
	if (!inserted) {
		sys_dlist_append(&cb_list, &cb->node);
	}

	k_mutex_unlock(&cb_lock);
	return 0;
}

int halo_ble_unregister_callback(struct halo_ble_callback *cb)
{
	if (cb == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&cb_lock, K_FOREVER);
	sys_dlist_remove(&cb->node);
	k_mutex_unlock(&cb_lock);
	return 0;
}

void emu_ble_stubs_conn_event(bool connected)
{
	struct halo_ble_event_data event = {
		.event = connected ? HALO_BLE_EVENT_CONNECTED : HALO_BLE_EVENT_DISCONNECTED,
		.conidx = 0,
	};
	uint32_t event_bit = HALO_BLE_EVENT_MASK(event.event);
	struct halo_ble_callback *cb;

	k_mutex_lock(&cb_lock, K_FOREVER);
	SYS_DLIST_FOR_EACH_CONTAINER(&cb_list, cb, node) {
		if ((cb->event_mask & event_bit) == 0) {
			continue;
		}
		if (cb->callback(&event, cb->user_data) != 0) {
			LOG_WRN("BLE callback '%s' failed", cb->name ? cb->name : "?");
		}
	}
	k_mutex_unlock(&cb_lock);
}
