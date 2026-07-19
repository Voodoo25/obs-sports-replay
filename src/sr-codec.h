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

#ifdef __cplusplus
extern "C" {
#endif

enum sr_encoder_backend {
	SR_ENC_AUTO = 0,
	SR_ENC_NVENC,
	SR_ENC_AMF,
	SR_ENC_QSV,
	SR_ENC_X264,
};

struct sr_encoder;
struct sr_decoder;

/* Creates an all-intra H.264 encoder. Tries hardware encoders first when
 * backend is SR_ENC_AUTO and falls back to libx264. Returns NULL only if
 * no encoder could be opened at all. qp: 0 (best) .. 51 (worst). */
struct sr_encoder *sr_encoder_create(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
				     enum sr_encoder_backend backend, int qp);
void sr_encoder_destroy(struct sr_encoder *enc);

/* Encodes one OBS frame (any common format; converted internally).
 * Returns an encoded packet owned by the caller, or NULL if the encoder
 * buffered the frame or the format is unsupported. */
AVPacket *sr_encoder_encode(struct sr_encoder *enc, const struct obs_source_frame *frame);

enum AVCodecID sr_encoder_codec_id(const struct sr_encoder *enc);
const char *sr_encoder_name(const struct sr_encoder *enc);

/* Codec header (SPS/PPS) produced by the encoder, needed to decode the
 * stored packets and to mux them to a file. Valid while the encoder lives. */
void sr_encoder_get_extradata(const struct sr_encoder *enc, const uint8_t **data, int *size);

/* Software decoder for the stored all-intra stream. extradata may be NULL. */
struct sr_decoder *sr_decoder_create(enum AVCodecID codec_id, const uint8_t *extradata, int extradata_size);
void sr_decoder_destroy(struct sr_decoder *dec);

/* Call when jumping to a non-sequential packet. */
void sr_decoder_flush(struct sr_decoder *dec);

/* Decodes one packet. On success *out points to a frame owned by the
 * decoder, valid until the next call. */
bool sr_decoder_decode(struct sr_decoder *dec, const AVPacket *pkt, AVFrame **out);

#ifdef __cplusplus
}
#endif
