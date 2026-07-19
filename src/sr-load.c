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

#include "sr-load.h"

#include <plugin-support.h>
#include <libavformat/avformat.h>

#define NS_TB \
	(AVRational) { 1, 1000000000 }

bool sr_load_replay(const char *path, struct sr_replay *out)
{
	memset(out, 0, sizeof(*out));

	AVFormatContext *fmt = NULL;
	if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
		obs_log(LOG_WARNING, "sr_load: could not open '%s'", path);
		return false;
	}
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
	out->codec_id = st->codecpar->codec_id;
	out->width = (uint32_t)st->codecpar->width;
	out->height = (uint32_t)st->codecpar->height;
	if (st->codecpar->extradata && st->codecpar->extradata_size > 0) {
		out->extradata = bmemdup(st->codecpar->extradata, (size_t)st->codecpar->extradata_size);
		out->extradata_size = st->codecpar->extradata_size;
	}

	AVPacket *pkt = av_packet_alloc();
	while (av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == vs) {
			const int64_t src_ts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
			struct sr_packet e = {
				.pkt = av_packet_clone(pkt),
				.ts = (uint64_t)av_rescale_q(src_ts, st->time_base, NS_TB),
			};
			if (e.pkt)
				da_push_back(out->video, &e);
		}
		av_packet_unref(pkt);
	}
	av_packet_free(&pkt);
	avformat_close_input(&fmt);

	if (!out->video.num) {
		sr_replay_free(out);
		return false;
	}

	out->first_ts = out->video.array[0].ts;
	out->last_ts = out->video.array[out->video.num - 1].ts;
	obs_log(LOG_INFO, "sr_load: loaded '%s' (%zu frames)", path, out->video.num);
	return true;
}
