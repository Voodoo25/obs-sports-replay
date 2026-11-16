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

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include "sr-buffer.h"
#include "sr-codec.h"
#include "sr-capture.h"
#include "sr-credit.h"

#define S_DURATION "duration_ms"
#define S_ENCODER "encoder"
#define S_QUALITY "quality"
#define S_KEYINT "keyint"
#define S_FPS "capture_fps"

struct sr_capture {
	obs_source_t *self;
	struct sr_buffer buffer;
	struct sr_encoder *encoder;

	enum sr_encoder_backend backend;
	int qp;
	int keyint;
	int capture_fps; /* 0 = encode every frame the source delivers */

	/* frame rate limiter state */
	uint64_t frame_interval_ns;
	uint64_t last_kept_ts;
	bool have_last_kept;

	/* format the current encoder was opened with */
	uint32_t enc_width;
	uint32_t enc_height;
	bool encoder_failed;
	bool reset_encoder;

	/* safeguards: encoders that misbehaved on this source are skipped
	 * until its settings change; the watch judges the current one */
	uint32_t skip_backends;
	struct sr_encoder_watch watch;
	uint64_t last_trim_log; /* os_gettime_ns of the last trim warning */

	/* timestamp of the last frame the source delivered */
	uint64_t last_frame_ts;
	bool have_last_frame;

	uint64_t last_stats_log;
};

struct sr_buffer *sr_capture_get_buffer(void *capture_data)
{
	struct sr_capture *c = capture_data;
	return c ? &c->buffer : NULL;
}

static const char *sr_capture_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("SportsReplayCapture");
}

static void sr_capture_update(void *data, obs_data_t *settings)
{
	struct sr_capture *c = data;

	c->buffer.duration_ns = (uint64_t)obs_data_get_int(settings, S_DURATION) * 1000000ULL;

	const enum sr_encoder_backend backend = (enum sr_encoder_backend)obs_data_get_int(settings, S_ENCODER);
	const int qp = (int)obs_data_get_int(settings, S_QUALITY);
	const int keyint = (int)obs_data_get_int(settings, S_KEYINT);
	const int fps = (int)obs_data_get_int(settings, S_FPS);

	if (backend != c->backend || qp != c->qp || keyint != c->keyint || fps != c->capture_fps) {
		c->backend = backend;
		c->qp = qp;
		c->keyint = keyint;
		c->capture_fps = fps;
		c->frame_interval_ns = (fps > 0) ? 1000000000ULL / (uint64_t)fps : 0;
		c->have_last_kept = false;
		c->reset_encoder = true;
		c->encoder_failed = false;
		c->skip_backends = 0;
	}
}

static void *sr_capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct sr_capture *c = bzalloc(sizeof(struct sr_capture));
	c->self = source;
	sr_buffer_init(&c->buffer);
	c->backend = SR_ENC_AUTO;
	c->qp = 23;
	c->keyint = SR_DEFAULT_KEYINT;
	sr_capture_update(c, settings);
	return c;
}

static void sr_capture_destroy(void *data)
{
	struct sr_capture *c = data;
	sr_encoder_destroy(c->encoder);
	sr_buffer_free(&c->buffer);
	bfree(c);
}

/* Every capture filter carries the same default name, so log lines name the
 * source the filter is attached to. */
static const char *sr_capture_log_name(const struct sr_capture *c)
{
	obs_source_t *parent = obs_filter_get_parent(c->self);
	return parent ? obs_source_get_name(parent) : obs_source_get_name(c->self);
}

static void log_buffer_stats(struct sr_capture *c, uint64_t now)
{
	if (c->last_stats_log && now - c->last_stats_log < 60000000000ULL)
		return;
	c->last_stats_log = now;
	const double mb = 1024.0 * 1024.0;
	const size_t bytes = sr_buffer_video_bytes(&c->buffer);
	obs_log(LOG_INFO, "'%s': replay buffer using %.1f MB (%.1f s, %s; all buffers %.0f of %.0f MB)",
		sr_capture_log_name(c), (double)bytes / mb, (double)sr_buffer_video_span_ns(&c->buffer) / 1e9,
		c->encoder ? sr_encoder_name(c->encoder) : "no encoder", (double)sr_buffers_total_bytes() / mb,
		(double)sr_buffers_budget() / mb);
}

/* A safeguard cut the buffer short. Worth a warning - it means something
 * upstream misbehaves - but at most every ten seconds per source. */
