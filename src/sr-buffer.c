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

#include "sr-buffer.h"

void sr_buffer_init(struct sr_buffer *b)
{
	memset(b, 0, sizeof(*b));
	pthread_mutex_init(&b->mutex, NULL);
}

static void free_video_packet(struct sr_packet *p)
{
	av_packet_free(&p->pkt);
}

static void free_audio_chunk(struct sr_audio_chunk *c)
{
	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		bfree(c->data[i]);
		c->data[i] = NULL;
	}
}

void sr_buffer_clear(struct sr_buffer *b)
{
	pthread_mutex_lock(&b->mutex);
	while (b->video.size) {
		struct sr_packet p;
		deque_pop_front(&b->video, &p, sizeof(p));
		free_video_packet(&p);
	}
	while (b->audio.size) {
		struct sr_audio_chunk c;
		deque_pop_front(&b->audio, &c, sizeof(c));
		free_audio_chunk(&c);
	}
	pthread_mutex_unlock(&b->mutex);
}

void sr_buffer_free(struct sr_buffer *b)
{
	sr_buffer_clear(b);
	deque_free(&b->video);
	deque_free(&b->audio);
	bfree(b->extradata);
	b->extradata = NULL;
	pthread_mutex_destroy(&b->mutex);
}

void sr_buffer_set_extradata(struct sr_buffer *b, const uint8_t *data, int size)
{
	pthread_mutex_lock(&b->mutex);
	bfree(b->extradata);
	b->extradata = NULL;
	b->extradata_size = 0;
	if (data && size > 0) {
		b->extradata = bmemdup(data, (size_t)size);
		b->extradata_size = size;
	}
	pthread_mutex_unlock(&b->mutex);
}

void sr_buffer_push_video(struct sr_buffer *b, AVPacket *pkt, uint64_t ts)
{
	struct sr_packet entry = {.pkt = pkt, .ts = ts};

	pthread_mutex_lock(&b->mutex);
	deque_push_back(&b->video, &entry, sizeof(entry));

	struct sr_packet front;
	deque_peek_front(&b->video, &front, sizeof(front));
	while (b->video.size > sizeof(entry) && ts > front.ts && ts - front.ts > b->duration_ns) {
		deque_pop_front(&b->video, &front, sizeof(front));
		free_video_packet(&front);
		deque_peek_front(&b->video, &front, sizeof(front));
	}
	pthread_mutex_unlock(&b->mutex);
}

void sr_buffer_push_audio(struct sr_buffer *b, const struct obs_audio_data *audio, uint32_t channels)
{
	struct sr_audio_chunk chunk = {0};
	chunk.frames = audio->frames;
	chunk.ts = audio->timestamp;
	for (uint32_t i = 0; i < channels && i < MAX_AV_PLANES; i++) {
		if (!audio->data[i])
			break;
		chunk.data[i] = bmemdup(audio->data[i], audio->frames * sizeof(float));
	}

	pthread_mutex_lock(&b->mutex);
	deque_push_back(&b->audio, &chunk, sizeof(chunk));

	struct sr_audio_chunk front;
	deque_peek_front(&b->audio, &front, sizeof(front));
	while (b->audio.size > sizeof(chunk) && chunk.ts > front.ts && chunk.ts - front.ts > b->duration_ns) {
		deque_pop_front(&b->audio, &front, sizeof(front));
		free_audio_chunk(&front);
		deque_peek_front(&b->audio, &front, sizeof(front));
	}
	pthread_mutex_unlock(&b->mutex);
}

bool sr_buffer_snapshot(struct sr_buffer *b, struct sr_replay *out)
{
	memset(out, 0, sizeof(*out));

	pthread_mutex_lock(&b->mutex);

	const size_t video_count = b->video.size / sizeof(struct sr_packet);
	if (!video_count) {
		pthread_mutex_unlock(&b->mutex);
		return false;
	}

	da_reserve(out->video, video_count);
	for (size_t i = 0; i < video_count; i++) {
		struct sr_packet *src = deque_data(&b->video, i * sizeof(struct sr_packet));
		struct sr_packet copy = {.pkt = av_packet_clone(src->pkt), .ts = src->ts};
		if (copy.pkt)
			da_push_back(out->video, &copy);
	}

	const size_t audio_count = b->audio.size / sizeof(struct sr_audio_chunk);
	da_reserve(out->audio, audio_count);
	const uint32_t channels = get_audio_channels(b->speakers);
	for (size_t i = 0; i < audio_count; i++) {
		struct sr_audio_chunk *src = deque_data(&b->audio, i * sizeof(struct sr_audio_chunk));
		struct sr_audio_chunk copy = {0};
		copy.frames = src->frames;
		copy.ts = src->ts;
		for (uint32_t p = 0; p < channels && p < MAX_AV_PLANES; p++) {
			if (!src->data[p])
				break;
			copy.data[p] = bmemdup(src->data[p], src->frames * sizeof(float));
		}
		da_push_back(out->audio, &copy);
	}

	out->codec_id = b->codec_id;
	out->width = b->width;
	out->height = b->height;
	if (b->extradata && b->extradata_size > 0) {
		out->extradata = bmemdup(b->extradata, (size_t)b->extradata_size);
		out->extradata_size = b->extradata_size;
	}
	out->samples_per_sec = b->samples_per_sec;
	out->speakers = b->speakers;

	pthread_mutex_unlock(&b->mutex);

	if (!out->video.num) {
		sr_replay_free(out);
		return false;
	}

	out->first_ts = out->video.array[0].ts;
	out->last_ts = out->video.array[out->video.num - 1].ts;
	return true;
}

void sr_replay_free(struct sr_replay *r)
{
	for (size_t i = 0; i < r->video.num; i++)
		free_video_packet(&r->video.array[i]);
	da_free(r->video);

	for (size_t i = 0; i < r->audio.num; i++)
		free_audio_chunk(&r->audio.array[i]);
	da_free(r->audio);

	bfree(r->extradata);

	memset(r, 0, sizeof(*r));
}

size_t sr_buffer_video_bytes(struct sr_buffer *b)
{
	size_t total = 0;
	pthread_mutex_lock(&b->mutex);
	const size_t count = b->video.size / sizeof(struct sr_packet);
	for (size_t i = 0; i < count; i++) {
		struct sr_packet *p = deque_data(&b->video, i * sizeof(struct sr_packet));
		total += (size_t)p->pkt->size;
	}
	pthread_mutex_unlock(&b->mutex);
	return total;
}
