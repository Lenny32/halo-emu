/*
 * stub_gapc.c — single-connection GAPC emulation with a deterministic
 * Just-Works pairing sequence.
 *
 * Host-side CONNECT frame drives, in order (all on the BLE task thread):
 *   1. adv stopped callback (a connection consumes the advertising set)
 *   2. gapm_cbs->p_con_req_cbs->le_connection_req(...)
 *      -> the firmware confirms with gapc_le_connection_cfm() and usually
 *         calls gapc_le_request_security()
 *   3. if not paired yet: emulated central-initiated pairing
 *      pairing_req -> (app gapc_le_pairing_accept) -> ltk_req ->
 *      info_req(IRK) -> info_req(CSRK) -> key_received(peer keys) ->
 *      pairing_succeed -> auth_info(encrypted)
 *
 * This satisfies ble_security.c's bond machinery: the bond is committed in
 * halo_ble_sec_on_paired() (gapc_is_bonded() must already return true) and
 * "encrypted" makes halo_ble_is_paired()/is_encrypted() true, unlocking the
 * Lua RX write path.
 */

#include "stub.h"
#include "gapc.h"
#include "gapc_le.h"
#include "gapc_sec.h"
#include "hl_error.h"

#define CONIDX 0

static const gapc_le_con_param_t stub_con_params = {
    .interval = 24, /* 30 ms */
    .latency = 0,
    .sup_to = 100, /* 1 s */
};

/* ------------------------------------------------------------------ */
/* Host-driven events                                                  */
/* ------------------------------------------------------------------ */

void stub_conn_dispatch_connect(const uint8_t *addr_type_and_addr)
{
    const gapm_callbacks_t *cbs = g_stub.gapm_cbs;

    if (cbs == NULL || cbs->p_con_req_cbs == NULL ||
        cbs->p_con_req_cbs->le_connection_req == NULL || g_stub.connected) {
        return;
    }

    g_stub.peer.addr_type = addr_type_and_addr[0];
    stub_memcpy(g_stub.peer.addr, addr_type_and_addr + 1, GAP_BD_ADDR_LEN);
    g_stub.connected = true;
    g_stub.sec_requested = false;
    g_stub.bonded = false;

    /* Advertising set is consumed by the connection */
    if (g_stub.advertising) {
        g_stub.advertising = false;
        if (g_stub.adv_cbs && g_stub.adv_cbs->hdr.actv.stopped) {
            g_stub.adv_cbs->hdr.actv.stopped(0, 0, GAP_ERR_NO_ERROR);
        }
    }

    cbs->p_con_req_cbs->le_connection_req(CONIDX, 0, 0, 1 /* slave */,
                                          &g_stub.peer, &stub_con_params, 0);

    {
        uint8_t p = CONIDX;

        stub_ipc_send(HALO_BLE_EVT_CONNECTED, &p, 1, NULL, 0);
        stub_ipc_kick();
    }

    /* The security manager may have rejected the peer (gapc_disconnect). */
    if (g_stub.connected) {
        stub_conn_run_pairing_if_needed();
    }
}

void stub_conn_dispatch_disconnect(uint16_t reason)
{
    const gapm_callbacks_t *cbs = g_stub.gapm_cbs;

    if (!g_stub.connected) {
        return;
    }
    g_stub.connected = false;
    g_stub.sec_requested = false;

    {
        uint8_t p[3] = { CONIDX, (uint8_t)(reason & 0xFF),
                         (uint8_t)(reason >> 8) };

        stub_ipc_send(HALO_BLE_EVT_DISCONNECTED, p, sizeof(p), NULL, 0);
        stub_ipc_kick();
    }

    if (cbs && cbs->p_info_cbs && cbs->p_info_cbs->disconnected) {
        cbs->p_info_cbs->disconnected(CONIDX, 0, reason);
    }
}

/* ------------------------------------------------------------------ */
/* Pairing emulation                                                   */
/* ------------------------------------------------------------------ */