static void sr_capture_report_trim(struct sr_capture *c, const struct sr_guard_eviction *ev)
{
	if (!ev->trim)
		return;

	const uint64_t now = os_gettime_ns();
	if (c->last_trim_log && now - c->last_trim_log < 10000000000ULL)
		return;
	c->last_trim_log = now;

	const char *reason = (ev->trim & SR_TRIM_MEMORY) ? "memory budget" : "time span limit";
	if (ev->drop_all)
		obs_log(LOG_WARNING, "'%s': replay buffer over its %s with no keyframe to cut at, restarting it",
			sr_capture_log_name(c), reason);
	else
		obs_log(LOG_WARNING, "'%s': replay buffer over its %s, dropped %zu old frames", sr_capture_log_name(c),
			reason, ev->drop);
}

/* The watch caught the encoder not doing its job. Trusting it further could
 * let the buffer grow without bound or starve it, so it is replaced by the
 * next backend, and never tried again on this source until the settings
 * change. */
static void sr_capture_replace_encoder(struct sr_capture *c, enum sr_guard_verdict verdict)
{
	const char *why = "misbehaved";
	switch (verdict) {
	case SR_ENCODER_NO_KEYFRAMES:
		why = "is not producing keyframes";
		break;
	case SR_ENCODER_ERRORS:
		why = "keeps rejecting frames";
		break;
	case SR_ENCODER_STALLED:
		why = "stopped returning packets";
		break;
	case SR_ENCODER_OK:
		return;
	}

	obs_log(LOG_WARNING, "'%s': replay encoder '%s' %s, switching to another encoder", sr_capture_log_name(c),
		sr_encoder_name(c->encoder), why);
	c->skip_backends |= SR_BACKEND_BIT(sr_encoder_backend(c->encoder));
	c->reset_encoder = true;
}

/* A source that restarts (a capture device reopened with new settings, a
 * feed coming back after a drop) starts its clock over. The buffered frames
 * belong to the old timeline and would never expire against the new one, so
 * the buffer grew without bound: start over with a fresh encoder, whose first
 * packet is a keyframe. */
static void sr_capture_check_clock(struct sr_capture *c, uint64_t ts)
{
	if (c->have_last_frame && ts < c->last_frame_ts) {
		obs_log(LOG_INFO, "'%s': source clock went backwards, restarting the replay buffer",
			sr_capture_log_name(c));
		c->reset_encoder = true;
		c->last_stats_log = 0;
	}
	c->have_last_frame = true;
	c->last_frame_ts = ts;
}

/* Frame rate limiter. With a capture rate configured, frames that arrive
 * ahead of the next slot never reach the encoder. The quarter-interval
 * tolerance keeps a 60 fps source asked for 30 fps from falling to 20 when
 * its timestamps jitter. */
static bool sr_capture_keep_frame(struct sr_capture *c, uint64_t ts)
{
	if (!c->frame_interval_ns)
		return true;

	/* first frame, or the source restarted and its clock went backwards */
	if (!c->have_last_kept || ts < c->last_kept_ts) {
		c->have_last_kept = true;
		c->last_kept_ts = ts;
		return true;
	}

	if (ts - c->last_kept_ts + c->frame_interval_ns / 4 < c->frame_interval_ns)
		return false;

	c->last_kept_ts = ts;
	return true;
}

static struct obs_source_frame *sr_capture_filter_video(void *data, struct obs_source_frame *frame)
{
	struct sr_capture *c = data;

	if (!frame || !frame->data[0] || c->encoder_failed)
		return frame;

	sr_capture_check_clock(c, frame->timestamp);

	if (!sr_capture_keep_frame(c, frame->timestamp))
		return frame;

	if (c->encoder && (c->reset_encoder || frame->width != c->enc_width || frame->height != c->enc_height)) {
		sr_encoder_destroy(c->encoder);
		c->encoder = NULL;
		sr_buffer_clear(&c->buffer);
	}

	if (!c->encoder) {
		uint32_t fps_num = (uint32_t)c->capture_fps;
		uint32_t fps_den = 1;
		if (!c->capture_fps) {
			struct obs_video_info ovi;
			obs_get_video_info(&ovi);
			fps_num = ovi.fps_num;
			fps_den = ovi.fps_den;
		}
		c->encoder = sr_encoder_create(sr_capture_log_name(c), frame->width, frame->height, fps_num, fps_den,
					       c->backend, c->skip_backends, c->qp, c->keyint);
		if (!c->encoder) {
			obs_log(LOG_ERROR,
				"'%s': no usable H.264 encoder left, replay capture disabled for this source",
				sr_capture_log_name(c));
			c->encoder_failed = true;
			return frame;
		}
		sr_encoder_watch_init(&c->watch, c->keyint, (double)fps_num / (double)(fps_den ? fps_den : 1));
		c->reset_encoder = false;
		c->enc_width = frame->width;
		c->enc_height = frame->height;

		pthread_mutex_lock(&c->buffer.mutex);
		c->buffer.codec_id = sr_encoder_codec_id(c->encoder);
		c->buffer.width = frame->width;
		c->buffer.height = frame->height;
		pthread_mutex_unlock(&c->buffer.mutex);

		const uint8_t *extradata = NULL;
		int extradata_size = 0;
		sr_encoder_get_extradata(c->encoder, &extradata, &extradata_size);
		sr_buffer_set_extradata(&c->buffer, extradata, extradata_size);
	}

