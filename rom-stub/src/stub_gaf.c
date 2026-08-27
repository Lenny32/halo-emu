/*
 * stub_gaf.c — Generic Audio Framework: BAP unicast/capabilities server,
 * CAP, TMAP and the ARC volume/microphone/input-control services
 * (ticket 0038).
 *
 * These replace the "declared unsupported" returns of ticket 0028: the
 * firmware's halo_ble_audio_init() (ble_audio.c:2299-2620) walks eight
 * configure steps and aborts on the first non-zero status, so all of
 * them have to succeed before any LE Audio state machine can run.
 *
 * What this file models is the *profile* layer only — the part that on
 * silicon lives in the ROM above the ES0 controller. There is no HCI
 * and no link layer here (see EMULATOR.md), so nothing discovers this
 * device over the air: a peer is fabricated host-side and drives the
 * ASE state machine over the doorbell. The transitions themselves are
 * real, in the sense that the firmware's callbacks are invoked in the
 * order the profile specifies and its *_cfm() answers are honoured.
 *
 * Storage rules from the API docs that matter here: the caller owns the
 * callback tables and the PAC records for as long as they are
 * registered, so pointers are kept rather than copied.
 */

#include "stub.h"

#include "gaf.h"
#include "bap.h"
#include "bap_uc.h"
#include "bap_uc_srv.h"
#include "bap_capa.h"
#include "bap_capa_srv.h"
#include "cap.h"
#include "arc.h"
#include "arc_aics.h"
#include "arc_mics.h"
#include "arc_vcs.h"
#include "tmap_tmas.h"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* The firmware registers one PAC record per supported codec capability
 * and caps itself at MAX_PACS_RECORDS = 64 (ble_audio.c:194), so match
 * that: 0.8.8 registers sink records then source records and overflowed
 * an 8-entry table at source record 4. */
#define STUB_MAX_PAC_RECORDS 64
#define STUB_MAX_ARC_INPUTS 2

struct stub_pac_record {
    bool used;
    uint8_t pac_lid;
    uint8_t record_id;
    const bap_capa_t *p_capa;
    const bap_capa_metadata_t *p_metadata;
    gaf_codec_id_t codec_id;
};

static struct {
    /* BAP unicast server */
    const bap_uc_srv_cb_t *uc_srv_cb;
    bool uc_srv_configured;
    uint8_t nb_ase_sink;
    uint8_t nb_ase_src;
    uint8_t nb_ases_cfg;

    /* BAP capabilities server (PACS) */
    const bap_capa_srv_cb_t *capa_srv_cb;
    bool capa_srv_configured;
    struct stub_pac_record record[STUB_MAX_PAC_RECORDS];

    /* ARC: volume, microphone, audio input control */
    const arc_vcs_cb_t *vcs_cb;
    const arc_mics_cb_t *mics_cb;
    const arc_aics_cb_t *aics_cb;
    uint8_t nb_aics_inputs;
    uint8_t aics_used;

    /* CAP / TMAP have no per-instance state the firmware reads back */
    bool cap_configured;
    bool tmap_configured;
} g_gaf;

const bap_uc_srv_cb_t *stub_gaf_uc_srv_cb(void)
{
    return g_gaf.uc_srv_configured ? g_gaf.uc_srv_cb : NULL;
}

uint8_t stub_gaf_nb_ases(void)
{
    return g_gaf.nb_ase_sink + g_gaf.nb_ase_src;
}

uint8_t stub_gaf_nb_ase_sink(void)
{
    return g_gaf.nb_ase_sink;
}

/* ------------------------------------------------------------------ */
/* BAP unicast server (ASCS)                                           */
/* ------------------------------------------------------------------ */

