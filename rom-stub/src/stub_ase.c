/*
 * stub_ase.c — BAP unicast server ASE state machine (ticket 0038).
 *
 * On silicon the ASCS state machine is driven by a central writing the
 * ASE Control Point characteristic. There is no HCI and no link layer
 * here (EMULATOR.md), so the "central" is fabricated on the host side:
 * halo_ble.c forwards HALO_BLE_OP_ASE_* frames from tools/ble_bridge.py
 * and this file turns them into the callback sequence the profile
 * specifies. The firmware's side of the conversation is real — its
 * *_req callbacks run, its *_cfm() answers are honoured, and a refusal
 * stops the transition exactly as it would on hardware.
 *
 * The specified order (bap_uc.h:124-141) is
 *
 *   IDLE -> CODEC_CONFIGURED -> QOS_CONFIGURED -> ENABLING
 *        -> STREAMING -> DISABLING -> RELEASING -> IDLE
 *
 * with the server asking permission at each of the first three edges:
 * cb_configure_codec_req/cb_configure_qos_req/cb_enable_req, each
 * answered by the matching bap_uc_srv_*_cfm(). Those answers arrive on
 * the caller's thread, so the transition is applied inside the cfm
 * rather than assumed by the caller.
 *
 * Data movement is NOT here: cb_dp_update_req only gates the datapath,
 * and the isochronous transport it gates is ticket 0039.
 */

#include "stub.h"

#include "gaf.h"
#include "bap.h"
#include "bap_uc.h"
#include "bap_uc_srv.h"

/* Response codes for the ASE Control Point (bap_uc.h). */
#define BAP_UC_CP_RSP_SUCCESS 0x00

#define STUB_MAX_ASE 8

struct stub_ase {
    bool used;
    uint8_t ase_lid;
    uint8_t con_lid;
    uint8_t state;
    uint8_t stream_lid;
    bool dp_started;
    bap_qos_cfg_t qos_cfg;
};

static struct {
    struct stub_ase ase[STUB_MAX_ASE];
} g_ase;

/* Set by stub_gaf.c when the firmware configures the unicast server. */
const bap_uc_srv_cb_t *stub_gaf_uc_srv_cb(void);
uint8_t stub_gaf_nb_ases(void);
uint8_t stub_gaf_nb_ase_sink(void);

static struct stub_ase *ase_get(uint8_t ase_lid)
{
    if (ase_lid >= STUB_MAX_ASE) {
        return NULL;
    }
    return &g_ase.ase[ase_lid];
}

static void ase_set_state(struct stub_ase *p_ase, uint8_t state)
{
    const bap_uc_srv_cb_t *p_cb = stub_gaf_uc_srv_cb();
    uint8_t evt[3];

    p_ase->state = state;
    if (p_cb != NULL && p_cb->cb_ase_state != NULL) {
        p_cb->cb_ase_state(p_ase->ase_lid, p_ase->con_lid, state,
                           &p_ase->qos_cfg);
    }

    /* Let the host follow the state machine it is driving. */
    evt[0] = p_ase->ase_lid;
    evt[1] = p_ase->con_lid;
    evt[2] = state;
    stub_ipc_send(HALO_BLE_EVT_ASE_STATE, evt, sizeof(evt), NULL, 0);
    stub_ipc_kick(); /* the ring is only drained on the kick */
}

/* ------------------------------------------------------------------ */
/* Host-driven transitions (the fabricated central)                    */
/* ------------------------------------------------------------------ */

/*
 * Ask the firmware to configure a codec on an ASE. The firmware answers
 * through bap_uc_srv_configure_codec_cfm(), which is where the state
 * actually moves — a refusal leaves the ASE where it was.
 */
void stub_ase_configure_codec(uint8_t con_lid, uint8_t ase_lid,
                              uint8_t tgt_latency, uint8_t tgt_phy,
                              gaf_codec_id_t *p_codec_id,
                              const bap_cfg_ptr_t *p_cfg)
{
    const bap_uc_srv_cb_t *p_cb = stub_gaf_uc_srv_cb();
    struct stub_ase *p_ase = ase_get(ase_lid);

    if (p_cb == NULL || p_ase == NULL ||
        p_cb->cb_configure_codec_req == NULL) {
        return;
    }

    p_ase->used = true;
    p_ase->ase_lid = ase_lid;
    p_ase->con_lid = con_lid;
    p_ase->stream_lid = GAF_INVALID_LID;

    /* ase_instance_idx: sink characteristics come first, then source —
     * the same split the firmware assumes when it fills audio_ctx.ase
     * (ble_audio.c:2326-2334). */
    p_cb->cb_configure_codec_req(con_lid, ase_lid, ase_lid, tgt_latency,
                                 tgt_phy, p_codec_id, p_cfg);
}

