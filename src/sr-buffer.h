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
#include <util/deque.h>
#include <util/darray.h>
#include <util/threading.h>

#include <libavcodec/avcodec.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One encoded video frame kept in the ring buffer. Because every frame is
 * encoded as an intra frame, any packet can be decoded on its own. */
struct sr_packet {
	AVPacket *pkt;
	uint64_t ts; /* OBS timestamp in nanoseconds */
};

/* One chunk of captured audio, float planar. */
struct sr_audio_chunk {
	float *data[MAX_AV_PLANES];
	uint32_t frames;
	uint64_t ts; /* OBS timestamp in nanoseconds */
};

/* Ring buffer holding the last N seconds of encoded video and raw audio.
 * Owned by the capture filter; the playback source only takes snapshots. */
struct sr_buffer {
	pthread_mutex_t mutex;
	struct deque video; /* struct sr_packet */
	struct deque audio; /* struct sr_audio_chunk */
	uint64_t duration_ns;

	/* video stream parameters required to decode the packets later */
	enum AVCodecID codec_id;
	uint32_t width;
	uint32_t height;
	uint8_t *extradata; /* SPS/PPS header */
	int extradata_size;

	/* audio stream parameters */
	uint32_t samples_per_sec;
	enum speaker_layout speakers;
};

/* A frozen copy of the ring buffer, taken when a replay is triggered. */
struct sr_replay {
	DARRAY(struct sr_packet) video;
	DARRAY(struct sr_audio_chunk) audio;
	uint64_t first_ts;
	uint64_t last_ts;
	enum AVCodecID codec_id;
	uint32_t width;
	uint32_t height;
	uint8_t *extradata; /* SPS/PPS header (owned) */
	int extradata_size;
	uint32_t samples_per_sec;
	enum speaker_layout speakers;
};

void sr_buffer_init(struct sr_buffer *b);
void sr_buffer_free(struct sr_buffer *b);
void sr_buffer_clear(struct sr_buffer *b);

/* Stores a copy of the codec header (SPS/PPS). */
void sr_buffer_set_extradata(struct sr_buffer *b, const uint8_t *data, int size);

/* Takes ownership of pkt. Evicts old packets past duration_ns. */
void sr_buffer_push_video(struct sr_buffer *b, AVPacket *pkt, uint64_t ts);
void sr_buffer_push_audio(struct sr_buffer *b, const struct obs_audio_data *audio, uint32_t channels);

/* Copies the current buffer contents into out (packets are ref-counted
 * clones, audio is duplicated). Returns false if there is no video yet. */
bool sr_buffer_snapshot(struct sr_buffer *b, struct sr_replay *out);

void sr_replay_free(struct sr_replay *r);

/* Approximate memory used by stored packets, for the stats log. */
size_t sr_buffer_video_bytes(struct sr_buffer *b);

#ifdef __cplusplus
}
#endif
