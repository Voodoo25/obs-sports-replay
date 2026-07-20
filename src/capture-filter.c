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

#include "sr-buffer.h"
#include "sr-codec.h"
#include "sr-capture.h"
#include "sr-credit.h"

#define S_DURATION "duration_ms"
#define S_ENCODER "encoder"
#define S_QUALITY "quality"

struct sr_capture {
	obs_source_t *self;
	struct sr_buffer buffer;
	struct sr_encoder *encoder;

	enum sr_encoder_backend backend;
	int qp;

	/* format the current encoder was opened with */
	uint32_t enc_width;
	uint32_t enc_height;
	bool encoder_failed;
	bool reset_encoder;

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
	if (backend != c->backend || qp != c->qp) {
		c->backend = backend;
		c->qp = qp;
		c->reset_encoder = true;
		c->encoder_failed = false;
	}
}

static void *sr_capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct sr_capture *c = bzalloc(sizeof(struct sr_capture));
	c->self = source;
	sr_buffer_init(&c->buffer);
	c->backend = SR_ENC_AUTO;
	c->qp = 23;
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

static void log_buffer_stats(struct sr_capture *c, uint64_t now)
{
	if (c->last_stats_log && now - c->last_stats_log < 60000000000ULL)
		return;
	c->last_stats_log = now;
	const size_t bytes = sr_buffer_video_bytes(&c->buffer);
	obs_log(LOG_INFO, "'%s': replay buffer using %.1f MB", obs_source_get_name(c->self),
		(double)bytes / (1024.0 * 1024.0));
}

static struct obs_source_frame *sr_capture_filter_video(void *data, struct obs_source_frame *frame)
{
	struct sr_capture *c = data;

	if (!frame || !frame->data[0] || c->encoder_failed)
		return frame;

	if (c->encoder && (c->reset_encoder || frame->width != c->enc_width || frame->height != c->enc_height)) {
		sr_encoder_destroy(c->encoder);
		c->encoder = NULL;
		sr_buffer_clear(&c->buffer);
	}

	if (!c->encoder) {
		struct obs_video_info ovi;
		obs_get_video_info(&ovi);
		c->encoder = sr_encoder_create(frame->width, frame->height, ovi.fps_num, ovi.fps_den, c->backend,
					       c->qp);
		if (!c->encoder) {
			obs_log(LOG_ERROR, "'%s': no H.264 encoder available, replay capture disabled",
				obs_source_get_name(c->self));
			c->encoder_failed = true;
			return frame;
		}
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

	AVPacket *pkt = sr_encoder_encode(c->encoder, frame);
	if (pkt)
		sr_buffer_push_video(&c->buffer, pkt, frame->timestamp);

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

	char credit[256];
	obs_properties_add_text(props, "sr_credit", sr_plugin_credit_html(credit, sizeof(credit)), OBS_TEXT_INFO);

	return props;
}

static void sr_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_DURATION, 15000);
	obs_data_set_default_int(settings, S_ENCODER, SR_ENC_AUTO);
	obs_data_set_default_int(settings, S_QUALITY, 23);
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