uint16_t hstub_bap_uc_srv_configure(const bap_uc_srv_cb_t *p_cb,
                                    bap_uc_srv_cfg_t *p_cfg)
{
    if (p_cb == NULL || p_cfg == NULL) {
        return GAF_ERR_INVALID_PARAM;
    }
    /* "At least one Sink/Source ASE characteristic shall be supported"
     * and the total is capped at 15 (bap.h:583-593). */
    if (p_cfg->nb_ase_chars_sink + p_cfg->nb_ase_chars_src == 0 ||
        p_cfg->nb_ase_chars_sink + p_cfg->nb_ase_chars_src > 15) {
        return GAF_ERR_INVALID_PARAM;
    }
    if (p_cfg->nb_ases_cfg <
            p_cfg->nb_ase_chars_sink + p_cfg->nb_ase_chars_src) {
        return GAF_ERR_INVALID_PARAM;
    }

    g_gaf.uc_srv_cb = p_cb;
    g_gaf.nb_ase_sink = p_cfg->nb_ase_chars_sink;
    g_gaf.nb_ase_src = p_cfg->nb_ase_chars_src;
    g_gaf.nb_ases_cfg = p_cfg->nb_ases_cfg;
    g_gaf.uc_srv_configured = true;

    return GAF_ERR_NO_ERROR;
}

bool hstub_bap_uc_srv_is_configured(void)
{
    return g_gaf.uc_srv_configured;
}

/* ------------------------------------------------------------------ */
/* BAP capabilities server (PACS)                                      */
/* ------------------------------------------------------------------ */

uint16_t hstub_bap_capa_srv_configure(const bap_capa_srv_cb_t *p_cb,
                                      bap_capa_srv_cfg_t *p_cfg)
{
    if (p_cb == NULL || p_cfg == NULL) {
        return GAF_ERR_INVALID_PARAM;
    }

    g_gaf.capa_srv_cb = p_cb;
    g_gaf.capa_srv_configured = true;

    return GAF_ERR_NO_ERROR;
}

bool hstub_bap_capa_srv_is_configured(void)
{
    return g_gaf.capa_srv_configured;
}

uint16_t hstub_bap_capa_srv_set_record(uint8_t pac_lid, uint8_t record_id,
                                       const gaf_codec_id_t *p_codec_id,
                                       const bap_capa_t *p_capa,
                                       const bap_capa_metadata_t *p_metadata)
{
    struct stub_pac_record *free_slot = NULL;

    if (!g_gaf.capa_srv_configured) {
        return GAF_ERR_COMMAND_DISALLOWED;
    }
    /* "Pointer to Codec Capabilities structure - cannot be NULL"
     * (bap_capa_srv.h:206-208) */
    if (p_capa == NULL || p_codec_id == NULL) {
        return GAF_ERR_INVALID_PARAM;
    }

    for (unsigned i = 0; i < STUB_MAX_PAC_RECORDS; i++) {
        struct stub_pac_record *rec = &g_gaf.record[i];

        if (rec->used && rec->record_id == record_id) {
            free_slot = rec; /* update in place: record IDs are unique */
            break;
        }
        if (!rec->used && free_slot == NULL) {
            free_slot = rec;
        }
    }
    if (free_slot == NULL) {
        return GAF_ERR_INSUFFICIENT_RESOURCES;
    }

    free_slot->used = true;
    free_slot->pac_lid = pac_lid;
    free_slot->record_id = record_id;
    free_slot->codec_id = *p_codec_id;
    free_slot->p_capa = p_capa;
    free_slot->p_metadata = p_metadata;

    return GAF_ERR_NO_ERROR;
}

uint16_t hstub_bap_capa_srv_remove_record(uint8_t record_id)
{
    for (unsigned i = 0; i < STUB_MAX_PAC_RECORDS; i++) {
        struct stub_pac_record *rec = &g_gaf.record[i];

        if (rec->used && rec->record_id == record_id) {
            rec->used = false;
            return GAF_ERR_NO_ERROR;
        }
    }
    return GAF_ERR_INVALID_PARAM;
}

/* ------------------------------------------------------------------ */
/* CAP (Common Audio Profile) / TMAP                                   */
/* ------------------------------------------------------------------ */

uint16_t hstub_cap_configure(uint8_t cfg_bf,
                             const cap_cas_cfg_param_t *p_cfg_param_cas,
                             const cap_cac_cb_t *p_cb_cac)
{
    g_gaf.cap_configured = true;
    return GAF_ERR_NO_ERROR;
}

