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

#include "sr-guard.h"

#define MB (1024ull * 1024ull)
#define NS_PER_SEC 1000000000ull

size_t sr_guard_backend_order(enum sr_encoder_backend requested, uint32_t skip_mask, enum sr_encoder_backend out[4])
{
	static const enum sr_encoder_backend fallback[] = {SR_ENC_NVENC, SR_ENC_AMF, SR_ENC_QSV, SR_ENC_X264};
	size_t n = 0;

	if (requested != SR_ENC_AUTO && !(skip_mask & SR_BACKEND_BIT(requested)))
		out[n++] = requested;

	for (size_t i = 0; i < sizeof(fallback) / sizeof(fallback[0]); i++) {
		const enum sr_encoder_backend b = fallback[i];
		if (b == requested || (skip_mask & SR_BACKEND_BIT(b)))
			continue;
		out[n++] = b;
	}
	return n;
}

uint64_t sr_guard_max_span_ns(uint64_t duration_ns)
{
	return duration_ns + duration_ns / 2 + 2 * NS_PER_SEC;
}

static struct sr_guard_pkt pkt_at(sr_guard_pkt_fn get, void *ctx, size_t i)
{
	struct sr_guard_pkt p = {0};
	get(ctx, i, &p);
	return p;
}

/* Index of the first keyframe after start, or count if there is none. */
static size_t next_keyframe(sr_guard_pkt_fn get, void *ctx, size_t start, size_t count)
{
	for (size_t i = start + 1; i < count; i++) {
		if (pkt_at(get, ctx, i).key)
			return i;
	}
	return count;
}

struct sr_guard_eviction sr_guard_plan_eviction(sr_guard_pkt_fn get, void *ctx, size_t count, uint64_t now,
						uint64_t duration_ns, uint64_t max_span_ns, uint64_t bytes_total,
						uint64_t bytes_limit)
{
	struct sr_guard_eviction ev = {0};
	if (!count)
		return ev;

	size_t front = 0;
	uint64_t bytes = bytes_total;

	for (;;) {
		const size_t next = next_keyframe(get, ctx, front, count);

		const struct sr_guard_pkt first = pkt_at(get, ctx, front);
		const bool over_span = now > first.ts && now - first.ts > max_span_ns;
		const bool over_bytes = bytes > bytes_limit;

		bool expired = false;
		if (next < count) {
			/* the GOP ending right before the next keyframe is only
			 * expendable once its last frame left the window */
			const struct sr_guard_pkt last = pkt_at(get, ctx, next - 1);
			expired = now > last.ts && now - last.ts > duration_ns;
		}

		if (!expired && !over_span && !over_bytes)
			break;

		if (next >= count) {
			/* a single GOP left and still over a limit: nothing
			 * decodable can be kept within bounds */
			if (over_span || over_bytes) {
				ev.drop_all = true;
				ev.drop = count;
				ev.trim |= over_span ? SR_TRIM_SPAN : 0;
				ev.trim |= over_bytes ? SR_TRIM_MEMORY : 0;
			}
			break;
		}

		if (!expired) {
			ev.trim |= over_span ? SR_TRIM_SPAN : 0;
			ev.trim |= over_bytes ? SR_TRIM_MEMORY : 0;
		}

		for (size_t i = front; i < next; i++) {
			const uint64_t size = pkt_at(get, ctx, i).size;
			bytes = bytes > size ? bytes - size : 0;
		}
		front = next;
	}

	if (!ev.drop_all)
		ev.drop = front;
	return ev;
}

uint64_t sr_guard_memory_budget(uint64_t total_ram)
{
	if (!total_ram)
		return 2048 * MB;

	uint64_t budget = total_ram / 4;
	if (budget < 512 * MB)
		budget = 512 * MB;
	if (budget > 16384 * MB)
		budget = 16384 * MB;
	return budget;
}

uint64_t sr_guard_buffer_limit(uint64_t total_bytes, uint64_t budget, size_t buffer_count)
{
	if (total_bytes <= budget || !buffer_count)
		return UINT64_MAX;
	return budget / buffer_count;
}

void sr_encoder_watch_init(struct sr_encoder_watch *w, int keyint, double fps)
{
	if (keyint < 1)
		keyint = 1;
	if (fps <= 0.0)
		fps = 60.0;

	const uint32_t one_second = (uint32_t)(fps + 0.5);

	w->key_limit = (uint32_t)keyint * 4 + one_second;
	w->error_limit = one_second / 2 > 15 ? one_second / 2 : 15;
	w->stall_limit = one_second * 3;
	w->since_key = 0;
	w->errors = 0;
	w->since_packet = 0;
}

enum sr_guard_verdict sr_encoder_watch_sent(struct sr_encoder_watch *w, bool ok)
{
	if (!ok) {
		if (++w->errors >= w->error_limit)
			return SR_ENCODER_ERRORS;
		return SR_ENCODER_OK;
	}

	w->errors = 0;
	if (++w->since_packet >= w->stall_limit)
		return SR_ENCODER_STALLED;
	return SR_ENCODER_OK;
}

enum sr_guard_verdict sr_encoder_watch_packet(struct sr_encoder_watch *w, bool key)
{
	w->since_packet = 0;
	if (key) {
		w->since_key = 0;
		return SR_ENCODER_OK;
	}
	if (++w->since_key >= w->key_limit)
		return SR_ENCODER_NO_KEYFRAMES;
	return SR_ENCODER_OK;
}