void stub_conn_run_pairing_if_needed(void)
{
    const gapc_security_cb_t *sec =
        g_stub.gapm_cbs ? g_stub.gapm_cbs->p_sec_cbs : NULL;

    if (sec == NULL || g_stub.bonded || !g_stub.connected) {
        return;
    }

    /* 1. Central initiates pairing (also covers the firmware's own
     *    gapc_le_request_security(): a real central would react to the
     *    Security Request the same way). */
    g_stub.pairing_accepted = false;
    if (sec->pairing_req) {
        sec->pairing_req(CONIDX, 0, 0 /* auth_level */);
    }
    if (!g_stub.pairing_accepted) {
        if (sec->pairing_failed) {
            sec->pairing_failed(CONIDX, 0, 0);
        }
        return;
    }

    /* 2. Local key distribution: the firmware provides LTK/IRK/CSRK. */
    g_stub.have_ltk = false;
    if (sec->ltk_req) {
        sec->ltk_req(CONIDX, 0, GAP_KEY_LEN);
    }
    if (sec->info_req) {
        sec->info_req(CONIDX, 0, GAPC_INFO_IRK);
        sec->info_req(CONIDX, 0, GAPC_INFO_CSRK);
    }

    /* 3. Peer key distribution: fabricate deterministic peer keys with the
     *    peer's (public/static) address as identity, so a later reconnect
     *    with the same address matches the stored bond slot directly. */
    if (sec->key_received) {
        gapc_pairing_keys_t keys;

        stub_memset(&keys, 0, sizeof(keys));
        keys.valid_key_bf = GAP_KDIST_ENCKEY | GAP_KDIST_IDKEY;
        keys.pairing_lvl = GAP_PAIRING_BOND_SECURE_CON;
        for (unsigned i = 0; i < GAP_KEY_LEN; i++) {
            keys.ltk.key.key[i] = (uint8_t)(0xE0 + i);
            keys.irk.key.key[i] = (uint8_t)(0x10 + i);
        }
        keys.ltk.key_size = GAP_KEY_LEN;
        keys.irk.identity = g_stub.peer;
        sec->key_received(CONIDX, 0, &keys);
    }

    /* 4. Done: bonded before pairing_succeed so gapc_is_bonded() is true
     *    inside halo_ble_sec_on_paired(). */
    g_stub.bonded = true;
    if (sec->pairing_succeed) {
        sec->pairing_succeed(CONIDX, 0, GAP_PAIRING_BOND_SECURE_CON, true,
                             GAP_KDIST_ENCKEY);
    }

    /* 5. Link encrypted. */
    if (sec->auth_info) {
        sec->auth_info(CONIDX, 0, GAP_PAIRING_BOND_SECURE_CON, true,
                       GAP_KEY_LEN);
    }

    {
        uint8_t p = CONIDX;

        stub_ipc_send(HALO_BLE_EVT_PAIRED, &p, 1, NULL, 0);
        stub_ipc_kick();
    }
}

/* ------------------------------------------------------------------ */
/* Pinned GAPC entry points                                            */
/* ------------------------------------------------------------------ */

uint16_t hstub_gapc_le_connection_cfm(uint8_t conidx, uint32_t metainfo,
                                      const gapc_bond_data_t *p_data)
{
    (void)metainfo;
    (void)p_data;
    return (conidx == CONIDX && g_stub.connected) ? GAP_ERR_NO_ERROR
                                                  : GAP_ERR_COMMAND_DISALLOWED;
}

uint16_t hstub_gapc_disconnect(uint8_t conidx, uint32_t metainfo,
                               uint16_t reason, gapc_proc_cmp_cb cmp_cb)
{
    if (conidx != CONIDX || !g_stub.connected) {
        return GAP_ERR_COMMAND_DISALLOWED;
    }
    /* Deliver the disconnection like a link-loss event.  Called from the
     * firmware (possibly mid-callback): run the full teardown inline —
     * the firmware's own handler chain copes (it does on hardware, where
     * the callback also arrives on the BLE task). */
    stub_conn_dispatch_disconnect(reason);
    if (cmp_cb) {
        cmp_cb(conidx, metainfo, GAP_ERR_NO_ERROR);
    }
    return GAP_ERR_NO_ERROR;
}

bool hstub_gapc_is_bonded(uint8_t conidx)
{
    return conidx == CONIDX && g_stub.bonded;
}

