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

#include "sr-thumb.h"

#include <obs-module.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

bool sr_thumbnail_rgba(const char *path, int w, int h, uint8_t **out)
{
	*out = NULL;
	if (!path || !*path || w <= 0 || h <= 0)
		return false;

	AVFormatContext *fmt = NULL;
	if (avformat_open_input(&fmt, path, NULL, NULL) < 0)
		return false;
	if (avformat_find_stream_info(fmt, NULL) < 0) {
		avformat_close_input(&fmt);
		return false;
	}

	const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (vs < 0) {
		avformat_close_input(&fmt);
		return false;
	}

	AVStream *st = fmt->streams[vs];
	const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
	AVCodecContext *dec = codec ? avcodec_alloc_context3(codec) : NULL;
	if (!dec) {
		avformat_close_input(&fmt);
		return false;
	}
	avcodec_parameters_to_context(dec, st->codecpar);
	if (avcodec_open2(dec, codec, NULL) < 0) {
		avcodec_free_context(&dec);
		avformat_close_input(&fmt);
		return false;
	}

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	bool have_frame = false;

	while (!have_frame && av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == vs && avcodec_send_packet(dec, pkt) == 0) {
			if (avcodec_receive_frame(dec, frame) == 0)
				have_frame = true;
		}
		av_packet_unref(pkt);
	}
	if (!have_frame) {
		avcodec_send_packet(dec, NULL);
		if (avcodec_receive_frame(dec, frame) == 0)
			have_frame = true;
	}

	bool ok = false;
	if (have_frame) {
		struct SwsContext *sws = sws_getContext(frame->width, frame->height, frame->format, w, h,
							AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
		if (sws) {
			uint8_t *buf = bzalloc((size_t)w * h * 4);
			uint8_t *dst[4] = {buf, NULL, NULL, NULL};
			int dst_linesize[4] = {w * 4, 0, 0, 0};
			sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, dst,
				  dst_linesize);
			sws_freeContext(sws);
			*out = buf;
			ok = true;
		}
	}

	av_frame_free(&frame);
	av_packet_free(&pkt);
	avcodec_free_context(&dec);
	avformat_close_input(&fmt);
	return ok;
}
