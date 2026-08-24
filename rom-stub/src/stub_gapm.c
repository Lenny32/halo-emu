/*
 * stub_gapm.c — GAPM configuration and the advertising activity.
 *
 * Every procedure completes synchronously: the completion callback runs
 * inside the API call, before it returns.  The firmware tolerates this —
 * completion is signalled through counting semaphores (k_sem_give before
 * k_sem_take is fine) and the advertising state machine is driven from the
 * proc_cmp callback chain exactly as on real hardware, just without the
 * intervening scheduler hops.
 */

#include "stub.h"
#include "gapm_le.h"
#include "gapm_le_adv.h"
#include "hl_error.h"

uint16_t hstub_gapm_configure(uint32_t metainfo, const gapm_config_t *p_cfg,
                              const gapm_callbacks_t *p_cbs,
                              gapm_proc_cmp_cb cmp_cb)
{
    if (p_cfg == NULL || p_cbs == NULL || cmp_cb == NULL) {
        return GAP_ERR_INVALID_PARAM;
    }

    g_stub.gapm_cbs = p_cbs;
    g_stub.identity = p_cfg->private_identity;
    g_stub.identity_type =
        (p_cfg->privacy_cfg & GAPM_PRIV_CFG_PRIV_ADDR_BIT) ? 1 : 0;
    g_stub.configured = true;

    cmp_cb(metainfo, GAP_ERR_NO_ERROR);
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapm_reset(uint32_t metainfo, gapm_proc_cmp_cb cmp_cb)
{
    /* alif_ble_disable() path: quiesce everything. */
    g_stub.configured = false;
    g_stub.adv_created = false;
    g_stub.advertising = false;
    g_stub.connected = false;
    g_stub.sec_requested = false;
    stub_memset(g_stub.user, 0, sizeof(g_stub.user));
    stub_memset(g_stub.svc, 0, sizeof(g_stub.svc));
    g_stub.next_hdl = STUB_FIRST_HDL;

    if (cmp_cb) {
        cmp_cb(metainfo, GAP_ERR_NO_ERROR);
    }
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapm_get_identity(gap_bdaddr_t *p_addr)
{
    if (p_addr == NULL) {
        return GAP_ERR_INVALID_PARAM;
    }
    stub_memcpy(p_addr->addr, g_stub.identity.addr, GAP_BD_ADDR_LEN);
    p_addr->addr_type = g_stub.identity_type;
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapm_set_name(uint32_t metainfo, uint8_t name_len,
                             const uint8_t *p_name, gapm_proc_cmp_cb cmp_cb)
{
    if (p_name == NULL || cmp_cb == NULL) {
        return GAP_ERR_INVALID_PARAM;
    }
    g_stub.name = p_name; /* lifetime owned by the app, per API contract */
    g_stub.name_len = name_len;
    cmp_cb(metainfo, GAP_ERR_NO_ERROR);
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapm_le_set_appearance(uint16_t appearance)
{
    g_stub.appearance = appearance;
    return GAP_ERR_NO_ERROR;
}

/* ------------------------------------------------------------------ */
/* Advertising                                                         */
/* ------------------------------------------------------------------ */

#define STUB_ADV_IDX 0

static void adv_state_evt(uint8_t state)
{
    uint8_t p[2] = { STUB_ADV_IDX, state };

    stub_ipc_send(HALO_BLE_EVT_ADV_STATE, p, sizeof(p), NULL, 0);
    stub_ipc_kick();
}

uint16_t hstub_gapm_le_create_adv_legacy(uint32_t metainfo,
                                         uint8_t own_addr_type,
                                         const gapm_le_adv_create_param_t *p_param,
                                         const gapm_le_adv_cb_actv_t *p_cbs)
{
    (void)own_addr_type;

    if (p_param == NULL || p_cbs == NULL || p_cbs->hdr.actv.proc_cmp == NULL) {
        return GAP_ERR_INVALID_PARAM;
    }
    if (g_stub.adv_created) {
        return GAP_ERR_COMMAND_DISALLOWED;
    }

    g_stub.adv_cbs = p_cbs;
    g_stub.adv_created = true;
    adv_state_evt(HALO_BLE_ADV_CREATED);

    if (p_cbs->created) {
        p_cbs->created(metainfo, STUB_ADV_IDX, 0);
    }
    /* This callback drives the app's chain: set adv data -> set scan rsp
     * -> start advertising, each re-entering the stub below. */
    p_cbs->hdr.actv.proc_cmp(metainfo, GAPM_ACTV_CREATE_LE_ADV, STUB_ADV_IDX,
                             GAP_ERR_NO_ERROR);
    return GAP_ERR_NO_ERROR;
}

static uint16_t adv_data_common(uint8_t actv_idx, co_buf_t *p_data,
                                uint8_t kind, uint8_t proc_id)
{
    if (!g_stub.adv_created || actv_idx != STUB_ADV_IDX || p_data == NULL) {
        return GAP_ERR_COMMAND_DISALLOWED;
    }

    uint8_t hdr[2] = { actv_idx, kind };

    stub_ipc_send(HALO_BLE_EVT_ADV_DATA, hdr, sizeof(hdr),
                  co_buf_data(p_data), co_buf_data_len(p_data));
    stub_ipc_kick();

    g_stub.adv_cbs->hdr.actv.proc_cmp(0, proc_id, actv_idx, GAP_ERR_NO_ERROR);
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapm_le_set_adv_data(uint8_t actv_idx, co_buf_t *p_data)
{
    return adv_data_common(actv_idx, p_data, HALO_BLE_ADV_DATA_ADV,
                           GAPM_ACTV_SET_ADV_DATA);
}

uint16_t hstub_gapm_le_set_scan_response_data(uint8_t actv_idx,
                                              co_buf_t *p_data)
{
    return adv_data_common(actv_idx, p_data, HALO_BLE_ADV_DATA_SCAN_RSP,
                           GAPM_ACTV_SET_SCAN_RSP_DATA);
}

uint16_t hstub_gapm_le_start_adv(uint8_t actv_idx,
                                 const gapm_le_adv_param_t *p_param)
{
    (void)p_param;

    if (!g_stub.adv_created || actv_idx != STUB_ADV_IDX) {
        return GAP_ERR_COMMAND_DISALLOWED;
    }
    g_stub.advertising = true;
    adv_state_evt(HALO_BLE_ADV_STARTED);
    g_stub.adv_cbs->hdr.actv.proc_cmp(0, GAPM_ACTV_START, actv_idx,
                                      GAP_ERR_NO_ERROR);
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapm_stop_activity(uint8_t actv_idx)
{
    if (!g_stub.adv_created || actv_idx != STUB_ADV_IDX) {
        return GAP_ERR_COMMAND_DISALLOWED;
    }
    if (g_stub.advertising) {
        g_stub.advertising = false;
        adv_state_evt(HALO_BLE_ADV_STOPPED);
        if (g_stub.adv_cbs->hdr.actv.stopped) {
            g_stub.adv_cbs->hdr.actv.stopped(0, actv_idx, GAP_ERR_NO_ERROR);
        }
    }
    g_stub.adv_cbs->hdr.actv.proc_cmp(0, GAPM_ACTV_STOP, actv_idx,
                                      GAP_ERR_NO_ERROR);
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapm_le_resolve_address(const gap_addr_t *p_addr,
                                       uint8_t nb_irk,
                                       const gap_sec_key_t *p_irk,
                                       gapm_le_addr_resolved_ind_cb res_cb)
{
    (void)p_addr;
    (void)nb_irk;
    (void)p_irk;
    (void)res_cb;
    /* "Address is not resolvable" — the firmware treats this as an unknown
     * peer and falls back to a fresh pairing, which the stub supports. */
    return GAP_ERR_INVALID_PARAM;
}
