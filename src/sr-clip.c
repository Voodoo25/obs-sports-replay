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

#include "sr-clip.h"

#include <plugin-support.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

struct sr_clip {
	AVFormatContext *fmt;
	AVCodecContext *dec;
	int stream_index;
	AVRational time_base;

	uint32_t src_width;
	uint32_t src_height;
	uint32_t out_width;
	uint32_t out_height;

	struct SwsContext *sws;
	enum AVPixelFormat sws_src_format;

	AVPacket *pkt;
	AVFrame *dec_frame; /* raw decoder output */
	AVFrame *ready;     /* scaled I420, the next frame to show */
	int64_t ready_pts_ns;
	bool have_ready;
	bool flushed; /* sent the draining null packet */
	bool drained; /* decoder fully drained */
	int64_t start_pts;
	int64_t fallback_pts;
};

struct sr_clip *sr_clip_open(const char *path)
{
	if (!path || !*path)
		return NULL;

	AVFormatContext *fmt = NULL;
	if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
		obs_log(LOG_WARNING, "sr_clip: could not open '%s'", path);
		return NULL;
	}
	if (avformat_find_stream_info(fmt, NULL) < 0) {
		avformat_close_input(&fmt);
		return NULL;
	}

	int stream_index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (stream_index < 0) {
		obs_log(LOG_WARNING, "sr_clip: no video stream in '%s'", path);
		avformat_close_input(&fmt);
		return NULL;
	}

	AVStream *stream = fmt->streams[stream_index];
	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec) {
		avformat_close_input(&fmt);
		return NULL;
	}

	AVCodecContext *dec = avcodec_alloc_context3(codec);
	if (!dec) {
		avformat_close_input(&fmt);
		return NULL;
	}
	/* copies extradata (SPS/PPS etc.) needed to decode file streams */
	avcodec_parameters_to_context(dec, stream->codecpar);
	dec->thread_count = 2;
	if (avcodec_open2(dec, codec, NULL) < 0) {
		avcodec_free_context(&dec);
		avformat_close_input(&fmt);
		return NULL;
	}

	struct sr_clip *c = bzalloc(sizeof(struct sr_clip));
	c->fmt = fmt;
	c->dec = dec;
	c->stream_index = stream_index;
	c->time_base = stream->time_base;
	c->src_width = (uint32_t)dec->width;
	c->src_height = (uint32_t)dec->height;
	c->out_width = (uint32_t)dec->width;
	c->out_height = (uint32_t)dec->height;
	c->sws_src_format = AV_PIX_FMT_NONE;
	c->pkt = av_packet_alloc();
	c->dec_frame = av_frame_alloc();
	c->ready = av_frame_alloc();
	c->start_pts = AV_NOPTS_VALUE;

	obs_log(LOG_INFO, "sr_clip: opened '%s' (%ux%u, %s)", path, c->src_width, c->src_height, codec->name);
	return c;
}

void sr_clip_close(struct sr_clip *c)
{
	if (!c)
		return;
	if (c->sws)
		sws_freeContext(c->sws);
	av_frame_free(&c->ready);
	av_frame_free(&c->dec_frame);
	av_packet_free(&c->pkt);
	avcodec_free_context(&c->dec);
	avformat_close_input(&c->fmt);
	bfree(c);
}

uint32_t sr_clip_width(const struct sr_clip *c)
{
	return c->out_width;
}

uint32_t sr_clip_height(const struct sr_clip *c)
{
	return c->out_height;
}

void sr_clip_set_output_size(struct sr_clip *c, uint32_t width, uint32_t height)
{
	if (!width || !height)
		return;
	if (c->out_width == width && c->out_height == height)
		return;
	c->out_width = width & ~1u;
	c->out_height = height & ~1u;
	if (c->sws) {
		sws_freeContext(c->sws);
		c->sws = NULL;
		c->sws_src_format = AV_PIX_FMT_NONE;
	}
}

static bool scale_ready(struct sr_clip *c)
{
	const enum AVPixelFormat src_format = c->dec_frame->format;

	if (!c->sws || c->sws_src_format != src_format) {
		if (c->sws)
			sws_freeContext(c->sws);
		c->sws = sws_getContext(c->dec_frame->width, c->dec_frame->height, src_format, (int)c->out_width,
					(int)c->out_height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
		c->sws_src_format = src_format;
		if (!c->sws)
			return false;
	}

	av_frame_unref(c->ready);
	c->ready->format = AV_PIX_FMT_YUV420P;
	c->ready->width = (int)c->out_width;
	c->ready->height = (int)c->out_height;
	if (av_frame_get_buffer(c->ready, 0) < 0)
		return false;

	sws_scale(c->sws, (const uint8_t *const *)c->dec_frame->data, c->dec_frame->linesize, 0, c->dec_frame->height,
		  c->ready->data, c->ready->linesize);
	return true;
}

/* Reads packets until the decoder yields the next frame into c->ready. */
static bool decode_next(struct sr_clip *c)
{
	for (;;) {
		int ret = avcodec_receive_frame(c->dec, c->dec_frame);
		if (ret == 0) {
			int64_t pts = c->dec_frame->best_effort_timestamp;
			if (pts == AV_NOPTS_VALUE)
				pts = c->fallback_pts++;
			if (c->start_pts == AV_NOPTS_VALUE)
				c->start_pts = pts;
			c->ready_pts_ns = av_rescale_q(pts - c->start_pts, c->time_base,
						       (AVRational){1, 1000000000});
			if (!scale_ready(c))
				continue;
			return true;
		}
		if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
			c->drained = true;
			return false;
		}
		if (ret == AVERROR_EOF) {
			c->drained = true;
			return false;
		}

		/* EAGAIN: feed more packets */
		if (c->flushed) {
			c->drained = true;
			return false;
		}
		int rd = av_read_frame(c->fmt, c->pkt);
		if (rd < 0) {
			avcodec_send_packet(c->dec, NULL); /* start draining */
			c->flushed = true;
			continue;
		}
		if (c->pkt->stream_index == c->stream_index)
			avcodec_send_packet(c->dec, c->pkt);
		av_packet_unref(c->pkt);
	}
}

void sr_clip_rewind(struct sr_clip *c)
{
	av_seek_frame(c->fmt, c->stream_index, 0, AVSEEK_FLAG_BACKWARD);
	avcodec_flush_buffers(c->dec);
	c->flushed = false;
	c->drained = false;
	c->start_pts = AV_NOPTS_VALUE;
	c->fallback_pts = 0;
	c->have_ready = decode_next(c);
}

bool sr_clip_advance(struct sr_clip *c, int64_t playhead_ns, AVFrame **out, bool *ended)
{
	*out = NULL;
	*ended = false;

	if (!c->have_ready) {
		*ended = true;
		return false;
	}

	bool produced = false;
	while (c->have_ready && c->ready_pts_ns <= playhead_ns) {
		*out = c->ready;
		produced = true;
		c->have_ready = decode_next(c);
	}

	if (!c->have_ready && c->drained && !produced)
		*ended = true;

	return produced;
}
