/*
 * svtav1_wrap.c
 *
 * Low-latency AV1 encoder wrapper around libSvtAv1Enc.
 * Requires SVT-AV1 >= 3.0.0  (tested against v4.1.0).
 *
 * API compatibility notes (changes that affected this file)
 * ----------------------------------------------------------
 * v2.0: EbBufferHeaderType.n_flags renamed to .flags.
 *       EbSvtAv1EncConfiguration.logical_processors replaced by
 *       level_of_parallelism.
 *
 * v3.0: svt_av1_enc_init_handle() signature changed from 3 args
 *       (handle, app_private, cfg) to 2 args (handle, cfg); the
 *       unused app_private parameter was removed.
 *       svt_av1_enc_deinit_handle() removed entirely.
 *       EbSvtAv1EncConfiguration: several fields removed as part of
 *       the "unused fields" cleanup — enable_intra_refresh,
 *       enable_overlays, super_block_size, film_grain_denoise_strength,
 *       and enable_tf were all removed or replaced.
 *       EbSvtIOFormat: width and height fields removed (redundant with
 *       EbSvtAv1EncConfiguration.source_width / source_height).
 *       Max preset reduced to M10 (enc_mode 10).
 *
 * v4.x: No further API changes relevant to this wrapper.
 *
 * Output format
 * -------------
 * Packets are raw AV1 OBUs with no container framing.  SVT-AV1 prepends a
 * Temporal Delimiter OBU and Sequence Header OBU to the first encoded packet
 * automatically, producing a self-describing bitstream from byte zero.
 *
 * Zero-copy delivery
 * ------------------
 * av1enc_get_packet() sets Av1Packet.payload to point directly into
 * SVT-AV1's own output buffer.  svt_av1_enc_release_out_buffer is deferred
 * until av1enc_packet_free(), so no malloc or memcpy of encoded data is
 * ever performed in the hot path.
 *
 * Build (Linux):
 *   gcc -O2 -fPIC -shared -o libsvtav1_wrap.so svtav1_wrap.c \
 *       $(pkg-config --cflags --libs SvtAv1Enc)
 */

#include "svtav1_wrap.h"

#include <EbSvtAv1Enc.h>
#include <EbSvtAv1.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Enforce a minimum version.  SVT_AV1_CHECK_VERSION was introduced in v0.9;
 * all v3+ releases have it.  We require v3.0 for the cleaned-up API.
 */
#if !defined(SVT_AV1_CHECK_VERSION)
#  error "SVT_AV1_CHECK_VERSION not defined — SVT-AV1 >= 3.0.0 is required"
#endif
#if !SVT_AV1_CHECK_VERSION(3, 0, 0)
#  error "SVT-AV1 >= 3.0.0 is required (v4.1.0 recommended)"
#endif

/* ------------------------------------------------------------------ */
/* Internal state                                                       */
/* ------------------------------------------------------------------ */

struct Av1Encoder {
    EbComponentType          *svt_handle;
    EbSvtAv1EncConfiguration  svt_cfg;

    uint32_t width;
    uint32_t height;

    int64_t  next_pts;
    int      first_sent;
};

/* ------------------------------------------------------------------ */
/* Internal: submit one picture                                         */
/* ------------------------------------------------------------------ */

