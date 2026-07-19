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

#include "sr-codec.h"

#include <plugin-support.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

struct sr_encoder {
	AVCodecContext *ctx;
	const AVCodec *codec;
	AVFrame *frame; /* reusable NV12 frame fed to the encoder */
	struct SwsContext *sws;
	enum AVPixelFormat sws_src_format;
	uint32_t src_width;
	uint32_t src_height;
	int64_t next_pts;
	bool unsupported_format_logged;
};

struct sr_decoder {
	AVCodecContext *ctx;
	AVFrame *frame;
};

static enum AVPixelFormat obs_to_av_format(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
		return AV_PIX_FMT_YUV420P;
	case VIDEO_FORMAT_NV12:
		return AV_PIX_FMT_NV12;
	case VIDEO_FORMAT_YUY2:
		return AV_PIX_FMT_YUYV422;
	case VIDEO_FORMAT_UYVY:
		return AV_PIX_FMT_UYVY422;
	case VIDEO_FORMAT_YVYU:
		return AV_PIX_FMT_YVYU422;
	case VIDEO_FORMAT_RGBA:
		return AV_PIX_FMT_RGBA;
	case VIDEO_FORMAT_BGRA:
		return AV_PIX_FMT_BGRA;
	case VIDEO_FORMAT_BGRX:
		return AV_PIX_FMT_BGR0;
	case VIDEO_FORMAT_Y800:
		return AV_PIX_FMT_GRAY8;
	case VIDEO_FORMAT_I444:
		return AV_PIX_FMT_YUV444P;
	case VIDEO_FORMAT_I422:
		return AV_PIX_FMT_YUV422P;
	default:
		return AV_PIX_FMT_NONE;
	}
}

static bool open_encoder(struct sr_encoder *enc, const char *name, uint32_t width, uint32_t height, uint32_t fps_num,
			 uint32_t fps_den, int qp)
{
	const AVCodec *codec = avcodec_find_encoder_by_name(name);
	if (!codec)
		return false;

	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return false;

	/* every frame is an intra frame so the replay can start playback and
	 * scrub at any position without decoding predecessors */
	ctx->width = (int)(width & ~1u);
	ctx->height = (int)(height & ~1u);
	ctx->pix_fmt = AV_PIX_FMT_NV12;
	ctx->time_base = (AVRational){(int)fps_den, (int)fps_num};
	ctx->framerate = (AVRational){(int)fps_num, (int)fps_den};
	ctx->gop_size = 1;
	ctx->max_b_frames = 0;
	/* emit SPS/PPS as out-of-band extradata so the stream can be muxed to
	 * mp4 and decoded from a stored header */
	ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	ctx->color_range = AVCOL_RANGE_MPEG;
	ctx->colorspace = AVCOL_SPC_BT709;
	ctx->color_primaries = AVCOL_PRI_BT709;
	ctx->color_trc = AVCOL_TRC_BT709;
	ctx->thread_count = 0;

	if (strcmp(name, "libx264") == 0) {
		char crf[8];
		snprintf(crf, sizeof(crf), "%d", qp);
		av_opt_set(ctx->priv_data, "crf", crf, 0);
		av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
		av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
	} else if (strcmp(name, "h264_nvenc") == 0) {
		av_opt_set(ctx->priv_data, "rc", "constqp", 0);
		av_opt_set_int(ctx->priv_data, "qp", qp, 0);
		av_opt_set(ctx->priv_data, "preset", "p4", 0);
		av_opt_set(ctx->priv_data, "tune", "ull", 0);
		av_opt_set_int(ctx->priv_data, "delay", 0, 0);
	} else if (strcmp(name, "h264_amf") == 0) {
		av_opt_set(ctx->priv_data, "rc", "cqp", 0);
		av_opt_set_int(ctx->priv_data, "qp_i", qp, 0);
		av_opt_set_int(ctx->priv_data, "qp_p", qp, 0);
		av_opt_set(ctx->priv_data, "usage", "ultralowlatency", 0);
	} else if (strcmp(name, "h264_qsv") == 0) {
		ctx->global_quality = qp;
		av_opt_set(ctx->priv_data, "preset", "fast", 0);
	}

	if (avcodec_open2(ctx, codec, NULL) < 0) {
		avcodec_free_context(&ctx);
		return false;
	}

	AVFrame *frame = av_frame_alloc();
	if (!frame) {
		avcodec_free_context(&ctx);
		return false;
	}
	frame->format = AV_PIX_FMT_NV12;
	frame->width = ctx->width;
	frame->height = ctx->height;
	frame->color_range = AVCOL_RANGE_MPEG;
	frame->colorspace = AVCOL_SPC_BT709;
	if (av_frame_get_buffer(frame, 0) < 0) {
		av_frame_free(&frame);
		avcodec_free_context(&ctx);
		return false;
	}

	enc->ctx = ctx;
	enc->codec = codec;
	enc->frame = frame;
	return true;
}

struct sr_encoder *sr_encoder_create(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
				     enum sr_encoder_backend backend, int qp)
{
	static const char *auto_order[] = {"h264_nvenc", "h264_amf", "h264_qsv", "libx264"};
	const char *only = NULL;

	switch (backend) {
	case SR_ENC_NVENC:
		only = "h264_nvenc";
		break;
	case SR_ENC_AMF:
		only = "h264_amf";
		break;
	case SR_ENC_QSV:
		only = "h264_qsv";
		break;
	case SR_ENC_X264:
		only = "libx264";
		break;
	case SR_ENC_AUTO:
		break;
	}