	const enum sr_send_result sent = sr_encoder_send(c->encoder, frame);
	if (sent != SR_SEND_SKIPPED) {
		enum sr_guard_verdict verdict = sr_encoder_watch_sent(&c->watch, sent == SR_SEND_OK);

		AVPacket *pkt;
		uint64_t pkt_ts = 0;
		while (verdict == SR_ENCODER_OK && (pkt = sr_encoder_receive(c->encoder, &pkt_ts))) {
			verdict = sr_encoder_watch_packet(&c->watch, (pkt->flags & AV_PKT_FLAG_KEY) != 0);

			const struct sr_guard_eviction ev = sr_buffer_push_video(&c->buffer, pkt, pkt_ts);
			sr_capture_report_trim(c, &ev);
			if (ev.drop_all)
				c->reset_encoder = true;
		}

		if (verdict != SR_ENCODER_OK)
			sr_capture_replace_encoder(c, verdict);
	}

	log_buffer_stats(c, frame->timestamp);
	return frame;
}

static struct obs_audio_data *sr_capture_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct sr_capture *c = data;

	if (!c->buffer.samples_per_sec) {
		struct obs_audio_info oai;
		if (obs_get_audio_info(&oai)) {
			pthread_mutex_lock(&c->buffer.mutex);
			c->buffer.samples_per_sec = oai.samples_per_sec;
			c->buffer.speakers = oai.speakers;
			pthread_mutex_unlock(&c->buffer.mutex);
		}
	}

	if (c->buffer.samples_per_sec)
		sr_buffer_push_audio(&c->buffer, audio, get_audio_channels(c->buffer.speakers));

	return audio;
}

static obs_properties_t *sr_capture_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_int(props, S_DURATION, obs_module_text("Duration"), 1000, 120000, 500);
	obs_property_int_set_suffix(p, " ms");

	p = obs_properties_add_list(props, S_ENCODER, obs_module_text("Encoder"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Encoder.Auto"), SR_ENC_AUTO);
	obs_property_list_add_int(p, obs_module_text("Encoder.NVENC"), SR_ENC_NVENC);
	obs_property_list_add_int(p, obs_module_text("Encoder.AMF"), SR_ENC_AMF);
	obs_property_list_add_int(p, obs_module_text("Encoder.QSV"), SR_ENC_QSV);
	obs_property_list_add_int(p, obs_module_text("Encoder.X264"), SR_ENC_X264);

	p = obs_properties_add_list(props, S_QUALITY, obs_module_text("Quality"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Quality.High"), 18);
	obs_property_list_add_int(p, obs_module_text("Quality.Medium"), 23);
	obs_property_list_add_int(p, obs_module_text("Quality.Low"), 28);

	p = obs_properties_add_list(props, S_FPS, obs_module_text("CaptureFps"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("CaptureFps.Auto"), 0);
	obs_property_list_add_int(p, "60", 60);
	obs_property_list_add_int(p, "50", 50);
	obs_property_list_add_int(p, "30", 30);
	obs_property_list_add_int(p, "25", 25);
	obs_property_set_long_description(p, obs_module_text("CaptureFps.Tip"));

	p = obs_properties_add_list(props, S_KEYINT, obs_module_text("Keyint"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Keyint.All"), 1);
	obs_property_list_add_int(p, obs_module_text("Keyint.Balanced"), 15);
	obs_property_list_add_int(p, obs_module_text("Keyint.Light"), 30);
	obs_property_set_long_description(p, obs_module_text("Keyint.Tip"));

	char credit[256];
	obs_properties_add_text(props, "sr_credit", sr_plugin_credit_html(credit, sizeof(credit)), OBS_TEXT_INFO);

	return props;
}

static void sr_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_DURATION, 15000);
	obs_data_set_default_int(settings, S_ENCODER, SR_ENC_AUTO);
	obs_data_set_default_int(settings, S_QUALITY, 23);
	obs_data_set_default_int(settings, S_FPS, 0);
	obs_data_set_default_int(settings, S_KEYINT, SR_DEFAULT_KEYINT);
}

struct obs_source_info sr_capture_info = {
	.id = SR_CAPTURE_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = sr_capture_get_name,
	.create = sr_capture_create,
	.destroy = sr_capture_destroy,
	.update = sr_capture_update,
	.get_defaults = sr_capture_defaults,
	.get_properties = sr_capture_properties,
	.filter_video = sr_capture_filter_video,
	.filter_audio = sr_capture_filter_audio,
};