void stub_ase_configure_qos(uint8_t ase_lid, uint8_t stream_lid,
                            const bap_qos_cfg_t *p_qos_cfg)
{
    const bap_uc_srv_cb_t *p_cb = stub_gaf_uc_srv_cb();
    struct stub_ase *p_ase = ase_get(ase_lid);

    if (p_cb == NULL || p_ase == NULL || !p_ase->used ||
        p_cb->cb_configure_qos_req == NULL) {
        return;
    }
    if (p_ase->state != BAP_UC_ASE_STATE_CODEC_CONFIGURED &&
        p_ase->state != BAP_UC_ASE_STATE_QOS_CONFIGURED) {
        return;
    }

    p_ase->stream_lid = stream_lid;
    if (p_qos_cfg != NULL) {
        p_ase->qos_cfg = *p_qos_cfg;
    }
    p_cb->cb_configure_qos_req(ase_lid, stream_lid, &p_ase->qos_cfg);
}

void stub_ase_enable(uint8_t ase_lid, bap_cfg_metadata_ptr_t *p_metadata)
{
    const bap_uc_srv_cb_t *p_cb = stub_gaf_uc_srv_cb();
    struct stub_ase *p_ase = ase_get(ase_lid);

    if (p_cb == NULL || p_ase == NULL || !p_ase->used ||
        p_cb->cb_enable_req == NULL) {
        return;
    }
    if (p_ase->state != BAP_UC_ASE_STATE_QOS_CONFIGURED) {
        return;
    }
    p_cb->cb_enable_req(ase_lid, p_metadata);
}

/*
 * The receiver-start / CIS-established edge. A sink ASE reaches
 * STREAMING as soon as the CIS is up; a source ASE waits for the client
 * to hand over, which the host drives with the same command.
 */
void stub_ase_start_ready(uint8_t ase_lid)
{
    struct stub_ase *p_ase = ase_get(ase_lid);

    if (p_ase == NULL || !p_ase->used ||
        p_ase->state != BAP_UC_ASE_STATE_ENABLING) {
        return;
    }
    ase_set_state(p_ase, BAP_UC_ASE_STATE_STREAMING);
}

void stub_ase_disable(uint8_t ase_lid)
{
    struct stub_ase *p_ase = ase_get(ase_lid);

    if (p_ase == NULL || !p_ase->used) {
        return;
    }
    if (p_ase->state != BAP_UC_ASE_STATE_STREAMING &&
        p_ase->state != BAP_UC_ASE_STATE_ENABLING) {
        return;
    }
    ase_set_state(p_ase, BAP_UC_ASE_STATE_DISABLING);
}

void stub_ase_release(uint8_t ase_lid)
{
    const bap_uc_srv_cb_t *p_cb = stub_gaf_uc_srv_cb();
    struct stub_ase *p_ase = ase_get(ase_lid);

    if (p_cb == NULL || p_ase == NULL || !p_ase->used ||
        p_cb->cb_release_req == NULL) {
        return;
    }
    if (p_ase->state == BAP_UC_ASE_STATE_IDLE) {
        return;
    }
    p_cb->cb_release_req(ase_lid);
}

/*
 * Gate the datapath. The firmware answers with bap_uc_srv_dp_update_cfm()
 * and only then considers the path up; ticket 0039 hangs the actual
 * isochronous transport off this.
 */
void stub_ase_dp_update(uint8_t ase_lid, bool start)
{
    const bap_uc_srv_cb_t *p_cb = stub_gaf_uc_srv_cb();
    struct stub_ase *p_ase = ase_get(ase_lid);

    if (p_cb == NULL || p_ase == NULL || !p_ase->used ||
        p_cb->cb_dp_update_req == NULL) {
        return;
    }
    p_cb->cb_dp_update_req(ase_lid, start);
}

bool stub_ase_is_streaming(uint8_t ase_lid)
{
    struct stub_ase *p_ase = ase_get(ase_lid);

    return p_ase != NULL && p_ase->used &&
           p_ase->state == BAP_UC_ASE_STATE_STREAMING;
}

uint8_t stub_ase_state(uint8_t ase_lid)
{
    struct stub_ase *p_ase = ase_get(ase_lid);

    return p_ase != NULL ? p_ase->state : BAP_UC_ASE_STATE_IDLE;
}

/* ------------------------------------------------------------------ */
/* Confirmations from the firmware                                     */
/* ------------------------------------------------------------------ */

void hstub_bap_uc_srv_configure_codec_cfm(uint8_t con_lid, uint8_t rsp_code,
                                          uint8_t reason, uint8_t ase_lid,
                                          bap_qos_req_t *p_qos_req,
                                          const bap_cfg_t *p_cfg,
                                          uint32_t ctl_delay_us,
                                          uint16_t dp_cfg_bf)
{
    struct stub_ase *p_ase = ase_get(ase_lid);

    if (p_ase == NULL || rsp_code != BAP_UC_CP_RSP_SUCCESS) {
        return;
    }
    p_ase->used = true;
    p_ase->ase_lid = ase_lid;
    p_ase->con_lid = con_lid;
    ase_set_state(p_ase, BAP_UC_ASE_STATE_CODEC_CONFIGURED);
}

