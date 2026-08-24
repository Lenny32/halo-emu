/*
 * fw_blesmoke.c — bare-metal test firmware for the synthetic BLE ROM stub.
 *
 * There is no firmware build (zephyr.bin) on this host, so the ticket-0028
 * gate is exercised with this image: it re-enacts the exact call sequence
 * the halo firmware makes against the ROM (modules/hal/alif/ble/plf/
 * alif_ble.c + modules/halo/src/ble_connection.c + ble_lua.c), compiled
 * against the same vendored Alif headers and linked against the same
 * pinned ROM addresses (via the generated rom-stub ELF symbols).
 *
 * Boot sequence mirrored:
 *   ble_stack_init(hooks, cfg)  -> nonzero (success)
 *   rwip_init(0)                -> p_app_init callback  => "stack-init-ok"
 *   gapm_configure + set_name   -> sync completion      => "gapm-ok"
 *   gatt_user_srv_register + gatt_db_svc_add (RX/TX/CCC service)
 *   adv create -> set data -> set scan rsp -> start     => "adv-start"
 *   event loop: WFI, rwip_process() on doorbell IRQ 377
 *
 * Host-driven (tests/smoke_ble.py over the doorbell TCP bridge):
 *   CONNECT   -> le_connection_req -> connection_cfm + request_security,
 *                emulated pairing  => "connected" / "paired"
 *   GATT_WRITE(CCC=1), GATT_WRITE(RX, payload)
 *                -> cb_att_val_set => echoes payload back as a TX
 *                   notification (gatt_srv_event_send)  => "rx <n>"
 *
 * Console: UART3 (DW 16550, reg-shift 2) @ 0x4901B000.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ble_api.h"
#include "gap.h"
#include "gapm.h"
#include "gapm_le.h"
#include "gapm_le_adv.h"
#include "gapc.h"
#include "gapc_le.h"
#include "gapc_sec.h"
#include "gatt.h"
#include "gatt_db.h"
#include "gatt_srv.h"
#include "co_buf.h"
#include "hl_error.h"
#include "rwip.h"

/* ------------------------------------------------------------------ */
/* Bare-metal glue                                                     */
/* ------------------------------------------------------------------ */

#define UART3_BASE 0x4901B000u
/* reg-shift 2: register n lives at base + (n << 2) */
#define UART_THR (*(volatile uint32_t *)(UART3_BASE + (0 << 2)))
#define UART_LSR (*(volatile uint32_t *)(UART3_BASE + (5 << 2)))

#define NVIC_ISER ((volatile uint32_t *)0xE000E100u)
#define BLE_IRQ 377u

#define UTIMER0_CHAN_INTERRUPT (*(volatile uint32_t *)0x48001118u)

static void putstr(const char *s)
{
    while (*s) {
        while (!(UART_LSR & 0x20)) {
        }
        UART_THR = (uint8_t)*s++;
    }
}

static void puthex16(uint16_t v)
{
    static const char d[] = "0123456789abcdef";
    char b[5] = { d[(v >> 12) & 0xF], d[(v >> 8) & 0xF], d[(v >> 4) & 0xF],
                  d[v & 0xF], 0 };

    putstr(b);
}

