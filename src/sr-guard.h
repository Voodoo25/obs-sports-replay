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

/* Safeguards that keep a misbehaving encoder, driver or source from
 * escalating into the live show: a replay buffer must never grow without
 * bound, and an encoder that does not deliver what was asked of it is
 * replaced instead of trusted.
 *
 * Everything here is plain C with no OBS or FFmpeg dependency, so the
 * decisions can be unit tested on any platform (see tests/). The buffer and
 * the capture filter only carry them out. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sr_encoder_backend {
	SR_ENC_AUTO = 0,
	SR_ENC_NVENC,
	SR_ENC_AMF,
	SR_ENC_QSV,
	SR_ENC_X264,
};

#define SR_BACKEND_BIT(b) (1u << (unsigned)(b))

/* Order in which encoders are tried: the requested one first (unless it is
 * AUTO), then hardware encoders, then libx264. Backends whose bit is set in
 * skip_mask are left out - they already failed on this source. Returns the
 * number of entries written to out (at most 4). */
size_t sr_guard_backend_order(enum sr_encoder_backend requested, uint32_t skip_mask, enum sr_encoder_backend out[4]);

/* --- replay buffer limits ------------------------------------------------ */

/* What the eviction planner needs to know about one stored video packet. */
struct sr_guard_pkt {
	uint64_t ts;   /* nanoseconds */
	uint64_t size; /* bytes */
	bool key;
};

typedef void (*sr_guard_pkt_fn)(void *ctx, size_t index, struct sr_guard_pkt *out);

/* Why packets are dropped beyond normal expiry. */
enum sr_guard_trim {
	SR_TRIM_NONE = 0,
	SR_TRIM_SPAN = 1,   /* the buffer holds far more time than configured */
	SR_TRIM_MEMORY = 2, /* the buffer is over its share of the memory budget */
};

struct sr_guard_eviction {
	size_t drop;   /* packets to drop from the front */
	bool drop_all; /* nothing decodable can be kept: clear and restart */
	unsigned trim; /* sr_guard_trim bits that forced extra drops */
};

/* Longest stretch of time a buffer may hold before it is cut regardless of
 * keyframes. Normal operation holds the configured duration plus at most one
 * GOP, well inside this. */
uint64_t sr_guard_max_span_ns(uint64_t duration_ns);

/* Decides which packets to drop from the front of a buffer of count packets
 * (oldest first), given the newest timestamp now. Packets are only dropped in
 * whole GOPs, because a frame is decodable only from its keyframe on:
 *  1. GOPs whose last frame is older than duration_ns expire normally;
 *  2. while the buffer spans more than max_span_ns, or holds more than
 *     bytes_limit, the oldest GOP is dropped as well;
 *  3. if a limit is still exceeded with a single GOP left - an encoder that
 *     stopped producing keyframes - drop_all is set. */
struct sr_guard_eviction sr_guard_plan_eviction(sr_guard_pkt_fn get, void *ctx, size_t count, uint64_t now,
						uint64_t duration_ns, uint64_t max_span_ns, uint64_t bytes_total,
						uint64_t bytes_limit);

/* Memory all replay buffers together may use: a quarter of the machine's
 * RAM, within [512 MB, 16 GB]. total_ram 0 (unknown) gives 2 GB. */
uint64_t sr_guard_memory_budget(uint64_t total_ram);

/* Byte limit for one of buffer_count buffers: unlimited while the total is
 * within budget, its fair share of the budget once the total exceeds it. */
uint64_t sr_guard_buffer_limit(uint64_t total_bytes, uint64_t budget, size_t buffer_count);

/* --- encoder watchdog ---------------------------------------------------- */

enum sr_guard_verdict {
	SR_ENCODER_OK = 0,
	SR_ENCODER_NO_KEYFRAMES, /* ignores the keyframe interval */
	SR_ENCODER_ERRORS,       /* keeps failing to accept frames */
	SR_ENCODER_STALLED,      /* accepts frames but returns no packets */
};

struct sr_encoder_watch {
	uint32_t key_limit;   /* packets allowed without a keyframe */
	uint32_t error_limit; /* consecutive failed sends allowed */
	uint32_t stall_limit; /* sent frames allowed without any packet */

	uint32_t since_key;
	uint32_t errors;
	uint32_t since_packet;
};

/* keyint: the interval the encoder was asked for, in frames. fps: the rate
 * frames are fed at (0 if unknown, treated as 60). */
void sr_encoder_watch_init(struct sr_encoder_watch *w, int keyint, double fps);

/* Report one frame sent to the encoder (ok false: the encoder rejected it). */
enum sr_guard_verdict sr_encoder_watch_sent(struct sr_encoder_watch *w, bool ok);

/* Report one packet received from the encoder. */
enum sr_guard_verdict sr_encoder_watch_packet(struct sr_encoder_watch *w, bool key);

#ifdef __cplusplus
}
#endif
