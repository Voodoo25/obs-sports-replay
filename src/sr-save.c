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

#include "sr-save.h"

#include <plugin-support.h>
#include <libavformat/avformat.h>

#define NS_TB \
	(AVRational) { 1, 1000000000 }

bool sr_save_replay(const struct sr_replay *r, const char *path)
{
	if (!r->video.num) {
		obs_log(LOG_WARNING, "sr_save: nothing to save");
		return false;
	}

	AVFormatContext *oc = NULL;
	avformat_alloc_output_context2(&oc, NULL, NULL, path);
	if (!oc) {
		obs_log(LOG_WARNING, "sr_save: could not create output context for '%s'", path);
		return false;
	}

	AVStream *st = avformat_new_stream(oc, NULL);
	if (!st) {
		avformat_free_context(oc);
		return false;
	}

	AVCodecParameters *par = st->codecpar;
	par->codec_type = AVMEDIA_TYPE_VIDEO;
	par->codec_id = r->codec_id;
	par->width = (int)r->width;
	par->height = (int)r->height;
	par->format = AV_PIX_FMT_YUV420P;
	if (r->extradata && r->extradata_size > 0) {
		par->extradata = av_mallocz((size_t)r->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
		if (par->extradata) {
			memcpy(par->extradata, r->extradata, (size_t)r->extradata_size);
			par->extradata_size = r->extradata_size;
		}
	}
	st->time_base = NS_TB;

	if (!(oc->oformat->flags & AVFMT_NOFILE)) {
		if (avio_open(&oc->pb, path, AVIO_FLAG_WRITE) < 0) {
			obs_log(LOG_WARNING, "sr_save: could not open '%s' for writing", path);
			avformat_free_context(oc);
			return false;
		}
	}

	if (avformat_write_header(oc, NULL) < 0) {
		obs_log(LOG_WARNING, "sr_save: could not write header for '%s'", path);
		if (!(oc->oformat->flags & AVFMT_NOFILE))
			avio_closep(&oc->pb);
		avformat_free_context(oc);
		return false;
	}

	const int64_t first = (int64_t)r->video.array[0].ts;
	bool ok = true;

	for (size_t i = 0; i < r->video.num && ok; i++) {
		AVPacket *pkt = av_packet_clone(r->video.array[i].pkt);
		if (!pkt) {
			ok = false;
			break;
		}
		pkt->stream_index = st->index;
		pkt->flags |= AV_PKT_FLAG_KEY; /* every frame is intra */

		const int64_t pts_ns = (int64_t)r->video.array[i].ts - first;
		int64_t dur_ns;
		if (i + 1 < r->video.num)
			dur_ns = (int64_t)r->video.array[i + 1].ts - (int64_t)r->video.array[i].ts;
		else
			dur_ns = (r->video.num > 1) ? (int64_t)r->video.array[i].ts -
							      (int64_t)r->video.array[i - 1].ts
						    : 33333333;

		pkt->pts = av_rescale_q(pts_ns, NS_TB, st->time_base);
		pkt->dts = pkt->pts;
		pkt->duration = av_rescale_q(dur_ns, NS_TB, st->time_base);

		if (av_interleaved_write_frame(oc, pkt) < 0)
			ok = false;
		av_packet_free(&pkt);
	}

	if (ok)
		av_write_trailer(oc);

	if (!(oc->oformat->flags & AVFMT_NOFILE))
		avio_closep(&oc->pb);
	avformat_free_context(oc);

	if (ok)
		obs_log(LOG_INFO, "sr_save: wrote replay to '%s' (%zu frames)", path, r->video.num);
	return ok;
}