uint16_t hstub_gapc_le_request_security(uint8_t conidx, uint8_t auth)
{
    (void)auth;
    if (conidx != CONIDX || !g_stub.connected) {
        return GAP_ERR_COMMAND_DISALLOWED;
    }
    /* Recorded only: the connect dispatcher runs the pairing sequence after
     * le_connection_req returns (never re-entrantly inside it). */
    g_stub.sec_requested = true;
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapc_le_pairing_accept(uint8_t conidx, bool accept,
                                      const gapc_pairing_t *p_pairing_info,
                                      uint32_t metainfo)
{
    (void)p_pairing_info;
    (void)metainfo;
    if (conidx != CONIDX) {
        return GAP_ERR_COMMAND_DISALLOWED;
    }
    g_stub.pairing_accepted = accept;
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapc_le_pairing_provide_ltk(uint8_t conidx,
                                           const gapc_ltk_t *p_ltk)
{
    (void)conidx;
    if (p_ltk == NULL) {
        return GAP_ERR_INVALID_PARAM;
    }
    g_stub.have_ltk = true;
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapc_le_pairing_provide_irk(uint8_t conidx,
                                           const gap_sec_key_t *p_irk)
{
    (void)conidx;
    return p_irk ? GAP_ERR_NO_ERROR : GAP_ERR_INVALID_PARAM;
}

uint16_t hstub_gapc_pairing_provide_csrk(uint8_t conidx,
                                         const gap_sec_key_t *p_csrk)
{
    (void)conidx;
    return p_csrk ? GAP_ERR_NO_ERROR : GAP_ERR_INVALID_PARAM;
}

uint16_t hstub_gapc_pairing_numeric_compare_rsp(uint8_t conidx, bool accept)
{
    (void)conidx;
    (void)accept;
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapc_le_encrypt_req_reply(uint8_t conidx, bool accept,
                                         const gap_sec_key_t *p_ltk,
                                         uint8_t key_size)
{
    (void)conidx;
    (void)p_ltk;
    (void)key_size;
    (void)accept;
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapc_le_get_name_cfm(uint8_t conidx, uint16_t token,
                                    uint16_t status, uint16_t complete_length,
                                    uint16_t length, const uint8_t *p_name)
{
    (void)conidx;
    (void)token;
    (void)status;
    (void)complete_length;
    (void)length;
    (void)p_name;
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapc_le_get_appearance_cfm(uint8_t conidx, uint16_t token,
                                          uint16_t status, uint16_t appearance)
{
    (void)conidx;
    (void)token;
    (void)status;
    (void)appearance;
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapc_le_get_preferred_periph_params_cfm(
    uint8_t conidx, uint16_t token, uint16_t status,
    gapc_le_preferred_periph_param_t prefs)
{
    (void)conidx;
    (void)token;
    (void)status;
    (void)prefs;
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapc_le_update_params(
    uint8_t conidx, uint32_t metainfo,
    const gapc_le_con_param_nego_with_ce_len_t *p_param,
    gapc_proc_cmp_cb cmp_cb)
{
    const gapm_callbacks_t *cbs = g_stub.gapm_cbs;

    if (conidx != CONIDX || !g_stub.connected || p_param == NULL) {
        return GAP_ERR_COMMAND_DISALLOWED;
    }

    /* Accept the requested parameters immediately. */
    if (cbs && cbs->p_le_config_cbs && cbs->p_le_config_cbs->param_updated) {
        gapc_le_con_param_t updated = {
            .interval = p_param->hdr.interval_max,
            .latency = p_param->hdr.latency,
            .sup_to = p_param->hdr.sup_to,
        };

        cbs->p_le_config_cbs->param_updated(conidx, metainfo, &updated);
    }
    if (cmp_cb) {
        cmp_cb(conidx, metainfo, GAP_ERR_NO_ERROR);
    }
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapc_le_update_params_cfm(uint8_t conidx, bool accept,
                                         uint16_t ce_len_min,
                                         uint16_t ce_len_max)
{
    (void)conidx;
    (void)accept;
    (void)ce_len_min;
    (void)ce_len_max;
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapc_le_power_set_callbacks(const gapc_le_power_cb_t *p_cbs)
{
    g_stub.power_cbs = p_cbs;
    return GAP_ERR_NO_ERROR;
}