	struct sr_encoder *enc = bzalloc(sizeof(struct sr_encoder));
	enc->src_width = width;
	enc->src_height = height;
	enc->sws_src_format = AV_PIX_FMT_NONE;

	bool opened = false;
	if (only) {
		opened = open_encoder(enc, only, width, height, fps_num, fps_den, qp);
		/* an explicitly selected hardware encoder may still be
		 * missing on this machine; fall back to software */
		if (!opened && strcmp(only, "libx264") != 0) {
			obs_log(LOG_WARNING, "encoder '%s' unavailable, falling back to libx264", only);
			opened = open_encoder(enc, "libx264", width, height, fps_num, fps_den, qp);
		}
	} else {
		for (size_t i = 0; i < sizeof(auto_order) / sizeof(auto_order[0]) && !opened; i++)
			opened = open_encoder(enc, auto_order[i], width, height, fps_num, fps_den, qp);
	}

	if (!opened) {
		bfree(enc);
		return NULL;
	}

	obs_log(LOG_INFO, "opened replay encoder '%s' (%ux%u, qp %d)", enc->codec->name, enc->ctx->width,
		enc->ctx->height, qp);
	return enc;
}

void sr_encoder_destroy(struct sr_encoder *enc)
{
	if (!enc)
		return;
	if (enc->sws)
		sws_freeContext(enc->sws);
	av_frame_free(&enc->frame);
	avcodec_free_context(&enc->ctx);
	bfree(enc);
}

AVPacket *sr_encoder_encode(struct sr_encoder *enc, const struct obs_source_frame *frame)
{
	const enum AVPixelFormat src_format = obs_to_av_format(frame->format);
	if (src_format == AV_PIX_FMT_NONE) {
		if (!enc->unsupported_format_logged) {
			obs_log(LOG_WARNING, "unsupported source video format %d", (int)frame->format);
			enc->unsupported_format_logged = true;
		}
		return NULL;
	}

	if (enc->sws_src_format != src_format) {
		if (enc->sws)
			sws_freeContext(enc->sws);
		enc->sws = sws_getContext((int)frame->width, (int)frame->height, src_format, enc->ctx->width,
					  enc->ctx->height, AV_PIX_FMT_NV12, SWS_BILINEAR, NULL, NULL, NULL);
		enc->sws_src_format = src_format;
	}
	if (!enc->sws)
		return NULL;

	if (av_frame_make_writable(enc->frame) < 0)
		return NULL;

	const uint8_t *src_data[MAX_AV_PLANES] = {0};
	int src_linesize[MAX_AV_PLANES] = {0};
	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		src_data[i] = frame->data[i];
		src_linesize[i] = (int)frame->linesize[i];
	}

	sws_scale(enc->sws, src_data, src_linesize, 0, (int)frame->height, enc->frame->data, enc->frame->linesize);

	enc->frame->pts = enc->next_pts++;

	if (avcodec_send_frame(enc->ctx, enc->frame) < 0)
		return NULL;

	AVPacket *pkt = av_packet_alloc();
	if (!pkt)
		return NULL;

	const int ret = avcodec_receive_packet(enc->ctx, pkt);
	if (ret < 0) {
		av_packet_free(&pkt);
		return NULL;
	}
	return pkt;
}

enum AVCodecID sr_encoder_codec_id(const struct sr_encoder *enc)
{
	return enc->ctx->codec_id;
}

const char *sr_encoder_name(const struct sr_encoder *enc)
{
	return enc->codec->name;
}

void sr_encoder_get_extradata(const struct sr_encoder *enc, const uint8_t **data, int *size)
{
	*data = enc->ctx->extradata;
	*size = enc->ctx->extradata_size;
}

struct sr_decoder *sr_decoder_create(enum AVCodecID codec_id, const uint8_t *extradata, int extradata_size)
{
	const AVCodec *codec = avcodec_find_decoder(codec_id);
	if (!codec)
		return NULL;

	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return NULL;

	ctx->thread_count = 2;
	ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

	if (extradata && extradata_size > 0) {
		ctx->extradata = av_mallocz((size_t)extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
		if (ctx->extradata) {
			memcpy(ctx->extradata, extradata, (size_t)extradata_size);
			ctx->extradata_size = extradata_size;
		}
	}

	if (avcodec_open2(ctx, codec, NULL) < 0) {
		avcodec_free_context(&ctx);
		return NULL;
	}

	struct sr_decoder *dec = bzalloc(sizeof(struct sr_decoder));
	dec->ctx = ctx;
	dec->frame = av_frame_alloc();
	return dec;
}

void sr_decoder_destroy(struct sr_decoder *dec)
{
	if (!dec)
		return;
	av_frame_free(&dec->frame);
	avcodec_free_context(&dec->ctx);
	bfree(dec);
}

void sr_decoder_flush(struct sr_decoder *dec)
{
	avcodec_flush_buffers(dec->ctx);
}

bool sr_decoder_decode(struct sr_decoder *dec, const AVPacket *pkt, AVFrame **out)
{
	if (avcodec_send_packet(dec->ctx, pkt) < 0)
		return false;

	av_frame_unref(dec->frame);
	if (avcodec_receive_frame(dec->ctx, dec->frame) < 0)
		return false;

	*out = dec->frame;
	return true;
}
