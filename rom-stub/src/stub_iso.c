/*
 * stub_iso.c — isochronous "over shared memory" data path (ticket 0039).
 *
 * On silicon this moves LC3 SDUs between the host CPU and the ES0
 * controller through a shared-memory queue. There is no controller here
 * (EMULATOR.md), so the emulator host plays that part: SDUs the firmware
 * sends leave over the doorbell as HALO_BLE_EVT_ISO_SDU, and SDUs the
 * host injects with HALO_BLE_OP_ISO_SDU are copied into whichever buffer
 * the firmware has posted.
 *
 * The API contract the firmware relies on (gapi_isooshm.h:57-72, and the
 * callers in alif/modules/halo/src/iso_datapath_{ctoh,htoc}.c):
 *
 *   dp_init(dp, cb)             register the completion callback
 *   dp_bind(dp, stream_lid, dir) attach to a stream and a direction
 *   dp_set_buf(dp, buf)         hand over the next SDU buffer
 *   cb(dp, buf)                 "buffer done": for OUTPUT it has been
 *                               filled with a received SDU, for INPUT it
 *                               has been transmitted; either way the
 *                               firmware may post the next buffer, and
 *                               it is explicitly allowed to call
 *                               dp_set_buf() from inside the callback
 *
 * Direction naming follows the controller's point of view, which is the
 * opposite of the audio one: GAPI_DP_DIRECTION_INPUT is the firmware
 * *sending* (microphone -> air) and OUTPUT is the firmware *receiving*
 * (air -> speaker).
 *
 * The callback is documented as running from an ISR. Here it runs on
 * whichever thread pumped the exchange — the BLE task for received SDUs,
 * or the caller of dp_set_buf for transmitted ones. Both firmware
 * datapath layers only signal a queue from it, so this is safe; it is
 * noted because it is a real deviation from the hardware.
 */

#include "stub.h"

#include "gaf.h"
#include "gapi.h"
#include "gapi_isooshm.h"

#define STUB_MAX_DP 4

/* Largest SDU the firmware advertises: 16 kHz / 10 ms LC3 is 40 octets,
 * but leave room for the 48 kHz configurations its capability table can
 * be built with (ble_audio.c source_capas, up to 155). */
#define STUB_MAX_SDU 160

/* Nominal SDU interval: the firmware only ever configures 7.5 or 10 ms
 * and advertises 10 ms for both directions (ble_audio.c sink_capas). */
#define STUB_SDU_INTERVAL_US 10000

static struct {
    gapi_isooshm_dp_t *dp[STUB_MAX_DP];
    uint16_t seq_num;
    uint32_t time_us;
} g_iso;

/*
 * There is no radio clock to read. The firmware uses this only to stamp
 * SDUs and to keep its presentation-delay arithmetic monotonic, so a
 * counter that advances one SDU interval per exchanged SDU serves the
 * purpose and keeps runs reproducible.
 */
static uint32_t stub_time_us(void)
{
    return g_iso.time_us;
}

/* Advance the synthetic clock by one SDU interval. */
static void iso_tick(void)
{
    g_iso.time_us += STUB_SDU_INTERVAL_US;
}

static void iso_track(gapi_isooshm_dp_t *dp)
{
    for (unsigned i = 0; i < STUB_MAX_DP; i++) {
        if (g_iso.dp[i] == dp) {
            return;
        }
    }
    for (unsigned i = 0; i < STUB_MAX_DP; i++) {
        if (g_iso.dp[i] == NULL) {
            g_iso.dp[i] = dp;
            return;
        }
    }
}

static void iso_forget(const gapi_isooshm_dp_t *dp)
{
    for (unsigned i = 0; i < STUB_MAX_DP; i++) {
        if (g_iso.dp[i] == dp) {
            g_iso.dp[i] = NULL;
        }
    }
}

