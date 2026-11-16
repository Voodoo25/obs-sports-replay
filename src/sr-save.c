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
#include <util/dstr.h>
#include <util/platform.h>
#include <libavformat/avformat.h>

#define NS_TB \
	(AVRational) { 1, 1000000000 }

/* Creates the folder path is to be written in, with any missing parents. */
static bool ensure_parent_dir(const char *path)
{
	struct dstr dir = {0};
	dstr_copy(&dir, path);
	dstr_replace(&dir, "\\", "/");

	char *slash = strrchr(dir.array, '/');
	bool ok = true;
	if (slash && slash != dir.array) {
		*slash = 0;
		if (os_mkdirs(dir.array) == MKDIR_ERROR) {
			obs_log(LOG_WARNING, "sr_save: could not create the save folder '%s'", dir.array);
			ok = false;
		}
	}

	dstr_free(&dir);
	return ok;
}

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
		/* the save folder may not exist yet (a fresh default, a folder
		 * typed in or removed since): create it rather than fail */
		if (!ensure_parent_dir(path)) {
			avformat_free_context(oc);
			return false;
		}
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

	/* a file has to open on a keyframe: anything before the first one
	 * cannot be decoded on its own */
	size_t start = 0;
	while (start < r->video.num && !(r->video.array[start].pkt->flags & AV_PKT_FLAG_KEY))
		start++;
	if (start >= r->video.num)
		start = 0;

	const int64_t first = (int64_t)r->video.array[start].ts;
	bool ok = true;

	for (size_t i = start; i < r->video.num && ok; i++) {
		AVPacket *pkt = av_packet_clone(r->video.array[i].pkt);
		if (!pkt) {
			ok = false;
			break;
		}
		pkt->stream_index = st->index;

		const int64_t pts_ns = (int64_t)r->video.array[i].ts - first;
		int64_t dur_ns;
		if (i + 1 < r->video.num)
			dur_ns = (int64_t)r->video.array[i + 1].ts - (int64_t)r->video.array[i].ts;
		else
			dur_ns = (i > start) ? (int64_t)r->video.array[i].ts - (int64_t)r->video.array[i - 1].ts
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
		obs_log(LOG_INFO, "sr_save: wrote replay to '%s' (%zu frames)", path, r->video.num - start);
	return ok;
}
