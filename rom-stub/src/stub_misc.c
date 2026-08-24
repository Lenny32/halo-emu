/*
 * stub_misc.c — deliberate "feature not supported" implementations for the
 * referenced-but-unsupported surface: LE Audio (GAF: BAP/CAP/TMAP/ARC),
 * isochronous shared-memory data path, and the LC3 codec.
 *
 * These are NOT trap thunks: the firmware calls them on every boot and
 * handles failure cleanly (halo_ble_audio_init() aborts on the first
 * configure error; ble_manager logs a warning and continues — exactly the
 * ticket-0028 scope).  Audio lands with ticket 0032.
 *
 * Arguments are ignored, so the entry points are declared argument-less:
 * under AAPCS the caller's r0-r3/stack setup is simply unused and the
 * return value goes in r0.
 */

#include "stub.h"

#define GAF_UNSUPPORTED 0xFF /* any nonzero GAF status = error */

#define GAF_STUB(name)                                                        \
    uint16_t hstub_##name(void)                                               \
    {                                                                         \
        return GAF_UNSUPPORTED;                                               \
    }

#define BOOL_FALSE_STUB(name)                                                 \
    uint32_t hstub_##name(void)                                               \
    {                                                                         \
        return 0;                                                             \
    }

/* BAP unicast server / capabilities server */
GAF_STUB(bap_uc_srv_configure)
BOOL_FALSE_STUB(bap_uc_srv_is_configured)
GAF_STUB(bap_uc_srv_configure_codec_cfm)
GAF_STUB(bap_uc_srv_configure_qos_cfm)
GAF_STUB(bap_uc_srv_dp_update_cfm)
GAF_STUB(bap_uc_srv_enable_cfm)
GAF_STUB(bap_uc_srv_release_cfm)
GAF_STUB(bap_uc_srv_update_metadata_cfm)
GAF_STUB(bap_capa_srv_configure)
BOOL_FALSE_STUB(bap_capa_srv_is_configured)
GAF_STUB(bap_capa_srv_set_record)
GAF_STUB(bap_capa_srv_remove_record)

/* CAP / TMAP */
GAF_STUB(cap_configure)
GAF_STUB(tmap_tmas_configure)

/* ARC (volume / microphone / audio input control) */
GAF_STUB(arc_vcs_configure)
GAF_STUB(arc_mics_configure)
GAF_STUB(arc_aics_configure)
GAF_STUB(arc_aics_add)
GAF_STUB(arc_aics_set_description)
GAF_STUB(arc_aics_set_description_cfm)

/* Isochronous shared-memory data path (never reached while audio init
 * fails, but keep them quiet rather than trap-loud: they are part of the
 * declared-unsupported audio feature, not a missed dependency). */
GAF_STUB(gapi_isooshm_dp_init)
GAF_STUB(gapi_isooshm_dp_bind)
GAF_STUB(gapi_isooshm_dp_unbind)
GAF_STUB(gapi_isooshm_dp_set_buf)
GAF_STUB(gapi_isooshm_dp_get_sync)
GAF_STUB(gapi_isooshm_dp_get_local_time)

/* LC3 codec — int/size_t conventions (see lc3_api.h) */
int hstub_lc3_api_configure(void)
{
    return -1;
}

int hstub_lc3_api_initialise_encoder(void)
{
    return -1;
}

int hstub_lc3_api_initialise_decoder(void)
{
    return -1;
}

int hstub_lc3_api_encode_frame(void)
{
    return -1;
}

int hstub_lc3_api_decode_frame(void)
{
    return -1;
}

uint16_t hstub_lc3_api_get_byte_count(void)
{
    return 0;
}

size_t hstub_lc3_api_encoder_scratch_size(void)
{
    return 0;
}

size_t hstub_lc3_api_decoder_scratch_size(void)
{
    return 0;
}

size_t hstub_lc3_api_decoder_status_size(void)
{
    return 0;
}
