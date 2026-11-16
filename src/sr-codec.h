/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <obs-module.h>
#include <libavcodec/avcodec.h>

#include "sr-guard.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default keyframe interval, in frames. Small enough that playback only ever
 * decodes a quarter second of video to reach any frame, large enough to keep
 * the encoder off an all-intra workload. */
#define SR_DEFAULT_KEYINT 15

struct sr_encoder;
struct sr_decoder;

/* Creates an H.264 encoder, trying the requested backend first, then the
 * hardware encoders, then libx264 (see sr_guard_backend_order). Backends in
 * skip_mask are not tried. Returns NULL only if no encoder could be opened
 * at all. qp: 0 (best) .. 51 (worst). keyint is the keyframe interval in
 * frames: 1 encodes every frame as an intra frame, which is the most
 * expensive setting for the encoder; larger values cut that cost but make a
 * frame decodable only from its keyframe onwards. owner names the source in
 * log lines. */
struct sr_encoder *sr_encoder_create(const char *owner, uint32_t width, uint32_t height, uint32_t fps_num,
				     uint32_t fps_den, enum sr_encoder_backend backend, uint32_t skip_mask, int qp,
				     int keyint);
void sr_encoder_destroy(struct sr_encoder *enc);

enum sr_send_result {
	SR_SEND_OK,      /* the frame went to the encoder */
	SR_SEND_SKIPPED, /* unsupported source format, nothing was sent */
	SR_SEND_ERROR,   /* the encoder rejected the frame */
};

/* Feeds one OBS frame (any common format; converted internally). */
enum sr_send_result sr_encoder_send(struct sr_encoder *enc, const struct obs_source_frame *frame);

/* Returns the next encoded packet, owned by the caller, or NULL when the
 * encoder has none ready. *ts receives the OBS timestamp of the frame the
 * packet encodes, which lags the last frame sent when the encoder delays
 * its output. Call it until it returns NULL after every send. */
AVPacket *sr_encoder_receive(struct sr_encoder *enc, uint64_t *ts);

enum AVCodecID sr_encoder_codec_id(const struct sr_encoder *enc);
const char *sr_encoder_name(const struct sr_encoder *enc);
enum sr_encoder_backend sr_encoder_backend(const struct sr_encoder *enc);

/* Codec header (SPS/PPS) produced by the encoder, needed to decode the
 * stored packets and to mux them to a file. Valid while the encoder lives. */
void sr_encoder_get_extradata(const struct sr_encoder *enc, const uint8_t **data, int *size);

/* Software decoder for the stored stream. extradata may be NULL. */
struct sr_decoder *sr_decoder_create(enum AVCodecID codec_id, const uint8_t *extradata, int extradata_size);
void sr_decoder_destroy(struct sr_decoder *dec);

/* Call when jumping to a non-sequential packet. The next packet fed to the
 * decoder must be a keyframe. */
void sr_decoder_flush(struct sr_decoder *dec);

/* Decodes one packet. On success *out points to a frame owned by the
 * decoder, valid until the next call. */
bool sr_decoder_decode(struct sr_decoder *dec, const AVPacket *pkt, AVFrame **out);

#ifdef __cplusplus
}
#endif
