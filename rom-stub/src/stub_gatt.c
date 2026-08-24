/*
 * stub_gatt.c — GATT user registry, attribute database, server data path
 * (host-injected writes/reads, guest-initiated notifications), and the
 * minimal client surface the ANCS module touches.
 */

#include "stub.h"
#include "hl_error.h"

/* ------------------------------------------------------------------ */
/* User registry                                                       */
/* ------------------------------------------------------------------ */

static uint16_t user_register(bool is_client, const void *cb,
                              uint8_t *p_user_lid)
{
    if (cb == NULL || p_user_lid == NULL) {
        return GAP_ERR_INVALID_PARAM;
    }

    stub_lock();
    for (unsigned i = 0; i < STUB_MAX_GATT_USERS; i++) {
        if (!g_stub.user[i].used) {
            g_stub.user[i].used = true;
            g_stub.user[i].is_client = is_client;
            g_stub.user[i].cb = cb;
            stub_unlock();
            *p_user_lid = (uint8_t)i;
            return GAP_ERR_NO_ERROR;
        }
    }
    stub_unlock();
    return GAP_ERR_INSUFF_RESOURCES;
}

uint16_t hstub_gatt_user_srv_register(uint16_t pref_mtu, uint8_t prio_level,
                                      const gatt_srv_cb_t *p_cb,
                                      uint8_t *p_user_lid)
{
    (void)pref_mtu;
    (void)prio_level;
    return user_register(false, p_cb, p_user_lid);
}

uint16_t hstub_gatt_user_cli_register(uint16_t pref_mtu, uint8_t prio_level,
                                      const gatt_cli_cb_t *p_cb,
                                      uint8_t *p_user_lid)
{
    (void)pref_mtu;
    (void)prio_level;
    return user_register(true, p_cb, p_user_lid);
}

uint16_t hstub_gatt_bearer_mtu_min_get(uint8_t conidx)
{
    (void)conidx;
    return STUB_MTU;
}

/* ------------------------------------------------------------------ */
/* Database                                                            */
/* ------------------------------------------------------------------ */

uint16_t hstub_gatt_db_svc_add(uint8_t user_lid, uint8_t info,
                               const uint8_t *p_uuid, uint8_t nb_att,
                               const uint8_t *p_att_mask,
                               const gatt_att_desc_t *p_atts,
                               uint8_t nb_att_rsvd, uint16_t *p_start_hdl)
{
    struct stub_svc *svc = NULL;
    uint8_t nb_rsvd = nb_att_rsvd ? nb_att_rsvd : nb_att;

    (void)info;

    if (p_uuid == NULL || p_atts == NULL || p_start_hdl == NULL ||
        nb_att == 0 || p_att_mask != NULL /* not used by this firmware */) {
        return GAP_ERR_INVALID_PARAM;
    }
    if (user_lid >= STUB_MAX_GATT_USERS || !g_stub.user[user_lid].used) {
        return GAP_ERR_COMMAND_DISALLOWED;
    }
    if (nb_att > STUB_MAX_ATTS_PER_SVC) {
        return GAP_ERR_INSUFF_RESOURCES;
    }

    stub_lock();
    for (unsigned i = 0; i < STUB_MAX_SERVICES; i++) {
        if (!g_stub.svc[i].used) {
            svc = &g_stub.svc[i];
            break;
        }
    }
    if (svc == NULL) {
        stub_unlock();
        return GAP_ERR_INSUFF_RESOURCES;
    }

    svc->used = true;
    svc->user_lid = user_lid;
    svc->nb_att = nb_att;
    svc->start_hdl = g_stub.next_hdl;
    g_stub.next_hdl += nb_rsvd;
    stub_memcpy(svc->uuid, p_uuid, GATT_UUID_128_LEN);
    for (unsigned i = 0; i < nb_att; i++) {
        stub_memcpy(svc->att[i].uuid, p_atts[i].uuid, GATT_UUID_128_LEN);
        svc->att[i].info = p_atts[i].info;
        svc->att[i].ext_info = p_atts[i].ext_info;
    }
    stub_unlock();

    *p_start_hdl = svc->start_hdl;

    /* Dump the service to the host so tests / the REPL bridge can resolve
     * attribute handles by UUID. */
    {
        uint8_t hdr[3] = { (uint8_t)(svc->start_hdl & 0xFF),
                           (uint8_t)(svc->start_hdl >> 8), nb_att };

        stub_ipc_send(HALO_BLE_EVT_SVC, hdr, sizeof(hdr), svc->uuid,
                      GATT_UUID_128_LEN);
    }
    for (unsigned i = 0; i < nb_att; i++) {
        uint16_t hdl = svc->start_hdl + i;
        uint8_t hdr[4] = { (uint8_t)(hdl & 0xFF), (uint8_t)(hdl >> 8),
                           (uint8_t)(svc->att[i].info & 0xFF),
                           (uint8_t)(svc->att[i].info >> 8) };

        stub_ipc_send(HALO_BLE_EVT_ATT, hdr, sizeof(hdr), svc->att[i].uuid,
                      GATT_UUID_128_LEN);
    }
    stub_ipc_kick();

    return GAP_ERR_NO_ERROR;
}

