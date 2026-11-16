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

/* Unit tests for the safeguards in sr-guard.c. A simulated replay buffer is
 * fed packets the way the capture filter does and must stay bounded under
 * the failures seen in the field: an encoder that ignores the keyframe
 * interval (AMD AMF in its low-latency usages), sources whose clock jumps,
 * and several buffers outgrowing the machine's memory. */

#include "sr-guard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_PER_SEC 1000000000ull
#define MB (1024ull * 1024ull)

static int failures;
static int checks;

#define CHECK(cond)                                                                  \
	do {                                                                         \
		checks++;                                                            \
		if (!(cond)) {                                                       \
			failures++;                                                  \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                    \
	} while (0)

/* --- a simulated buffer, evicting exactly like sr_buffer_push_video ------- */

struct sim {
	struct sr_guard_pkt *pkts;
	size_t count;
	size_t cap;
	uint64_t bytes;
	uint64_t duration_ns;
	size_t restarts; /* drop_all results */
	unsigned trims;  /* sr_guard_trim bits seen */
};

static void sim_get(void *ctx, size_t index, struct sr_guard_pkt *out)
{
	const struct sim *s = ctx;
	*out = s->pkts[index];
}

static void sim_init(struct sim *s, uint64_t duration_ns)
{
	memset(s, 0, sizeof(*s));
	s->duration_ns = duration_ns;
}

static void sim_free(struct sim *s)
{
	free(s->pkts);
}

static uint64_t sim_span(const struct sim *s)
{
	return s->count > 1 ? s->pkts[s->count - 1].ts - s->pkts[0].ts : 0;
}

/* Pushes one packet; returns true if the buffer asked for an encoder
 * restart (drop_all). */
static bool sim_push(struct sim *s, uint64_t ts, uint64_t size, bool key, uint64_t bytes_limit)
{
	if (!s->count && !key)
		return false; /* a buffer must open with a keyframe */

	if (s->count == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 256;
		s->pkts = realloc(s->pkts, s->cap * sizeof(*s->pkts));
	}
	s->pkts[s->count++] = (struct sr_guard_pkt){.ts = ts, .size = size, .key = key};
	s->bytes += size;

	const struct sr_guard_eviction ev = sr_guard_plan_eviction(
		sim_get, s, s->count, ts, s->duration_ns, sr_guard_max_span_ns(s->duration_ns), s->bytes, bytes_limit);
	s->trims |= ev.trim;

	for (size_t i = 0; i < ev.drop; i++)
		s->bytes -= s->pkts[i].size;
	memmove(s->pkts, s->pkts + ev.drop, (s->count - ev.drop) * sizeof(*s->pkts));
	s->count -= ev.drop;

	if (ev.drop_all) {
		CHECK(s->count == 0);
		s->restarts++;
	}
	return ev.drop_all;
}

/* --- tests ---------------------------------------------------------------- */

/* A well-behaved encoder: GOP of 15 at 60 fps, 15 s buffer. The buffer
 * holds the configured time plus at most one GOP, and no safeguard fires. */
static void test_normal_gop(void)
{
	struct sim s;
	sim_init(&s, 15 * NS_PER_SEC);
	const uint64_t frame = NS_PER_SEC / 60;

	for (uint64_t i = 0; i < 60 * 600; i++)
		sim_push(&s, 1000 + i * frame, 100000, i % 15 == 0, UINT64_MAX);

	CHECK(s.trims == SR_TRIM_NONE);
	CHECK(s.restarts == 0);
	CHECK(sim_span(&s) >= 15 * NS_PER_SEC);
	CHECK(sim_span(&s) <= 15 * NS_PER_SEC + 15 * frame);
	CHECK(s.pkts[0].key);
	sim_free(&s);
}

/* All-intra: every packet is a keyframe, the buffer is exactly the window. */
static void test_all_intra(void)
{
	struct sim s;
	sim_init(&s, 7 * NS_PER_SEC);
	const uint64_t frame = NS_PER_SEC / 30;

	for (uint64_t i = 0; i < 30 * 120; i++)
		sim_push(&s, i * frame, 50000, true, UINT64_MAX);

	CHECK(s.trims == SR_TRIM_NONE);
	CHECK(sim_span(&s) <= 7 * NS_PER_SEC + frame);
	sim_free(&s);
}

/* The 31/10 failure: an encoder that emits one keyframe and then only P
 * frames. Before the safeguards the buffer held the whole show (99 minutes,
 * 49 GB). Now it is bounded by the span limit even with no watchdog. */
static void test_no_keyframes_is_bounded(void)
{
	struct sim s;
	sim_init(&s, 15 * NS_PER_SEC);
	const uint64_t frame = NS_PER_SEC / 60;
	const uint64_t max_span = sr_guard_max_span_ns(15 * NS_PER_SEC);
	bool restart_pending = false;
	uint64_t worst_span = 0;

	for (uint64_t i = 0; i < 60 * 3600; i++) {
		/* like the filter: after drop_all the encoder restarts, and a
		 * fresh encoder opens with a keyframe */
		const bool key = (i == 0) || restart_pending;
		restart_pending = sim_push(&s, i * frame, 200000, key, UINT64_MAX);
		if (sim_span(&s) > worst_span)
			worst_span = sim_span(&s);
	}

	CHECK(worst_span <= max_span + frame);
	CHECK(s.restarts > 0);
	CHECK(s.trims & SR_TRIM_SPAN);
	sim_free(&s);
}

/* Over its share of the memory budget, a buffer sheds whole GOPs - and only
 * as many as needed - so it stays decodable from its first packet. */
static void test_memory_limit_trims_whole_gops(void)
{
	struct sim s;
	sim_init(&s, 60 * NS_PER_SEC);
	const uint64_t frame = NS_PER_SEC / 60;
	const uint64_t limit = 300 * MB;

	for (uint64_t i = 0; i < 60 * 60; i++)
		sim_push(&s, i * frame, 1 * MB, i % 30 == 0, limit);

	CHECK(s.bytes <= limit);
	CHECK(s.bytes > limit - 30 * MB); /* trimmed one GOP at a time */
	CHECK(s.pkts[0].key);
	CHECK(s.trims & SR_TRIM_MEMORY);
	CHECK(s.restarts == 0);
	sim_free(&s);
}

/* A single GOP that alone exceeds the limit cannot be cut: clear it. */
static void test_single_gop_over_limit_restarts(void)
{
	struct sim s;
	sim_init(&s, 60 * NS_PER_SEC);
	bool restarted = false;
	for (uint64_t i = 0; i < 20 && !restarted; i++)
		restarted = sim_push(&s, i * 1000, 10 * MB, i == 0, 50 * MB);
	CHECK(restarted);
	CHECK(s.count == 0 && s.bytes == 0);
	sim_free(&s);
}

/* A source that pauses and resumes far ahead (feed lost for minutes): the
 * stale frames go, the buffer does not keep them. */
static void test_clock_jump_forward(void)
{
	struct sim s;
	sim_init(&s, 10 * NS_PER_SEC);
	const uint64_t frame = NS_PER_SEC / 60;
	uint64_t ts = 0;
	bool restart_pending = false;

	for (int i = 0; i < 600; i++, ts += frame)
		restart_pending = sim_push(&s, ts, 1000, i % 15 == 0, UINT64_MAX);

	ts += 300 * NS_PER_SEC;
	for (int i = 0; i < 600; i++, ts += frame) {
		const bool key = (i % 15 == 0) || restart_pending;
		restart_pending = sim_push(&s, ts, 1000, key, UINT64_MAX);
	}

	CHECK(sim_span(&s) <= 10 * NS_PER_SEC + 15 * frame);
	CHECK(s.pkts[0].key);
	sim_free(&s);
}

static void test_non_key_into_empty_buffer_is_discarded(void)
{
	struct sim s;
	sim_init(&s, NS_PER_SEC);
	sim_push(&s, 0, 10, false, UINT64_MAX);
	CHECK(s.count == 0);
	sim_push(&s, 1, 10, true, UINT64_MAX);
	CHECK(s.count == 1);
	sim_free(&s);
}

static void test_memory_budget(void)
{
	CHECK(sr_guard_memory_budget(0) == 2048 * MB);
	CHECK(sr_guard_memory_budget(32ull * 1024 * MB) == 8192 * MB);
	CHECK(sr_guard_memory_budget(1024 * MB) == 512 * MB);
	CHECK(sr_guard_memory_budget(256ull * 1024 * MB) == 16384 * MB);

	CHECK(sr_guard_buffer_limit(1000, 2000, 4) == UINT64_MAX);
	CHECK(sr_guard_buffer_limit(3000, 2000, 4) == 500);
	CHECK(sr_guard_buffer_limit(3000, 2000, 0) == UINT64_MAX);
}

/* Five buffers sharing an 8 GB budget, one of them never cut by keyframes
 * fast enough: together they stay near the budget. */
static void test_shared_budget(void)
{
	enum { N = 5 };
	struct sim s[N];
	const uint64_t budget = 2048 * MB;
	const uint64_t frame = NS_PER_SEC / 60;

	for (int b = 0; b < N; b++)
		sim_init(&s[b], 120 * NS_PER_SEC);

	uint64_t worst_total = 0;
	for (uint64_t i = 0; i < 60 * 120; i++) {
		for (int b = 0; b < N; b++) {
			uint64_t total = 0;
			for (int k = 0; k < N; k++)
				total += s[k].bytes;
			const uint64_t limit = sr_guard_buffer_limit(total, budget, N);
			/* buffer 0 is the hog: big frames */
			sim_push(&s[b], i * frame, b == 0 ? 2 * MB : 300000, i % 15 == 0, limit);
		}
		uint64_t total = 0;
		for (int k = 0; k < N; k++)
			total += s[k].bytes;
		if (total > worst_total)
			worst_total = total;
	}

	/* overshoot bounded by one GOP per buffer */
	CHECK(worst_total <= budget + N * 15 * 2 * MB);
	CHECK(s[0].trims & SR_TRIM_MEMORY);
	for (int b = 0; b < N; b++)
		sim_free(&s[b]);
}

static void test_backend_order(void)
{
	enum sr_encoder_backend out[4];
	size_t n;

	n = sr_guard_backend_order(SR_ENC_AUTO, 0, out);
	CHECK(n == 4 && out[0] == SR_ENC_NVENC && out[1] == SR_ENC_AMF && out[2] == SR_ENC_QSV &&
	      out[3] == SR_ENC_X264);

	n = sr_guard_backend_order(SR_ENC_AMF, 0, out);
	CHECK(n == 4 && out[0] == SR_ENC_AMF && out[1] == SR_ENC_NVENC && out[3] == SR_ENC_X264);

	/* AMF misbehaved: never retried, the next one takes over */
	n = sr_guard_backend_order(SR_ENC_AMF, SR_BACKEND_BIT(SR_ENC_AMF), out);
	CHECK(n == 3 && out[0] == SR_ENC_NVENC);

	const uint32_t all = SR_BACKEND_BIT(SR_ENC_NVENC) | SR_BACKEND_BIT(SR_ENC_AMF) | SR_BACKEND_BIT(SR_ENC_QSV) |
			     SR_BACKEND_BIT(SR_ENC_X264);
	CHECK(sr_guard_backend_order(SR_ENC_AUTO, all, out) == 0);
}

static void test_watch_keyframes(void)
{
	struct sr_encoder_watch w;
	sr_encoder_watch_init(&w, 15, 60.0);

	/* a healthy GOP-15 stream never trips */
	for (int i = 0; i < 6000; i++) {
		CHECK(sr_encoder_watch_sent(&w, true) == SR_ENCODER_OK);
		CHECK(sr_encoder_watch_packet(&w, i % 15 == 0) == SR_ENCODER_OK);
	}

	/* AMF low-latency: one keyframe, then P frames forever. Trips within
	 * about two seconds at 60 fps. */
	sr_encoder_watch_init(&w, 15, 60.0);
	int tripped_at = -1;
	for (int i = 0; i < 6000 && tripped_at < 0; i++) {
		sr_encoder_watch_sent(&w, true);
		if (sr_encoder_watch_packet(&w, i == 0) == SR_ENCODER_NO_KEYFRAMES)
			tripped_at = i;
	}
	CHECK(tripped_at > 15 && tripped_at <= 130);
}

static void test_watch_errors_and_stalls(void)
{
	struct sr_encoder_watch w;
	sr_encoder_watch_init(&w, 15, 60.0);

	/* occasional rejected frames are tolerated */
	for (int i = 0; i < 1000; i++) {
		CHECK(sr_encoder_watch_sent(&w, i % 10 != 0) == SR_ENCODER_OK);
		sr_encoder_watch_packet(&w, i % 15 == 0);
	}

	enum sr_guard_verdict v = SR_ENCODER_OK;
	int n = 0;
	while (v == SR_ENCODER_OK && n < 1000) {
		v = sr_encoder_watch_sent(&w, false);
		n++;
	}
	CHECK(v == SR_ENCODER_ERRORS && n <= 31);

	/* accepts frames, never returns packets */
	sr_encoder_watch_init(&w, 15, 60.0);
	v = SR_ENCODER_OK;
	n = 0;
	while (v == SR_ENCODER_OK && n < 1000) {
		v = sr_encoder_watch_sent(&w, true);
		n++;
	}
	CHECK(v == SR_ENCODER_STALLED && n <= 181);

	/* an encoder with a few frames of delay is fine */
	sr_encoder_watch_init(&w, 15, 60.0);
	for (int i = 0; i < 1000; i++) {
		CHECK(sr_encoder_watch_sent(&w, true) == SR_ENCODER_OK);
		if (i >= 8)
			CHECK(sr_encoder_watch_packet(&w, (i - 8) % 15 == 0) == SR_ENCODER_OK);
	}
}

int main(void)
{
	test_normal_gop();
	test_all_intra();
	test_no_keyframes_is_bounded();
	test_memory_limit_trims_whole_gops();
	test_single_gop_over_limit_restarts();
	test_clock_jump_forward();
	test_non_key_into_empty_buffer_is_discarded();
	test_memory_budget();
	test_shared_budget();
	test_backend_order();
	test_watch_keyframes();
	test_watch_errors_and_stalls();

	printf("%d checks, %d failed\n", checks, failures);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
