#ifndef SVTAV1_WRAP_H
#define SVTAV1_WRAP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque encoder context.
 *
 * Requires SVT-AV1 >= 4.1.0.
 *
 * Thread-safety
 * -------------
 * SVT-AV1 is internally thread-safe.  The send path and receive path may run
 * concurrently from different threads with no external locking:
 *
 *   Send side  (input thread):    av1enc_send_first_frame / send_frame / send_eos
 *   Receive side (output thread): av1enc_get_packet  – blocks until a packet
 *                                                      is ready or EOS
 *
 * av1enc_open() and av1enc_close() must be called outside of concurrent
 * send/receive activity.
 *
 * Output format
 * -------------
 * The bitstream is raw AV1 OBUs with no container framing.  SVT-AV1 prepends
 * a Temporal Delimiter OBU and Sequence Header OBU to the very first encoded
 * packet, so the output is a self-describing, decodable AV1 low-overhead
 * bitstream from the first byte.  No separate header emission step is needed.
 */
typedef struct Av1Encoder Av1Encoder;

/* ------------------------------------------------------------------ */
/* Output packet                                                        */
/* ------------------------------------------------------------------ */

/**
 * A zero-copy output packet containing raw AV1 OBU data.
 *
 * payload points directly into SVT-AV1's own output buffer; no copy of
 * the encoded data is made.  The buffer is kept alive until
 * av1enc_packet_free() is called.
 *
 * Typical usage:
 *   write(fd, pkt.payload, pkt.payload_size);
 *   av1enc_packet_free(&pkt);
 */
typedef struct {
    /* Direct pointer into SVT-AV1's output buffer.  Valid until free. */
    const uint8_t *payload;
    size_t         payload_size;

    /* Metadata. */
    int64_t  pts;
    int      is_key;

    /* Private: the SVT-AV1 buffer header, used by av1enc_packet_free(). */
    void *_svt_hdr;
} Av1Packet;

/** Return codes for av1enc_get_packet(). */
typedef enum {
    AV1ENC_PKT_OK    =  0,  /* packet populated; caller owns it          */
    AV1ENC_PKT_EOS   =  1,  /* end-of-stream reached; no packet returned */
    AV1ENC_PKT_ERROR = -1,  /* fatal encoder error                       */
} Av1GetPacketResult;

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t width;
    uint32_t height;

    /**
     * CRF quality level [0..63], lower = better.
     * Used only when bitrate_kbps == 0.  Default: 35.
     */
    uint32_t crf;

    /**
     * Encoder mode [0..13], lower = better. Default: 9
     */
    uint32_t enc_mode;

    /**
     * Key / intra frame period; should be a multiple of 8 minus 1.
     */
    int32_t intra_period_length;

    /**
     * Level of parallelism [0..6].
     *   0 = auto (let SVT-AV1 choose based on machine core count)
     *   1 = minimal parallelism / lowest memory use
     *   6 = maximum parallelism / highest memory use
     *
     * In low-delay mode only one picture can be processed at a time,
     * so values above 1 have limited effect on throughput but will
     * still affect memory allocation.
     */
    uint32_t parallelism;
} Av1EncConfig;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

/** Populate *cfg with sane low-latency defaults. */
void av1enc_config_default(Av1EncConfig *cfg);

/**
 * Open an encoder instance.
 * @return  New handle, or NULL on error.
 */
Av1Encoder *av1enc_open(const Av1EncConfig *cfg);

/**
 * Close the encoder and release all resources.
 *
 * Must only be called after the receive side has finished (av1enc_get_packet
 * returned AV1ENC_PKT_EOS or AV1ENC_PKT_ERROR) and all outstanding packets
 * have been freed with av1enc_packet_free().
 */
void av1enc_close(Av1Encoder *enc);

/* ------------------------------------------------------------------ */
/* Send side                                                            */
/* ------------------------------------------------------------------ */

/**
 * Submit the first YUV 4:2:0 frame (forced key frame).
 *
 * The first packet returned by av1enc_get_packet() will contain a Temporal
 * Delimiter OBU, a Sequence Header OBU, and the encoded key frame — no
 * separate header emission is needed.
 *
 * Planes:
 *   y_plane / y_stride   – luma,  width × height
 *   u_plane / uv_stride  – Cb,    width/2 × height/2
 *   v_plane              – Cr,    width/2 × height/2 (same uv_stride)
 *
 * @return  0 on success, non-zero on error.
 */
int av1enc_send_first_frame(
    Av1Encoder    *enc,
    const uint8_t *buf,  uint32_t y_stride, uint32_t uv_stride);

/**
 * Submit a subsequent YUV 4:2:0 frame.
 *
 * @param pts  Presentation timestamp in frame units.
 *             Pass -1 to auto-increment from the previous frame.
 * @return     0 on success, non-zero on error.
 */
int av1enc_send_frame(
    Av1Encoder    *enc,
    const uint8_t *buf,  uint32_t y_stride, uint32_t uv_stride, int64_t pts);

/**
 * Signal end-of-stream.  No further send calls may be made after this.
 * The receive side will drain remaining packets and then return AV1ENC_PKT_EOS.
 *
 * @return  0 on success, non-zero on error.
 */
int av1enc_send_eos(Av1Encoder *enc);

/* ------------------------------------------------------------------ */
/* Receive side                                                         */
/* ------------------------------------------------------------------ */

/**
 * Block until one encoded packet is ready, then populate *pkt_out.
 *
 * On AV1ENC_PKT_OK the caller owns the packet.  pkt_out->payload points
 * directly into SVT-AV1's output buffer — no allocation or copy is made.
 * Write payload[0..payload_size) to the output stream, then call
 * av1enc_packet_free() to return the buffer to SVT-AV1's pool.
 *
 * On AV1ENC_PKT_EOS *pkt_out is not populated; the stream is complete.
 *
 * Safe to call concurrently with the send-side functions.
 * Must NOT be called concurrently with itself.
 *
 * @return  AV1ENC_PKT_OK, AV1ENC_PKT_EOS, or AV1ENC_PKT_ERROR.
 */
Av1GetPacketResult av1enc_get_packet(Av1Encoder *enc, Av1Packet *pkt_out);

/**
 * Release a packet returned by av1enc_get_packet().
 *
 * Returns the SVT-AV1 output buffer to its internal pool.  After this call
 * pkt_out->payload is invalid.
 */
void av1enc_packet_free(Av1Packet *pkt);

#ifdef __cplusplus
}
#endif

#endif /* SVTAV1_WRAP_H */