static void panic(const char *what)
{
    putstr("TFW: FAIL ");
    putstr(what);
    putstr("\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* ------------------------------------------------------------------ */
/* Hook table (mirrors plf/alif_ble.c app_hooks)                       */
/* ------------------------------------------------------------------ */

static volatile bool evt_pending;
static volatile bool app_init_done;
static void (*sync_cap_cb)(void);

static void hk_int_disable(void)
{
    __asm__ volatile("cpsid i");
}

static void hk_int_restore(void)
{
    __asm__ volatile("cpsie i");
}

static void hk_app_init(void)
{
    app_init_done = true;
}

static void hk_evt_post(void)
{
    evt_pending = true;
}

static void hk_timer_init(void)
{
}

static uint32_t hk_timer_get_time(void)
{
    return 0;
}

static void hk_timer_enable(bool enable)
{
    (void)enable;
}

static void hk_timer_set_timeout(uint32_t timeout, timer_cb callback)
{
    (void)timeout;
    (void)callback;
}

static void hk_reset_request(uint32_t error)
{
    (void)error;
    panic("platform-reset");
}

static uint32_t hk_sync_timer_start(void (*cap)(void), void (*ovf)(void))
{
    (void)ovf;
    if (cap) {
        sync_cap_cb = cap;
    }
    return 160000000;
}

static uint32_t hk_sync_timer_cnt(void)
{
    return 0;
}

static void hk_sync_timer_noop(void)
{
}

static const ble_app_hooks_t hooks = {
    .p_global_int_disable = hk_int_disable,
    .p_global_int_restore = hk_int_restore,
    .p_app_init = hk_app_init,
    .p_timer_init = hk_timer_init,
    .p_timer_get_time = hk_timer_get_time,
    .p_timer_enable = hk_timer_enable,
    .p_timer_set_timeout = hk_timer_set_timeout,
    .p_platform_reset_request = hk_reset_request,
    .p_rtos_evt_post = hk_evt_post,
    .p_sync_timer_start = hk_sync_timer_start,
    .p_sync_timer_get_curr_cnt = hk_sync_timer_cnt,
    .p_sync_timer_get_last_capture = hk_sync_timer_cnt,
    .p_sync_timer_disable_evts = hk_sync_timer_noop,
    .p_sync_timer_restore_evts = hk_sync_timer_noop,
};

static uint32_t heap_env[1024];
static uint32_t heap_prf[1024];
static uint32_t heap_msg[1024];
static uint32_t heap_nrt[256];

static const ble_rom_config_t rom_cfg = {
    .p_ble_heap_env_mem = heap_env,
    .ble_heap_env_mem_size = sizeof(heap_env) / 4,
    .p_ble_heap_profile_mem = heap_prf,
    .ble_heap_profile_mem_size = sizeof(heap_prf) / 4,
    .p_ble_heap_msg_mem = heap_msg,
    .ble_heap_msg_mem_size = sizeof(heap_msg) / 4,
    .p_ble_heap_non_ret_mem = heap_nrt,
    .ble_heap_non_ret_mem_size = sizeof(heap_nrt) / 4,
    .ble_app_main_task = 0,
#ifdef CFG_PATCHING
    .patch = (void *)0,
#endif
};

/* ------------------------------------------------------------------ */
/* GAPM / GAPC callbacks (mirrors ble_connection.c)                    */
/* ------------------------------------------------------------------ */

static volatile bool gapm_done;
static volatile uint8_t adv_state; /* proc chain progress */
static volatile bool connected;
static volatile bool paired;

static void on_gapm_cmp(uint32_t metainfo, uint16_t status)
{
    (void)metainfo;
    if (status != GAP_ERR_NO_ERROR) {
        panic("gapm-cmp");
    }
    gapm_done = true;
}

static void on_le_connection_req(uint8_t conidx, uint32_t metainfo,
                                 uint8_t actv_idx, uint8_t role,
                                 const gap_bdaddr_t *p_peer_addr,
                                 const gapc_le_con_param_t *p_con_params,
                                 uint8_t clk_accuracy)
{
    (void)metainfo;
    (void)actv_idx;
    (void)role;
    (void)p_peer_addr;
    (void)p_con_params;
    (void)clk_accuracy;

    if (gapc_le_connection_cfm(conidx, 0, (void *)0) != GAP_ERR_NO_ERROR) {
        panic("conn-cfm");
    }
    gapc_le_request_security(conidx, 0);
    connected = true;
    putstr("TFW: connected\n");
}

static void on_disconnected(uint8_t conidx, uint32_t metainfo,
                            uint16_t reason)
{
    (void)conidx;
    (void)metainfo;
    (void)reason;
    connected = false;
    putstr("TFW: disconnected\n");
}

static void on_pairing_req(uint8_t conidx, uint32_t metainfo,
                           uint8_t auth_level)
{
    gapc_pairing_t info = {
        .auth = 0x2D, /* SEC_CON | BOND-ish; opaque to the stub */
        .iocap = 0x03,
        .ikey_dist = GAP_KDIST_ENCKEY | GAP_KDIST_IDKEY,
        .rkey_dist = GAP_KDIST_ENCKEY | GAP_KDIST_IDKEY,
        .key_size = GAP_KEY_LEN,
        .oob = 0,
    };

    (void)metainfo;
    (void)auth_level;
    gapc_le_pairing_accept(conidx, true, &info, 0);
}

static void on_ltk_req(uint8_t conidx, uint32_t metainfo, uint8_t key_size)
{
    gapc_ltk_t ltk;

    (void)metainfo;
    for (unsigned i = 0; i < GAP_KEY_LEN; i++) {
        ltk.key.key[i] = (uint8_t)i;
    }
    ltk.ediv = 0x1234;
    for (unsigned i = 0; i < GAP_RAND_NB_LEN; i++) {
        ltk.randnb.nb[i] = (uint8_t)i;
    }
    ltk.key_size = key_size;
    gapc_le_pairing_provide_ltk(conidx, &ltk);
}

static void on_info_req(uint8_t conidx, uint32_t metainfo, uint8_t exp_info)
{
    static const gap_sec_key_t key = { .key = { 1, 2, 3, 4, 5, 6, 7, 8, 9,
                                                10, 11, 12, 13, 14, 15,
                                                16 } };

    (void)metainfo;
    if (exp_info == 0 /* GAPC_INFO_IRK */) {
        gapc_le_pairing_provide_irk(conidx, &key);
    } else {
        gapc_pairing_provide_csrk(conidx, &key);
    }
}

static void on_key_received(uint8_t conidx, uint32_t metainfo,
                            const gapc_pairing_keys_t *p_keys)
{
    (void)conidx;
    (void)metainfo;
    (void)p_keys;
}

static void on_pairing_succeed(uint8_t conidx, uint32_t metainfo,
                               uint8_t pairing_level, bool enc_key_present,
                               uint8_t key_type)
{
    (void)metainfo;
    (void)pairing_level;
    (void)enc_key_present;
    (void)key_type;
    if (!gapc_is_bonded(conidx)) {
        panic("not-bonded");
    }
    paired = true;
    putstr("TFW: paired\n");
}

static void on_pairing_failed(uint8_t conidx, uint32_t metainfo,
                              uint16_t reason)
{
    (void)conidx;
    (void)metainfo;
    (void)reason;
    panic("pairing-failed");
}

static void on_auth_info(uint8_t conidx, uint32_t metainfo, uint8_t sec_lvl,
                         bool encrypted, uint8_t key_size)
{
    (void)conidx;
    (void)metainfo;
    (void)sec_lvl;
    (void)key_size;
    if (encrypted) {
        putstr("TFW: encrypted\n");
    }
}

static const struct gapc_connection_req_cb con_cbs = {
    .le_connection_req = on_le_connection_req,
};

static const gapc_security_cb_t sec_cbs = {
    .pairing_req = on_pairing_req,
    .ltk_req = on_ltk_req,
    .info_req = on_info_req,
    .key_received = on_key_received,
    .pairing_succeed = on_pairing_succeed,
    .pairing_failed = on_pairing_failed,
    .auth_info = on_auth_info,
};

static const gapc_connection_info_cb_t info_cbs = {
    .disconnected = on_disconnected,
};

static const gapc_le_config_cb_t le_cfg_cbs = { 0 };

static void on_gapm_hw_err(uint32_t metainfo, uint8_t code)
{
    (void)metainfo;
    (void)code;
    panic("gapm-hw-err");
}

static const gapm_cb_t gapm_err_cbs = {
    .cb_hw_error = on_gapm_hw_err,
};

static const gapm_callbacks_t gapm_cbs = {
    .p_con_req_cbs = &con_cbs,
    .p_sec_cbs = &sec_cbs,
    .p_info_cbs = &info_cbs,
    .p_le_config_cbs = &le_cfg_cbs,
    .p_bt_config_cbs = (void *)0,
    .p_gapm_cbs = &gapm_err_cbs,
};

/* ------------------------------------------------------------------ */
/* GATT service (mirrors ble_lua.c shape: RX write / TX notify / CCC)  */
/* ------------------------------------------------------------------ */

enum tfw_att_idx {
    TFW_IDX_SVC,
    TFW_IDX_RX_CHAR,
    TFW_IDX_RX_VAL,
    TFW_IDX_TX_CHAR,
    TFW_IDX_TX_VAL,
    TFW_IDX_TX_CCC,
    TFW_IDX_NB,
};

#define ATT16(uuid) { (uuid) & 0xFF, ((uuid) >> 8) & 0xFF, 0 }

static const uint8_t svc_uuid[GATT_UUID_128_LEN] = {
    0x01, 0x2E, 0xF0, 0xDE, 0xAD, 0xBE, 0xEF, 0x00,
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
};
static const uint8_t rx_uuid[GATT_UUID_128_LEN] = {
    0x02, 0x2E, 0xF0, 0xDE, 0xAD, 0xBE, 0xEF, 0x00,
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
};
static const uint8_t tx_uuid[GATT_UUID_128_LEN] = {
    0x03, 0x2E, 0xF0, 0xDE, 0xAD, 0xBE, 0xEF, 0x00,
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
};

static const gatt_att_desc_t att_db[TFW_IDX_NB] = {
    [TFW_IDX_SVC] = { ATT16(GATT_DECL_PRIMARY_SERVICE),
                      ATT_UUID(16) | PROP(RD), 0 },
    [TFW_IDX_RX_CHAR] = { ATT16(GATT_DECL_CHARACTERISTIC),
                          ATT_UUID(16) | PROP(RD), 0 },
    [TFW_IDX_RX_VAL] = { { 0 }, ATT_UUID(128) | PROP(WC) | PROP(WR), 512 },
    [TFW_IDX_TX_CHAR] = { ATT16(GATT_DECL_CHARACTERISTIC),
                          ATT_UUID(16) | PROP(RD), 0 },
    [TFW_IDX_TX_VAL] = { { 0 }, ATT_UUID(128) | PROP(N), OPT(NO_OFFSET) },
    [TFW_IDX_TX_CCC] = { ATT16(GATT_DESC_CLIENT_CHAR_CFG),
                         ATT_UUID(16) | PROP(RD) | PROP(WC) | PROP(WR),
                         OPT(NO_OFFSET) | 2 },
};

static uint8_t user_lid;
static uint16_t start_hdl;
static uint16_t ccc_cfg;

static void tiny_memcpy(uint8_t *d, const uint8_t *s, uint16_t n)
{
    while (n--) {
        *d++ = *s++;
    }
}

static void on_att_val_set(uint8_t conidx, uint8_t lid, uint16_t token,
                           uint16_t hdl, uint16_t offset, co_buf_t *p_data)
{
    uint16_t status = GAP_ERR_NO_ERROR;
    uint16_t att_idx = hdl - start_hdl;
    uint16_t len = co_buf_data_len(p_data);

    (void)offset;

    if (att_idx == TFW_IDX_TX_CCC && len >= 2) {
        ccc_cfg = co_buf_data(p_data)[0] | (co_buf_data(p_data)[1] << 8);
    } else if (att_idx == TFW_IDX_RX_VAL) {
        putstr("TFW: rx ");
        puthex16(len);
        putstr("\n");
        /* Echo back as a notification, like the Lua REPL responding */
        if (ccc_cfg == GATT_CCC_START_NTF) {
            co_buf_t *out;

            if (co_buf_alloc(&out, GATT_BUFFER_HEADER_LEN, len,
                             GATT_BUFFER_TAIL_LEN) == 0) {
                tiny_memcpy(co_buf_data(out), co_buf_data(p_data), len);
                gatt_srv_event_send(conidx, lid, 0x1234, GATT_NOTIFY,
                                    start_hdl + TFW_IDX_TX_VAL, out);
                co_buf_release(out);
            }
        }
    } else {
        status = ATT_ERR_REQUEST_NOT_SUPPORTED;
    }

    gatt_srv_att_val_set_cfm(conidx, lid, token, status);
}

static void on_att_read_get(uint8_t conidx, uint8_t lid, uint16_t token,
                            uint16_t hdl, uint16_t offset,
                            uint16_t max_length)
{
    co_buf_t *p_buf = (void *)0;
    uint16_t status = ATT_ERR_REQUEST_NOT_SUPPORTED;
    uint16_t len = 0;

    (void)offset;
    (void)max_length;

    if ((uint16_t)(hdl - start_hdl) == TFW_IDX_TX_CCC) {
        len = 2;
        if (co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, len,
                         GATT_BUFFER_TAIL_LEN) == 0) {
            co_buf_data(p_buf)[0] = ccc_cfg & 0xFF;
            co_buf_data(p_buf)[1] = ccc_cfg >> 8;
            status = GAP_ERR_NO_ERROR;
        }
    }
    gatt_srv_att_read_get_cfm(conidx, lid, token, status, len, p_buf);
    if (p_buf) {
        co_buf_release(p_buf);
    }
}

static void on_event_sent(uint8_t conidx, uint8_t lid, uint16_t metainfo,
                          uint16_t status)
{
    (void)conidx;
    (void)lid;
    (void)metainfo;
    (void)status;
}

static const gatt_srv_cb_t gatt_cbs = {
    .cb_event_sent = on_event_sent,
    .cb_att_read_get = on_att_read_get,
    .cb_att_val_set = on_att_val_set,
};

/* ------------------------------------------------------------------ */
/* Advertising chain (mirrors ble_connection.c on_adv_actv_proc_cmp)   */
/* ------------------------------------------------------------------ */

static void adv_proc_cmp(uint32_t metainfo, uint8_t proc_id,
                         uint8_t actv_idx, uint16_t status);
static void adv_stopped(uint32_t metainfo, uint8_t actv_idx,
                        uint16_t reason);
static void adv_created(uint32_t metainfo, uint8_t actv_idx, int8_t tx_pwr);

static const gapm_le_adv_cb_actv_t adv_cbs = {
    .hdr.actv.proc_cmp = adv_proc_cmp,
    .hdr.actv.stopped = adv_stopped,
    .created = adv_created,
};

static void adv_created(uint32_t metainfo, uint8_t actv_idx, int8_t tx_pwr)
{
    (void)metainfo;
    (void)actv_idx;
    (void)tx_pwr;
}

static void adv_stopped(uint32_t metainfo, uint8_t actv_idx, uint16_t reason)
{
    (void)metainfo;
    (void)actv_idx;
    (void)reason;
    putstr("TFW: adv-stopped\n");
}

static void adv_set_data(uint8_t actv_idx, bool scan_rsp)
{
    static const uint8_t ad[] = { 0x05, 0x09, 'T', 'F', 'W', '0' };
    co_buf_t *p_buf;

    if (co_buf_alloc(&p_buf, 0, sizeof(ad), 0) != 0) {
        panic("adv-buf");
    }
    tiny_memcpy(co_buf_data(p_buf), ad, sizeof(ad));
    if ((scan_rsp ? gapm_le_set_scan_response_data(actv_idx, p_buf)
                  : gapm_le_set_adv_data(actv_idx, p_buf)) !=
        GAP_ERR_NO_ERROR) {
        panic("adv-data");
    }
    co_buf_release(p_buf);
}

static void adv_proc_cmp(uint32_t metainfo, uint8_t proc_id,
                         uint8_t actv_idx, uint16_t status)
{
    (void)metainfo;

    if (status != GAP_ERR_NO_ERROR) {
        panic("adv-proc");
    }

    switch (proc_id) {
    case GAPM_ACTV_CREATE_LE_ADV:
        adv_set_data(actv_idx, false);
        break;
    case GAPM_ACTV_SET_ADV_DATA:
        adv_set_data(actv_idx, true);
        break;
    case GAPM_ACTV_SET_SCAN_RSP_DATA: {
        gapm_le_adv_param_t param = { .duration = 0 };

        if (gapm_le_start_adv(actv_idx, &param) != GAP_ERR_NO_ERROR) {
            panic("adv-start");
        }
        break;
    }
    case GAPM_ACTV_START:
        adv_state = 1;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* IRQ + main                                                          */
/* ------------------------------------------------------------------ */

void ble_doorbell_irq_handler(void)
{
    UTIMER0_CHAN_INTERRUPT = 0x01; /* W1C capture A */
    if (sync_cap_cb) {
        sync_cap_cb();
    }
}

void tfw_main(void)
{
    putstr("TFW: start\n");

    if (!ble_stack_init(&hooks, &rom_cfg)) {
        panic("stack-init");
    }
    rwip_init(0);
    if (!app_init_done) {
        panic("app-init");
    }
    putstr("TFW: stack-init-ok\n");

    /* GAPM configuration (shape of ble_connection.c gapm_cfg) */
    {
        gapm_config_t cfg = { 0 };

        cfg.role = GAP_ROLE_LE_PERIPHERAL;
        cfg.pairing_mode = GAPM_PAIRING_MODE_ALL;
        cfg.privacy_cfg = GAPM_PRIV_CFG_PRIV_ADDR_BIT;
        cfg.renew_dur = 1500;
        for (int i = 0; i < 6; i++) {
            cfg.private_identity.addr[i] = (uint8_t)(0xC0 + i);
        }

        gapm_done = false;
        if (gapm_configure(0, &cfg, &gapm_cbs, on_gapm_cmp) !=
                GAP_ERR_NO_ERROR || !gapm_done) {
            panic("gapm-configure");
        }

        gap_bdaddr_t id;

        gapm_get_identity(&id);
        if (id.addr[0] != 0xC0) {
            panic("identity");
        }

        gapm_done = false;
        if (gapm_set_name(1, 4, (const uint8_t *)"TFW0", on_gapm_cmp) !=
                GAP_ERR_NO_ERROR || !gapm_done) {
            panic("gapm-set-name");
        }
        gapm_le_set_appearance(0x0942);
    }
    putstr("TFW: gapm-ok\n");

    /* Deliberately hit an unimplemented pinned symbol: the veneer must
     * reach the trap thunk, which reports symbol+LR to the doorbell (QEMU
     * logs it by name) and returns GAP_ERR_NOT_SUPPORTED — loud, not a
     * crash and not a silent success. */
    if (gapm_get_token_id() != GAP_ERR_NOT_SUPPORTED) {
        panic("trap-return");
    }
    putstr("TFW: trap-ok\n");

    /* GATT service */
    if (gatt_user_srv_register(CFG_MAX_LE_MTU, 0, &gatt_cbs, &user_lid) !=
        GAP_ERR_NO_ERROR) {
        panic("srv-register");
    }
    {
        gatt_att_desc_t db[TFW_IDX_NB];

        for (int i = 0; i < TFW_IDX_NB; i++) {
            db[i] = att_db[i];
        }
        tiny_memcpy(db[TFW_IDX_RX_VAL].uuid, rx_uuid, GATT_UUID_128_LEN);
        tiny_memcpy(db[TFW_IDX_TX_VAL].uuid, tx_uuid, GATT_UUID_128_LEN);
        if (gatt_db_svc_add(user_lid, SVC_UUID(128), svc_uuid, TFW_IDX_NB,
                            (void *)0, db, TFW_IDX_NB, &start_hdl) !=
            GAP_ERR_NO_ERROR) {
            panic("svc-add");
        }
    }
    putstr("TFW: svc-hdl ");
    puthex16(start_hdl);
    putstr("\n");

    /* Advertising chain */
    {
        gapm_le_adv_create_param_t param = { 0 };

        param.prop = GAPM_ADV_PROP_UNDIR_CONN_MASK;
        param.disc_mode = GAPM_ADV_MODE_GEN_DISC;
        param.filter_pol = GAPM_ADV_ALLOW_SCAN_ANY_CON_ANY;
        param.prim_cfg.adv_intv_min = 40;
        param.prim_cfg.adv_intv_max = 200;
        param.prim_cfg.ch_map = 0x07;
        param.prim_cfg.phy = 1;

        if (gapm_le_create_adv_legacy(0, 0, &param, &adv_cbs) !=
                GAP_ERR_NO_ERROR || adv_state != 1) {
            panic("adv-create");
        }
    }
    putstr("TFW: adv-start\n");

    /* Doorbell IRQ + event loop (the ble_task while-loop shape) */
    NVIC_ISER[BLE_IRQ / 32] = 1u << (BLE_IRQ % 32);
    putstr("TFW: ready\n");

    for (;;) {
        __asm__ volatile("wfi");
        if (evt_pending) {
            evt_pending = false;
            rwip_process();
        }
    }
}