/* The bound data path for a direction, or NULL. */
static gapi_isooshm_dp_t *iso_find(uint8_t dir)
{
    for (unsigned i = 0; i < STUB_MAX_DP; i++) {
        gapi_isooshm_dp_t *dp = g_iso.dp[i];

        if (dp != NULL && dp->dir == dir &&
            dp->state != GAPI_ISOOSHM_STATE_INITIALIZED) {
            return dp;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

uint16_t hstub_gapi_isooshm_dp_init(gapi_isooshm_dp_t *dp,
                                    gapi_isooshm_cb_t cb)
{
    if (dp == NULL) {
        return GAP_ERR_INVALID_PARAM;
    }

    dp->stream_lid = GAF_INVALID_LID;
    dp->link_id = 0;
    dp->dir = GAPI_DP_DIRECTION_MAX;
    dp->state = GAPI_ISOOSHM_STATE_INITIALIZED;
    dp->grp_id = 0;
    dp->sdu_queue = NULL;
    dp->buf = NULL;
    dp->cb = cb;

    iso_track(dp);

    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapi_isooshm_dp_bind(gapi_isooshm_dp_t *dp, uint8_t stream_lid,
                                    enum gapi_dp_direction dir)
{
    if (dp == NULL) {
        return GAP_ERR_INVALID_PARAM;
    }

    dp->stream_lid = stream_lid;
    dp->dir = (uint8_t)dir;
    dp->state = GAPI_ISOOSHM_STATE_BOUND;
    iso_track(dp);

    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapi_isooshm_dp_unbind(gapi_isooshm_dp_t *dp,
                                      gapi_isooshm_sdu_buf_t **pp_buf)
{
    if (dp == NULL) {
        return GAP_ERR_INVALID_PARAM;
    }

    /* Hand back any buffer still posted: the caller owns it again. */
    if (pp_buf != NULL) {
        *pp_buf = dp->buf;
    }
    dp->buf = NULL;
    dp->state = GAPI_ISOOSHM_STATE_INITIALIZED;
    dp->dir = GAPI_DP_DIRECTION_MAX;
    iso_forget(dp);

    return GAP_ERR_NO_ERROR;
}

/*
 * Post the next SDU buffer.
 *
 * INPUT (firmware -> air) is completed synchronously: the SDU is already
 * in the buffer, so it goes out over the doorbell and the buffer is
 * immediately handed back. OUTPUT (air -> firmware) is parked until the
 * host injects an SDU.
 */
uint16_t hstub_gapi_isooshm_dp_set_buf(gapi_isooshm_dp_t *dp,
                                       gapi_isooshm_sdu_buf_t *buf)
{
    if (dp == NULL || buf == NULL) {
        return GAP_ERR_INVALID_PARAM;
    }
    if (dp->state == GAPI_ISOOSHM_STATE_INITIALIZED) {
        return GAP_ERR_COMMAND_DISALLOWED;
    }

    dp->buf = buf;
    dp->state = GAPI_ISOOSHM_STATE_TRANSFER_PENDING;

    if (dp->dir == GAPI_DP_DIRECTION_INPUT) {
        uint8_t hdr[4];
        uint16_t len = buf->sdu_len;

        if (len > STUB_MAX_SDU) {
            len = STUB_MAX_SDU;
        }
        hdr[0] = dp->stream_lid;
        hdr[1] = 0;
        hdr[2] = (uint8_t)(buf->seq_num & 0xFF);
        hdr[3] = (uint8_t)(buf->seq_num >> 8);
        stub_ipc_send(HALO_BLE_EVT_ISO_SDU, hdr, sizeof(hdr), buf->data, len);
        stub_ipc_kick();
        iso_tick();

        /* Transmitted: release the buffer straight away. The firmware is
         * allowed to post the next one from inside the callback. */
        dp->buf = NULL;
        dp->state = GAPI_ISOOSHM_STATE_BOUND;
        if (dp->cb != NULL) {
            dp->cb(dp, buf);
        }
    }

    return GAP_ERR_NO_ERROR;
}

uint16_t hstub_gapi_isooshm_dp_get_sync(gapi_isooshm_dp_t *dp,
                                        gapi_isooshm_sdu_sync_t *p_sync)
{
    if (dp == NULL || p_sync == NULL) {
        return GAP_ERR_INVALID_PARAM;
    }

    /* No radio anchor exists; report the local clock so the firmware's
     * presentation-delay arithmetic stays monotonic. */
    p_sync->sdu_ref = stub_time_us();
    p_sync->sdu_anchor = p_sync->sdu_ref;
    p_sync->seq_num = g_iso.seq_num;

    return GAP_ERR_NO_ERROR;
}

uint32_t hstub_gapi_isooshm_dp_get_local_time(void)
{
    return stub_time_us();
}

/* ------------------------------------------------------------------ */
/* Host-injected SDUs                                                  */
/* ------------------------------------------------------------------ */

/*
 * One SDU from the host into the firmware's posted OUTPUT buffer.
 * Dispatched from rwip_process (the BLE task), like every other
 * host-injected event.
 */
void stub_iso_host_sdu(const uint8_t *p_data, uint16_t len)
{
    gapi_isooshm_dp_t *dp = iso_find(GAPI_DP_DIRECTION_OUTPUT);
    gapi_isooshm_sdu_buf_t *buf;
    uint16_t sdu_len;

    if (dp == NULL || dp->buf == NULL || len < 4) {
        return; /* nothing posted: the SDU is dropped, as a real one would be */
    }

    sdu_len = len - 4;
    if (sdu_len > STUB_MAX_SDU) {
        sdu_len = STUB_MAX_SDU;
    }

    buf = dp->buf;
    buf->seq_num = g_iso.seq_num++;
    buf->sdu_len = sdu_len;
    iso_tick();
    buf->timestamp = stub_time_us();
    buf->status = GAPI_ISOOSHM_SDU_STATUS_VALID;
    for (uint16_t i = 0; i < sdu_len; i++) {
        buf->data[i] = p_data[4 + i];
    }

    dp->buf = NULL;
    dp->state = GAPI_ISOOSHM_STATE_BOUND;
    if (dp->cb != NULL) {
        dp->cb(dp, buf);
    }
}