void hstub_bap_uc_srv_configure_qos_cfm(uint8_t ase_lid, uint8_t rsp_code,
                                        uint8_t reason)
{
    struct stub_ase *p_ase = ase_get(ase_lid);

    if (p_ase == NULL || rsp_code != BAP_UC_CP_RSP_SUCCESS) {
        return;
    }
    ase_set_state(p_ase, BAP_UC_ASE_STATE_QOS_CONFIGURED);
}

void hstub_bap_uc_srv_enable_cfm(uint8_t ase_lid, uint8_t rsp_code,
                                 uint8_t reason,
                                 const bap_cfg_metadata_t *p_metadata)
{
    struct stub_ase *p_ase = ase_get(ase_lid);

    if (p_ase == NULL || rsp_code != BAP_UC_CP_RSP_SUCCESS) {
        return;
    }
    ase_set_state(p_ase, BAP_UC_ASE_STATE_ENABLING);
}

void hstub_bap_uc_srv_update_metadata_cfm(uint8_t ase_lid, uint8_t rsp_code,
                                          uint8_t reason,
                                          const bap_cfg_metadata_t *p_metadata)
{
    /* Metadata updates do not move the state machine. */
}

void hstub_bap_uc_srv_release_cfm(uint8_t ase_lid, uint8_t rsp_code,
                                  uint8_t reason, bool idle)
{
    struct stub_ase *p_ase = ase_get(ase_lid);

    if (p_ase == NULL || rsp_code != BAP_UC_CP_RSP_SUCCESS) {
        return;
    }
    /* "idle" asks to skip the RELEASING hop and land straight in IDLE. */
    if (!idle) {
        ase_set_state(p_ase, BAP_UC_ASE_STATE_RELEASING);
    }
    p_ase->stream_lid = GAF_INVALID_LID;
    p_ase->dp_started = false;
    ase_set_state(p_ase, BAP_UC_ASE_STATE_IDLE);
    p_ase->used = false;
}

void hstub_bap_uc_srv_dp_update_cfm(uint8_t ase_lid, bool accept)
{
    struct stub_ase *p_ase = ase_get(ase_lid);

    if (p_ase == NULL || !accept) {
        return;
    }
    p_ase->dp_started = !p_ase->dp_started;
}

/* ------------------------------------------------------------------ */
/* Doorbell dispatch                                                   */
/* ------------------------------------------------------------------ */

/*
 * Decode one HALO_BLE_OP_ASE_* frame. Called from rwip_process on the
 * BLE task, so the firmware callbacks below run on the thread they
 * would run on for any other host-injected event.
 */
void stub_ase_host_op(uint8_t op, const uint8_t *p_data, uint16_t len)
{
    if (len < 1) {
        return;
    }

    switch (op) {
    case HALO_BLE_OP_ASE_CODEC: {
        /* LC3 at whatever the host asked for; the firmware rejects
         * anything outside the capabilities it advertised
         * (frame_octet_within_capa(), ble_audio.c:2204-2236). */
        gaf_codec_id_t codec_id = { .codec_id = { 0 } };
        bap_cfg_ptr_t cfg = { 0 };

        if (len < 8) {
            return;
        }
        codec_id.codec_id[0] = GAPI_CODEC_FORMAT_LC3;
        cfg.param.sampling_freq = p_data[4];
        cfg.param.frame_dur = p_data[5];
        cfg.param.frame_octet = (uint16_t)(p_data[6] | (p_data[7] << 8));
        cfg.param.frames_sdu = 1;
        cfg.p_add_cfg = NULL;

        stub_ase_configure_codec(p_data[1], p_data[0], p_data[2], p_data[3],
                                 &codec_id, &cfg);
        break;
    }
    case HALO_BLE_OP_ASE_QOS: {
        bap_qos_cfg_t qos_cfg = { 0 };

        if (len < 2) {
            return;
        }
        /* A 16 kHz / 10 ms unicast stream: the QoS the firmware's own
         * preferred settings describe (ble_audio.c:1812-1826). */
        qos_cfg.sdu_intv_us = 10000;
        qos_cfg.max_sdu_size = 40;
        qos_cfg.retx_nb = 5;
        qos_cfg.trans_latency_max_ms = 20;
        qos_cfg.pres_delay_us = 40000;
        qos_cfg.phy = 2; /* LE 2M */
        qos_cfg.framing = 0; /* unframed */

        stub_ase_configure_qos(p_data[0], p_data[1], &qos_cfg);
        break;
    }
    case HALO_BLE_OP_ASE_ENABLE:
        stub_ase_enable(p_data[0], NULL);
        break;
    case HALO_BLE_OP_ASE_START:
        stub_ase_start_ready(p_data[0]);
        break;
    case HALO_BLE_OP_ASE_DISABLE:
        stub_ase_disable(p_data[0]);
        break;
    case HALO_BLE_OP_ASE_RELEASE:
        stub_ase_release(p_data[0]);
        break;
    case HALO_BLE_OP_ASE_DP:
        if (len >= 2) {
            stub_ase_dp_update(p_data[0], p_data[1] != 0);
        }
        break;
    default:
        break;
    }
}