static struct stub_svc *svc_by_hdl(uint16_t hdl)
{
    for (unsigned i = 0; i < STUB_MAX_SERVICES; i++) {
        struct stub_svc *svc = &g_stub.svc[i];

        if (svc->used && hdl >= svc->start_hdl &&
            hdl < (uint16_t)(svc->start_hdl + svc->nb_att)) {
            return svc;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Server data path                                                    */
/* ------------------------------------------------------------------ */

uint16_t hstub_gatt_srv_att_val_set_cfm(uint8_t conidx, uint8_t user_lid,
                                        uint16_t token, uint16_t status)
{
    (void)conidx;
    (void)user_lid;

    if (g_stub.cfm_pending && token == g_stub.cfm_token) {
        g_stub.cfm_pending = false;
        g_stub.cfm_status = status;
    }
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gatt_srv_att_read_get_cfm(uint8_t conidx, uint8_t user_lid,
                                         uint16_t token, uint16_t status,
                                         uint16_t att_length, co_buf_t *p_data)
{
    (void)conidx;
    (void)user_lid;

    if (g_stub.cfm_pending && token == g_stub.cfm_token) {
        g_stub.cfm_pending = false;
        g_stub.cfm_status = status;
        g_stub.cfm_length = att_length;
        g_stub.cfm_buf = p_data; /* held by the caller until its release */
    }
    return GAP_ERR_NO_ERROR;
}

void stub_gatt_host_write(uint16_t hdl, const uint8_t *data, uint16_t len)
{
    struct stub_svc *svc = svc_by_hdl(hdl);
    const gatt_srv_cb_t *cb;
    co_buf_t *buf;
    uint16_t status = ATT_ERR_INVALID_HANDLE;

    if (svc != NULL && !g_stub.user[svc->user_lid].is_client) {
        cb = g_stub.user[svc->user_lid].cb;
        if (cb->cb_att_val_set != NULL &&
            hstub_co_buf_alloc(&buf, 0, len, 0) == 0) {
            stub_memcpy(co_buf_data(buf), data, len);

            g_stub.cfm_pending = true;
            g_stub.cfm_token = ++g_stub.token_seq;
            g_stub.cfm_status = ATT_ERR_UNLIKELY_ERR;

            cb->cb_att_val_set(0, svc->user_lid, g_stub.cfm_token, hdl, 0,
                               buf);

            /* The firmware confirms synchronously from within the callback
             * (all halo services do). */
            status = g_stub.cfm_status;
            g_stub.cfm_pending = false;
            hstub_co_buf_release(buf);
        } else {
            status = ATT_ERR_UNLIKELY_ERR;
        }
    }

    {
        uint8_t p[4] = { (uint8_t)(hdl & 0xFF), (uint8_t)(hdl >> 8),
                         (uint8_t)(status & 0xFF), (uint8_t)(status >> 8) };

        stub_ipc_send(HALO_BLE_EVT_WRITE_STATUS, p, sizeof(p), NULL, 0);
        stub_ipc_kick();
    }
}

void stub_gatt_host_read(uint16_t hdl)
{
    struct stub_svc *svc = svc_by_hdl(hdl);
    const gatt_srv_cb_t *cb;
    uint16_t status = ATT_ERR_INVALID_HANDLE;
    const uint8_t *data = NULL;
    uint16_t len = 0;
    co_buf_t *buf = NULL;

    if (svc != NULL && !g_stub.user[svc->user_lid].is_client) {
        cb = g_stub.user[svc->user_lid].cb;
        if (cb->cb_att_read_get != NULL) {
            g_stub.cfm_pending = true;
            g_stub.cfm_token = ++g_stub.token_seq;
            g_stub.cfm_status = ATT_ERR_UNLIKELY_ERR;
            g_stub.cfm_length = 0;
            g_stub.cfm_buf = NULL;

            cb->cb_att_read_get(0, svc->user_lid, g_stub.cfm_token, hdl, 0,
                                STUB_MTU - 1);

            status = g_stub.cfm_status;
            buf = g_stub.cfm_buf;
            g_stub.cfm_pending = false;
            g_stub.cfm_buf = NULL;
            if (status == GAP_ERR_NO_ERROR && buf != NULL) {
                data = co_buf_data(buf);
                len = co_buf_data_len(buf);
            }
        }
    }

    {
        uint8_t p[4] = { (uint8_t)(hdl & 0xFF), (uint8_t)(hdl >> 8),
                         (uint8_t)(status & 0xFF), (uint8_t)(status >> 8) };

        stub_ipc_send(HALO_BLE_EVT_READ_RSP, p, sizeof(p), data, len);
        stub_ipc_kick();
    }
    /* Note: the read buffer is NOT released here — the firmware allocated
     * it, called the cfm, and releases it itself right after (see e.g.
     * ble_lua.c on_att_read_get). */
}

uint16_t hstub_gatt_srv_event_send(uint8_t conidx, uint8_t user_lid,
                                   uint16_t metainfo, uint8_t evt_type,
                                   uint16_t hdl, co_buf_t *p_data)
{
    const gatt_srv_cb_t *cb;

    if (user_lid >= STUB_MAX_GATT_USERS || !g_stub.user[user_lid].used ||
        g_stub.user[user_lid].is_client || p_data == NULL) {
        return GAP_ERR_INVALID_PARAM;
    }
    if (!g_stub.connected) {
        return GAP_ERR_COMMAND_DISALLOWED;
    }

    {
        uint8_t hdr[3] = { (uint8_t)(hdl & 0xFF), (uint8_t)(hdl >> 8),
                           evt_type };

        stub_ipc_send(HALO_BLE_EVT_NOTIFY, hdr, sizeof(hdr),
                      co_buf_data(p_data), co_buf_data_len(p_data));
        stub_ipc_kick();
    }

    /* Completion is synchronous; the firmware's cb only releases a
     * semaphore, so calling back from inside event_send is safe. */
    cb = g_stub.user[user_lid].cb;
    if (cb->cb_event_sent != NULL) {
        cb->cb_event_sent(conidx, user_lid, metainfo, GAP_ERR_NO_ERROR);
    }
    return GAP_ERR_NO_ERROR;
}

/* ------------------------------------------------------------------ */
/* Client surface (ANCS): no remote database in the emulator            */
/* ------------------------------------------------------------------ */

uint16_t hstub_gatt_cli_discover_svc(uint8_t conidx, uint8_t user_lid,
                                     uint16_t metainfo, uint8_t disc_type,
                                     bool full, uint16_t start_hdl,
                                     uint16_t end_hdl, uint8_t uuid_type,
                                     const uint8_t *p_uuid)
{
    const gatt_cli_cb_t *cb;

    (void)disc_type;
    (void)full;
    (void)start_hdl;
    (void)end_hdl;
    (void)uuid_type;
    (void)p_uuid;

    if (user_lid >= STUB_MAX_GATT_USERS || !g_stub.user[user_lid].used ||
        !g_stub.user[user_lid].is_client) {
        return GAP_ERR_INVALID_PARAM;
    }

    /* Complete with success and no services found: ble_ancs.c maps this to
     * "ANCS not present on peer" and settles cleanly. */
    cb = g_stub.user[user_lid].cb;
    if (cb->cb_discover_cmp != NULL) {
        cb->cb_discover_cmp(conidx, user_lid, metainfo, GAP_ERR_NO_ERROR);
    }
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gatt_cli_event_register(uint8_t conidx, uint8_t user_lid,
                                       uint16_t start_hdl, uint16_t end_hdl)
{
    (void)conidx;
    (void)user_lid;
    (void)start_hdl;
    (void)end_hdl;
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gatt_cli_write(uint8_t conidx, uint8_t user_lid,
                              uint16_t metainfo, uint8_t write_type,
                              uint16_t hdl, uint16_t offset, co_buf_t *p_data)
{
    const gatt_cli_cb_t *cb;

    (void)write_type;
    (void)hdl;
    (void)offset;
    (void)p_data;

    if (user_lid >= STUB_MAX_GATT_USERS || !g_stub.user[user_lid].used ||
        !g_stub.user[user_lid].is_client) {
        return GAP_ERR_INVALID_PARAM;
    }
    cb = g_stub.user[user_lid].cb;
    if (cb->cb_write_cmp != NULL) {
        cb->cb_write_cmp(conidx, user_lid, metainfo, ATT_ERR_INVALID_HANDLE);
    }
    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gatt_cli_att_event_cfm(uint8_t conidx, uint8_t user_lid,
                                      uint16_t token)
{
    (void)conidx;
    (void)user_lid;
    (void)token;
    return GAP_ERR_NO_ERROR;
}