static int do_send_picture(Av1Encoder    *enc,
                           const uint8_t *buf, uint32_t y_stride,
                           uint32_t uv_stride, int64_t        pts)
{
    EbSvtIOFormat io;
    memset(&io, 0, sizeof(io));
    io.luma      = (uint8_t *)buf;
    io.cb        = (uint8_t *)buf + y_stride * enc->height;
    io.cr        = (uint8_t *)io.cb + uv_stride * enc->height / 2;
    io.y_stride  = y_stride;
    io.cb_stride = uv_stride;
    io.cr_stride = uv_stride;
    /*
     * NOTE: EbSvtIOFormat.width and .height were removed in SVT-AV1 v3.0.
     * Dimensions are taken from EbSvtAv1EncConfiguration.source_width /
     * source_height which are set once at encoder open time.
     */

    EbBufferHeaderType hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.size         = sizeof(EbBufferHeaderType);
    hdr.p_buffer     = (uint8_t *)&io;
    int fsize = y_stride * enc->height + uv_stride * enc->height;
    hdr.n_filled_len = fsize;
    hdr.n_alloc_len = fsize;
    hdr.pts          = (uint64_t)pts;
    //hdr.pic_type     = EB_AV1_INVALID_PICTURE;
    hdr.flags        = 0;           /* renamed from n_flags in v2.0 */

    EbErrorType r = svt_av1_enc_send_picture(enc->svt_handle, &hdr);
    return (r == EB_ErrorNone) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Public: config / open / close                                        */
/* ------------------------------------------------------------------ */

void av1enc_config_default(Av1EncConfig *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->width        = 1920;
    cfg->height       = 1080;
    cfg->crf          = 35;
    cfg->enc_mode     = 9;
    /* Limit period length to allow for seeking. When configuring infinite GOP
     * size, the encoder will still produce periodic IFRs to avoid quality
     * degradation, and for webcam use cases it seems runs were rarely longer
     * than ~50 frames. Should be a multiple of 8 or 16 minus 1, depending on
     * encoding mode / hierarchy level. */
    cfg->intra_period_length = 47;
    cfg->film_grain_noise = 0;
    cfg->parallelism  = 0;  /* 0=auto; limited benefit for frame-at-a-time */
}

Av1Encoder *av1enc_open(const Av1EncConfig *cfg)
{
    if (!cfg) return NULL;

    Av1Encoder *enc = calloc(1, sizeof(Av1Encoder));
    if (!enc) return NULL;

    enc->width   = cfg->width;
    enc->height  = cfg->height;

    /*
     * v3.0+: svt_av1_enc_init_handle takes 2 arguments.
     * The unused void* app_private middle parameter was removed in v3.0.
     */
    EbErrorType ret = svt_av1_enc_init_handle(&enc->svt_handle, &enc->svt_cfg);
    if (ret != EB_ErrorNone) { free(enc); return NULL; }

    EbSvtAv1EncConfiguration *sc = &enc->svt_cfg;

    /* ---- Low-latency preset ---- */
    sc->enc_mode            = cfg->enc_mode;
    sc->pred_structure      = LOW_DELAY;
    // sc->look_ahead_distance = 0;    /* no lookahead; also implicitly disables TF */
    sc->intra_refresh_type  = 2;    /* 1 = CRA open-GOP; 2 = closed GOP */
    sc->intra_period_length = cfg->intra_period_length;
    sc->tune =                0;    /* Visual Quality (subjective), not the default */

    sc->source_width            = cfg->width;
    sc->source_height           = cfg->height;
    sc->forced_max_frame_width  = cfg->width;
    sc->forced_max_frame_height = cfg->height;
    sc->encoder_color_format    = EB_YUV420;
    sc->encoder_bit_depth       = 8;

    sc->rate_control_mode       = SVT_AV1_RC_MODE_CQP_OR_CRF;
    if (cfg->film_grain_noise > 0) {
        sc->film_grain_denoise_apply = 1;
        sc->film_grain_denoise_strength = cfg->film_grain_noise;
    }
    sc->qp                      = cfg->crf;
    sc->aq_mode                 = 2;

    /*
     * level_of_parallelism replaced logical_processors in v2.0.
     * 0 = auto (SVT-AV1 picks based on core count).
     * 1-6 = increasing parallelism / memory use.
     * In low-delay mode only one picture is in flight at a time, so
     * higher levels mainly affect memory allocation rather than throughput.
     */
    sc->level_of_parallelism = cfg->parallelism;

    sc->tile_columns = 1;
    sc->tile_rows    = 0;

    ret = svt_av1_enc_set_parameter(enc->svt_handle, sc);
    if (ret != EB_ErrorNone) {
        /* v3.0+: svt_av1_enc_deinit_handle() was removed. */
        free(enc);
        return NULL;
    }

    ret = svt_av1_enc_init(enc->svt_handle);
    if (ret != EB_ErrorNone) {
        svt_av1_enc_deinit(enc->svt_handle);
        free(enc);
        return NULL;
    }

    return enc;
}

void av1enc_close(Av1Encoder *enc)
{
    if (!enc) return;
    svt_av1_enc_deinit(enc->svt_handle);
    /* svt_av1_enc_deinit_handle() was removed in v3.0. */
    free(enc);
}

/* ------------------------------------------------------------------ */
/* Public: send side                                                    */
/* ------------------------------------------------------------------ */

int av1enc_send_first_frame(
    Av1Encoder    *enc,
    const uint8_t *buf,  uint32_t y_stride,  uint32_t uv_stride)
{
    if (!enc) return -1;
    enc->next_pts   = 0;
    enc->first_sent = 1;
    return do_send_picture(enc, buf, y_stride, uv_stride, enc->next_pts++);
}

int av1enc_send_frame(
    Av1Encoder    *enc,
    const uint8_t *buf,  uint32_t y_stride,
    uint32_t uv_stride, int64_t        pts)
{
    if (!enc) return -1;
    if (!enc->first_sent) {
        fprintf(stderr,
                "av1enc_send_frame: call av1enc_send_first_frame first\n");
        return -1;
    }

    int64_t use_pts = (pts >= enc->next_pts) ? pts : enc->next_pts;
    enc->next_pts   = use_pts + 1;

    return do_send_picture(enc, buf, y_stride, uv_stride, use_pts);
}

int av1enc_send_eos(Av1Encoder *enc)
{
    if (!enc) return -1;
    EbBufferHeaderType eos;
    memset(&eos, 0, sizeof(eos));
    eos.flags = EB_BUFFERFLAG_EOS;   /* .flags, not .n_flags (renamed in v2.0) */
    EbErrorType r = svt_av1_enc_send_picture(enc->svt_handle, &eos);
    return (r == EB_ErrorNone) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Public: receive side                                                 */
/* ------------------------------------------------------------------ */

Av1GetPacketResult av1enc_get_packet(Av1Encoder *enc, Av1Packet *pkt_out)
{
    if (!enc || !pkt_out) return AV1ENC_PKT_ERROR;

    EbBufferHeaderType *hdr = NULL;

    /*
     * Blocking call (second arg = 1).
     * As of v2.3, svt_av1_enc_get_packet in low-delay mode is guaranteed to
     * block until a packet is available, enforcing picture-in / picture-out.
     */
    EbErrorType ret = svt_av1_enc_get_packet(enc->svt_handle, &hdr,
                                             1 /* blocking */);
    if (ret == EB_NoErrorEmptyQueue)
        return AV1ENC_PKT_EOS;   /* shouldn't occur with blocking=1, but safe */
    if (ret != EB_ErrorNone)
        return AV1ENC_PKT_ERROR;

    /* Pure EOS sentinel: .flags (not .n_flags) since v2.0. */
    if ((hdr->flags & EB_BUFFERFLAG_EOS) && hdr->n_filled_len == 0) {
        svt_av1_enc_release_out_buffer(&hdr);
        return AV1ENC_PKT_EOS;
    }

    /*
     * Zero-copy: point directly into SVT-AV1's output buffer and defer
     * svt_av1_enc_release_out_buffer until av1enc_packet_free().
     */
    pkt_out->payload      = hdr->p_buffer;
    pkt_out->payload_size = hdr->n_filled_len;
    pkt_out->pts          = (int64_t)hdr->pts;
    pkt_out->is_key       = (hdr->pic_type == EB_AV1_KEY_PICTURE);
    pkt_out->_svt_hdr     = hdr;

    /*
     * If EOS is piggy-backed on a real packet, return PKT_OK now.
     * The next call will receive the empty sentinel and return PKT_EOS.
     */
    return AV1ENC_PKT_OK;
}

/* ------------------------------------------------------------------ */
/* Public: memory                                                       */
/* ------------------------------------------------------------------ */

void av1enc_packet_free(Av1Packet *pkt)
{
    if (!pkt || !pkt->_svt_hdr) return;
    EbBufferHeaderType *hdr = (EbBufferHeaderType *)pkt->_svt_hdr;
    svt_av1_enc_release_out_buffer(&hdr);
    pkt->_svt_hdr     = NULL;
    pkt->payload      = NULL;
    pkt->payload_size = 0;
}
