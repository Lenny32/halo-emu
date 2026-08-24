/*
 * stub.h — internals of the synthetic BLE/LC3 ROM stub (ticket 0028).
 *
 * The stub is our own bare-metal code linked at the 993 pinned addresses of
 * the Alif Balletto B1 on-chip BLE ROM (symbol map v1_2).  It implements the
 * GAPM/GAPC/GATT surface the halo firmware actually uses and bridges GATT
 * traffic to the emulator's doorbell device (hw/arm/halo_ble.c in the QEMU
 * fork) through two shared rings in the ROM window.
 *
 * Memory layout (inside the ROM window 0x00090000..0x00160000, see
 * rom-stub.ld — the QEMU machine loads rom-stub-v1_2.bin at 0x00090000):
 *
 *   0x00090000  header (magic/version/ring pointers)
 *   0x0009F094  pinned veneers .. 0x00140D56 (generated, one b.w each)
 *   0x00141000  .text/.rodata (implementations + trap thunks)
 *   0x0014E000  .data/.bss
 *   0x00156000  H2G ring (host -> guest: injected GATT writes, connect, ...)
 *   0x0015A000  G2H ring (guest -> host: notifications, events, db dump)
 *
 * Concurrency: API entry points are called from several firmware threads
 * (main thread during init, the BLE task in rwip_process, SMP/OTA thread,
 * Lua thread...).  Short critical sections use the firmware-provided
 * p_global_int_disable/p_global_int_restore hooks — never across an
 * app-callback invocation.
 */

#ifndef ROM_STUB_H_
#define ROM_STUB_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "halo_rom_ipc.h"

#include "ble_api.h" /* ble_app_hooks_t / ble_rom_config_t */
#include "gap.h"
#include "gapm.h"
#include "gapm_le.h"
#include "gapm_le_adv.h"
#include "gapc.h"
#include "gapc_le.h"
#include "gatt.h"
#include "gatt_db.h"
#include "gatt_srv.h"
#include "gatt_cli.h"

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define STUB_MAX_GATT_USERS 8
#define STUB_MAX_SERVICES 8
#define STUB_MAX_ATTS_PER_SVC 24
#define STUB_FIRST_HDL 0x0010
/* Bearer MTU reported to the firmware.  halo_ble_get_mtu() subtracts
 * GATT_BUFFER_HEADER_LEN (7), so 519 yields the 512-byte usable MTU the
 * REPL wire protocol promises (frame.bluetooth.max_length() == 511,
 * ticket 0030). */
#define STUB_MTU 519

#define STUB_NB_COBUF 10
#define STUB_COBUF_PAYLOAD 560 /* head + data + tail for one ATT PDU */

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

struct stub_gatt_user {
    bool used;
    bool is_client;
    const void *cb; /* gatt_srv_cb_t* or gatt_cli_cb_t* */
};

struct stub_att {
    uint8_t uuid[GATT_UUID_128_LEN];
    uint16_t info;
    uint16_t ext_info;
};

struct stub_svc {
    bool used;
    uint8_t user_lid;
    uint8_t nb_att;
    uint16_t start_hdl;
    uint8_t uuid[GATT_UUID_128_LEN];
    struct stub_att att[STUB_MAX_ATTS_PER_SVC];
};

struct stub_state {
    /* registered by ble_stack_init() */
    const ble_app_hooks_t *hooks;

    /* GAPM configuration */
    const gapm_callbacks_t *gapm_cbs;
    gap_addr_t identity;
    uint8_t identity_type;
    const uint8_t *name;
    uint8_t name_len;
    uint16_t appearance;
    bool configured;

    /* advertising activity (single) */
    const gapm_le_adv_cb_actv_t *adv_cbs;
    bool adv_created;
    bool advertising;

    /* GATT db */
    struct stub_gatt_user user[STUB_MAX_GATT_USERS];
    struct stub_svc svc[STUB_MAX_SERVICES];
    uint16_t next_hdl;

    /* connection (single, conidx 0) */
    bool connected;
    bool bonded;
    bool sec_requested;
    gap_bdaddr_t peer;

    /* pairing sub-state captured from app confirms */
    bool pairing_accepted;
    bool have_ltk;

    /* pending server read/write confirm */
    bool cfm_pending;
    uint16_t cfm_token;
    uint16_t cfm_status;
    uint16_t cfm_length;
    co_buf_t *cfm_buf;

    uint16_t token_seq;
    uint32_t rand_state;

    const gapc_le_power_cb_t *power_cbs;
};

extern struct stub_state g_stub;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

void stub_lock(void);
void stub_unlock(void);

void *stub_memcpy(void *dst, const void *src, size_t n);
void *stub_memset(void *dst, int c, size_t n);

/* co_buf pool */
void stub_cobuf_init(void);
uint8_t hstub_co_buf_alloc(co_buf_t **pp_buf, uint16_t head_len,
                           uint16_t data_len, uint16_t tail_len);
uint8_t hstub_co_buf_release(co_buf_t *p_buf);

/* G2H event emission (stub_ipc.c) */
void stub_ipc_init(void);
bool stub_ipc_send(uint8_t op, const void *hdr, uint16_t hdr_len,
                   const void *payload, uint16_t payload_len);
void stub_ipc_kick(void);
/* H2G consumption */
uint16_t stub_ipc_recv(uint8_t *op, uint8_t *payload, uint16_t max_len);

/* connection/pairing emulation (stub_gapc.c) */
void stub_conn_dispatch_connect(const uint8_t *addr_type_and_addr);
void stub_conn_dispatch_disconnect(uint16_t reason);
void stub_conn_run_pairing_if_needed(void);

/* GATT host-injected operations (stub_gatt.c) */
void stub_gatt_host_write(uint16_t hdl, const uint8_t *data, uint16_t len);
void stub_gatt_host_read(uint16_t hdl);

#endif /* ROM_STUB_H_ */
