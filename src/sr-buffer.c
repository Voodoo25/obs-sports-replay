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

#include <util/platform.h>

/* Bookkeeping shared by every replay buffer, so that together they stay
 * within a memory budget sized to the machine. Lock order: a buffer's mutex,
 * then this one. */
static pthread_mutex_t global_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t global_bytes;
static size_t global_buffers;
static uint64_t global_budget;

static void global_add_bytes(uint64_t bytes)
{
	pthread_mutex_lock(&global_mutex);
	global_bytes += bytes;
	pthread_mutex_unlock(&global_mutex);
}

static void global_sub_bytes(uint64_t bytes)
{
	pthread_mutex_lock(&global_mutex);
	global_bytes = global_bytes > bytes ? global_bytes - bytes : 0;
	pthread_mutex_unlock(&global_mutex);
}

uint64_t sr_buffers_total_bytes(void)
{
	pthread_mutex_lock(&global_mutex);
	const uint64_t bytes = global_bytes;
	pthread_mutex_unlock(&global_mutex);
	return bytes;
}

uint64_t sr_buffers_budget(void)
{
	pthread_mutex_lock(&global_mutex);
	if (!global_budget)
		global_budget = sr_guard_memory_budget(os_get_sys_total_size());
	const uint64_t budget = global_budget;
	pthread_mutex_unlock(&global_mutex);
	return budget;
}

/* Byte limit for one buffer right now: unlimited while all buffers together
 * fit the budget, its fair share once they do not. */
static uint64_t current_buffer_limit(void)
{
	const uint64_t budget = sr_buffers_budget();
	pthread_mutex_lock(&global_mutex);
	const uint64_t limit = sr_guard_buffer_limit(global_bytes, budget, global_buffers);
	pthread_mutex_unlock(&global_mutex);
	return limit;
}

void sr_buffer_init(struct sr_buffer *b)
{
	memset(b, 0, sizeof(*b));
	pthread_mutex_init(&b->mutex, NULL);

	pthread_mutex_lock(&global_mutex);
	global_buffers++;
	pthread_mutex_unlock(&global_mutex);
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

/* Caller holds b->mutex. */
static void drop_video_front(struct sr_buffer *b, size_t count)
{
	uint64_t freed = 0;
	for (size_t i = 0; i < count && b->video.size; i++) {
		struct sr_packet p;
		deque_pop_front(&b->video, &p, sizeof(p));
		freed += (uint64_t)p.pkt->size;
		free_video_packet(&p);
	}
	b->video_bytes = b->video_bytes > freed ? b->video_bytes - freed : 0;
	global_sub_bytes(freed);
}

void sr_buffer_clear(struct sr_buffer *b)
{
	pthread_mutex_lock(&b->mutex);
	drop_video_front(b, SIZE_MAX);
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

	pthread_mutex_lock(&global_mutex);
	if (global_buffers)
		global_buffers--;
	pthread_mutex_unlock(&global_mutex);

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

static struct sr_packet *video_at(struct sr_buffer *b, size_t idx)
{
	return deque_data(&b->video, idx * sizeof(struct sr_packet));
}

static bool is_keyframe(const struct sr_packet *p)
{
	return (p->pkt->flags & AV_PKT_FLAG_KEY) != 0;
}

static void guard_pkt_at(void *ctx, size_t index, struct sr_guard_pkt *out)
{
	const struct sr_packet *p = video_at(ctx, index);
	out->ts = p->ts;
	out->size = (uint64_t)p->pkt->size;
	out->key = is_keyframe(p);
}

struct sr_guard_eviction sr_buffer_push_video(struct sr_buffer *b, AVPacket *pkt, uint64_t ts)
{
	struct sr_guard_eviction ev = {0};
	struct sr_packet entry = {.pkt = pkt, .ts = ts};
	const uint64_t limit = current_buffer_limit();

	pthread_mutex_lock(&b->mutex);

	/* a buffer must open with a keyframe, or nothing in it decodes */
	if (!b->video.size && !is_keyframe(&entry)) {
		pthread_mutex_unlock(&b->mutex);
		free_video_packet(&entry);
		return ev;
	}

	deque_push_back(&b->video, &entry, sizeof(entry));
	b->video_bytes += (uint64_t)pkt->size;
	global_add_bytes((uint64_t)pkt->size);

	const size_t count = b->video.size / sizeof(struct sr_packet);
	ev = sr_guard_plan_eviction(guard_pkt_at, b, count, ts, b->duration_ns, sr_guard_max_span_ns(b->duration_ns),
				    b->video_bytes, limit);
	if (ev.drop)
		drop_video_front(b, ev.drop);

	pthread_mutex_unlock(&b->mutex);
	return ev;
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

	/* the source restarted and its clock went backwards: the buffered audio
	 * would never expire against the new timeline */
	if (b->audio.size) {
		struct sr_audio_chunk back;
		deque_peek_back(&b->audio, &back, sizeof(back));
		if (chunk.ts < back.ts) {
			while (b->audio.size) {
				struct sr_audio_chunk old;
				deque_pop_front(&b->audio, &old, sizeof(old));
				free_audio_chunk(&old);
			}
		}
	}

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
	pthread_mutex_lock(&b->mutex);
	const size_t bytes = (size_t)b->video_bytes;
	pthread_mutex_unlock(&b->mutex);
	return bytes;
}

uint64_t sr_buffer_video_span_ns(struct sr_buffer *b)
{
	uint64_t span = 0;
	pthread_mutex_lock(&b->mutex);
	const size_t count = b->video.size / sizeof(struct sr_packet);
	if (count > 1) {
		const uint64_t first = video_at(b, 0)->ts;
		const uint64_t last = video_at(b, count - 1)->ts;
		span = last > first ? last - first : 0;
	}
	pthread_mutex_unlock(&b->mutex);
	return span;
}