uint16_t hstub_tmap_tmas_configure(const tmap_tmas_cfg_param_t *p_cfg_param)
{
    g_gaf.tmap_configured = true;
    return GAF_ERR_NO_ERROR;
}

/* ------------------------------------------------------------------ */
/* ARC: volume / microphone / audio input control                      */
/* ------------------------------------------------------------------ */

uint16_t hstub_arc_vcs_configure(const arc_vcs_cb_t *p_cb, uint8_t step_size,
                                 uint8_t flags, uint8_t volume, uint8_t mute,
                                 uint16_t shdl, uint8_t cfg_bf,
                                 uint8_t nb_inputs, uint8_t *p_input_lid)
{
    if (p_cb == NULL) {
        return GAF_ERR_INVALID_PARAM;
    }
    g_gaf.vcs_cb = p_cb;

    /* One local index per requested input, handed back in order. */
    for (uint8_t i = 0; i < nb_inputs && p_input_lid != NULL; i++) {
        p_input_lid[i] = i;
    }
    return GAF_ERR_NO_ERROR;
}

uint16_t hstub_arc_mics_configure(const arc_mics_cb_t *p_cb, uint16_t shdl,
                                  uint8_t mute, uint8_t cfg_bf,
                                  uint8_t nb_inputs, uint8_t *p_input_lid)
{
    if (p_cb == NULL) {
        return GAF_ERR_INVALID_PARAM;
    }
    g_gaf.mics_cb = p_cb;

    for (uint8_t i = 0; i < nb_inputs && p_input_lid != NULL; i++) {
        p_input_lid[i] = i;
    }
    return GAF_ERR_NO_ERROR;
}

uint16_t hstub_arc_aics_configure(const arc_aics_cb_t *p_cb,
                                  uint8_t nb_inputs, uint16_t pref_mtu)
{
    if (p_cb == NULL || nb_inputs == 0) {
        return GAF_ERR_INVALID_PARAM;
    }
    g_gaf.aics_cb = p_cb;
    g_gaf.nb_aics_inputs = nb_inputs;
    g_gaf.aics_used = 0;

    return GAF_ERR_NO_ERROR;
}

uint16_t hstub_arc_aics_add(const arc_aic_gain_prop_t *p_gain_prop,
                            uint8_t input_type, uint8_t desc_max_len,
                            uint8_t cfg_bf, uint16_t shdl,
                            uint8_t *p_input_lid)
{
    if (g_gaf.aics_cb == NULL) {
        return GAF_ERR_COMMAND_DISALLOWED;
    }
    if (g_gaf.aics_used >= g_gaf.nb_aics_inputs ||
        g_gaf.aics_used >= STUB_MAX_ARC_INPUTS) {
        return GAF_ERR_INSUFFICIENT_RESOURCES;
    }
    if (p_input_lid != NULL) {
        *p_input_lid = g_gaf.aics_used;
    }
    g_gaf.aics_used++;

    return GAF_ERR_NO_ERROR;
}

/*
 * The remaining ARC setters are state the firmware pushes down and only
 * ever reads back through its own copy, so acknowledging them is the
 * whole contract.
 *
 * Note arc_aics_set_gain/_gain_mode/_status are deliberately absent:
 * ble_audio.c calls them, but they are not pinned ROM symbols (they are
 * not in vendor/lds/rom_symbols_ble_v1_2.lds), so the firmware links its
 * own. Implementing them here would be wrong, and gen_rom_layout.py
 * rejects any hstub_ with no pinned address for exactly that reason.
 */
uint16_t hstub_arc_aics_set_description(uint8_t input_lid, uint8_t desc_len,
                                        const uint8_t *p_desc)
{
    return GAF_ERR_NO_ERROR;
}

uint16_t hstub_arc_aics_set_description_cfm(uint8_t con_lid, uint8_t input_lid,
                                            bool accept)
{
    return GAF_ERR_NO_ERROR;
}
