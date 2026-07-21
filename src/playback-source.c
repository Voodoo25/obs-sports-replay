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
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <util/threading.h>
#include <media-io/video-io.h>

#include "sr-buffer.h"
#include "sr-codec.h"
#include "sr-capture.h"
#include "sr-scene-tracker.h"
#include "sr-clip.h"
#include "sr-save.h"
#include "sr-load.h"
#include "sr-config.h"
#include "sr-credit.h"

#include <time.h>
#include <sys/stat.h>
#include <util/dstr.h>
#include <util/platform.h>

#define S_SPEED "speed_percent"
#define S_BACKWARD "backward"
#define S_END_ACTION "end_action"
#define S_AUTOPLAY "autoplay"
#define S_INTRO_CLIP "intro_clip"
#define S_OUTRO_CLIP "outro_clip"
#define S_MUTED "run_muted"

enum sr_end_action {
	SR_END_FREEZE = 0, /* stay on the last frame */
	SR_END_RETURN = 1, /* cut back to the previous scene */
	SR_END_LOOP = 2,   /* replay again from the start */
};

/* The playout runs as a small sequence: intro clip, replay, outro clip. */
enum sr_phase {
	PHASE_IDLE = 0,
	PHASE_INTRO,
	PHASE_REPLAY,
	PHASE_OUTRO,
};

struct sr_playback {
	obs_source_t *self;
	pthread_mutex_t mutex;

	char *capture_source_name;

	struct sr_replay replay;
	bool have_replay;
	bool skip_next_autocapture; /* set by play_file so the scene switch it
				       triggers doesn't overwrite the loaded replay */

	struct sr_decoder *decoder;
	int64_t cur_idx; /* index of last decoded/output video packet */

	/* playhead is an absolute timestamp within [first_ts, last_ts] */
	uint64_t playhead;
	size_t audio_idx;

	int phase;
	struct sr_clip *intro_clip;
	struct sr_clip *outro_clip;
	char *intro_path;
	char *outro_path;
	int64_t clip_playhead; /* ns within the current intro/outro clip */

	double speed_percent;
	bool backward;
	int end_action;
	int end_action_override; /* -1 = use configured; else forces an action */
	bool autoplay;
	bool playing;
	bool paused;
	bool muted; /* forces replay audio off regardless of the source's own mixer mute */

	obs_hotkey_id hk_capture;
	obs_hotkey_id hk_play_pause;
	obs_hotkey_id hk_restart;
	obs_hotkey_id hk_faster;
	obs_hotkey_id hk_slower;
	obs_hotkey_id hk_normal;
	obs_hotkey_id hk_half;
	obs_hotkey_id hk_quarter;
	obs_hotkey_id hk_reverse;
	obs_hotkey_id hk_play_last;
	obs_hotkey_id hk_send_to_program;
};

/* ------------------------------------------------------------------ */
/* replay acquisition                                                  */

struct find_capture_ctx {
	obs_source_t *found;
};

static void find_capture_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	struct find_capture_ctx *ctx = param;
	if (ctx->found)
		return;
	if (strcmp(obs_source_get_unversioned_id(child), SR_CAPTURE_ID) == 0)
		ctx->found = child;
}

static void sr_playback_output_frame_at(struct sr_playback *p, size_t idx);
static void sr_playback_begin_sequence(struct sr_playback *p);

/* Takes ownership of *replay, replacing any current one, and starts playing.
 * end_action_override >= 0 forces that end action (e.g. return-to-scene for
 * replays launched from a file); -1 uses the source's configured action. */
static void sr_playback_install_replay(struct sr_playback *p, struct sr_replay *replay, int end_action_override)
{
	pthread_mutex_lock(&p->mutex);

	if (p->have_replay)
		sr_replay_free(&p->replay);
	p->replay = *replay;
	p->have_replay = true;
	p->end_action_override = end_action_override;

	sr_decoder_destroy(p->decoder);
	p->decoder = sr_decoder_create(p->replay.codec_id, p->replay.extradata, p->replay.extradata_size);

	sr_playback_begin_sequence(p);

	pthread_mutex_unlock(&p->mutex);
}

/* The end action in effect: a file-launch override, else the configured one. */
static int sr_playback_effective_end_action(const struct sr_playback *p)
{
	return (p->end_action_override >= 0) ? p->end_action_override : p->end_action;
}

static void sr_playback_capture_replay(struct sr_playback *p)
{
	if (!p->capture_source_name || !*p->capture_source_name)
		return;

	obs_source_t *target = obs_get_source_by_name(p->capture_source_name);
	if (!target) {
		obs_log(LOG_WARNING, "capture source '%s' not found", p->capture_source_name);
		return;
	}

	struct find_capture_ctx ctx = {0};
	obs_source_enum_filters(target, find_capture_filter, &ctx);

	struct sr_replay replay;
	bool got = false;
	if (ctx.found) {
		struct sr_buffer *buf = sr_capture_get_buffer(obs_obj_get_data(ctx.found));
		if (buf)
			got = sr_buffer_snapshot(buf, &replay);
	} else {
		obs_log(LOG_WARNING, "source '%s' has no Sports Replay Capture filter", p->capture_source_name);
	}
	obs_source_release(target);

	if (!got)
		return;

	/* auto-save to disk before publishing, while we still solely own the
	 * snapshot (mux only reads the packets, no re-encode) */
	char *save_dir = sr_config_get_save_dir();
	if (save_dir && *save_dir) {
		char stamp[32];
		const time_t now = time(NULL);
		struct tm tmv;
#ifdef _WIN32
		localtime_s(&tmv, &now);
#else
		localtime_r(&now, &tmv);
#endif
		strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);

		struct dstr path = {0};
		dstr_copy(&path, save_dir);
		dstr_replace(&path, "\\", "/");
		if (path.len && dstr_end(&path) != '/')
			dstr_cat_ch(&path, '/');
		dstr_cat(&path, p->capture_source_name);
		dstr_cat_ch(&path, '_');
		dstr_cat(&path, stamp);
		dstr_cat(&path, ".mp4");
		sr_save_replay(&replay, path.array);
		dstr_free(&path);
	}
	bfree(save_dir);

	obs_log(LOG_INFO, "captured replay: %zu frames, %.2f s", replay.video.num,
		(double)(replay.last_ts - replay.first_ts) / 1e9);

	sr_playback_install_replay(p, &replay, -1);
}

void sr_playback_play_file(obs_source_t *source, const char *path)
{
	if (!source || strcmp(obs_source_get_unversioned_id(source), SR_PLAYBACK_ID) != 0)
		return;
	struct sr_playback *p = obs_obj_get_data(source);
	if (!p || !path || !*path)
		return;

	struct sr_replay replay;
	if (!sr_load_replay(path, &replay))
		return;

	/* replays launched from a file always return to the previous scene */
	sr_playback_install_replay(p, &replay, SR_END_RETURN);

	/* the dock switches to the replay scene right after this, which fires
	 * activate; don't let autoplay overwrite the file we just loaded */
	pthread_mutex_lock(&p->mutex);
	p->skip_next_autocapture = true;
	pthread_mutex_unlock(&p->mutex);
}

/* ------------------------------------------------------------------ */
/* playback                                                            */

/* Pushes a decoded AVFrame (I420 or NV12) out as an async source frame. */
static void output_avframe(struct sr_playback *p, AVFrame *decoded)
{
	struct obs_source_frame out = {0};
	out.width = (uint32_t)decoded->width;
	out.height = (uint32_t)decoded->height;
	out.timestamp = os_gettime_ns();

	switch (decoded->format) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		out.format = VIDEO_FORMAT_I420;
		break;
	case AV_PIX_FMT_NV12:
		out.format = VIDEO_FORMAT_NV12;
		break;
	default:
		return;
	}

	for (size_t i = 0; i < 4; i++) {
		out.data[i] = decoded->data[i];
		out.linesize[i] = (uint32_t)decoded->linesize[i];
	}

	video_format_get_parameters(VIDEO_CS_709, VIDEO_RANGE_PARTIAL, out.color_matrix, out.color_range_min,
				    out.color_range_max);

	obs_source_output_video(p->self, &out);
}

static void sr_playback_output_frame_at(struct sr_playback *p, size_t idx)
{
	if (!p->decoder || idx >= p->replay.video.num)
		return;

	/* all frames are intra frames: a non-sequential jump only needs a
	 * decoder flush, not decoding from a keyframe */
	if (p->cur_idx >= 0 && (size_t)(p->cur_idx + 1) != idx)
		sr_decoder_flush(p->decoder);

	AVFrame *decoded = NULL;
	if (!sr_decoder_decode(p->decoder, p->replay.video.array[idx].pkt, &decoded))
		return;

	output_avframe(p, decoded);
	p->cur_idx = (int64_t)idx;
}

/* Enters the intro phase if an intro clip is loaded, otherwise the replay
 * phase. Assumes the mutex is held and a replay snapshot exists. */
static void sr_playback_begin_sequence(struct sr_playback *p)
{
	p->paused = false;
	p->playing = true;

	if (p->intro_clip) {
		sr_clip_set_output_size(p->intro_clip, p->replay.width, p->replay.height);
		sr_clip_rewind(p->intro_clip);
		p->clip_playhead = 0;
		p->phase = PHASE_INTRO;
	} else {
		p->cur_idx = -1;
		p->audio_idx = 0;
		p->playhead = p->backward ? p->replay.last_ts : p->replay.first_ts;
		p->phase = PHASE_REPLAY;
	}
}

static void sr_playback_begin_replay_phase(struct sr_playback *p)
{
	p->cur_idx = -1;
	p->audio_idx = 0;
	p->playhead = p->backward ? p->replay.last_ts : p->replay.first_ts;
	p->phase = PHASE_REPLAY;
}

/* find the frame whose timestamp is closest below the playhead */
static size_t frame_index_for_playhead(struct sr_playback *p)
{
	const struct sr_packet *arr = p->replay.video.array;
	size_t lo = 0;
	size_t hi = p->replay.video.num;
	while (lo + 1 < hi) {
		const size_t mid = (lo + hi) / 2;
		if (arr[mid].ts <= p->playhead)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

static void sr_playback_output_audio(struct sr_playback *p, uint64_t from, uint64_t to)
{
	if (p->backward || p->speed_percent != 100.0)
		return;

	while (p->audio_idx < p->replay.audio.num) {
		const struct sr_audio_chunk *chunk = &p->replay.audio.array[p->audio_idx];
		if (chunk->ts > to)
			break;
		if (chunk->ts > from && !p->muted) {
			struct obs_source_audio out = {0};
			for (size_t i = 0; i < MAX_AV_PLANES; i++)
				out.data[i] = (const uint8_t *)chunk->data[i];
			out.frames = chunk->frames;
			out.speakers = p->replay.speakers;
			out.samples_per_sec = p->replay.samples_per_sec;
			out.format = AUDIO_FORMAT_FLOAT_PLANAR;
			out.timestamp = os_gettime_ns();
			obs_source_output_audio(p->self, &out);
		}
		p->audio_idx++;
	}
}

/* Plays an intro/outro clip. Returns true when the clip has finished. */
static bool tick_clip_phase(struct sr_playback *p, struct sr_clip *clip, float seconds)
{
	if (!clip)
		return true;

	p->clip_playhead += (int64_t)((double)seconds * 1e9);

	AVFrame *frame = NULL;
	bool ended = false;
	if (sr_clip_advance(clip, p->clip_playhead, &frame, &ended) && frame)
		output_avframe(p, frame);

	return ended;
}

static void sr_playback_tick(void *data, float seconds)
{
	struct sr_playback *p = data;

	pthread_mutex_lock(&p->mutex);

	if (!p->have_replay || !p->playing || p->paused) {
		pthread_mutex_unlock(&p->mutex);
		return;
	}

	if (p->phase == PHASE_INTRO) {
		if (tick_clip_phase(p, p->intro_clip, seconds))
			sr_playback_begin_replay_phase(p);
		pthread_mutex_unlock(&p->mutex);
		return;
	}

	if (p->phase == PHASE_OUTRO) {
		bool finished = tick_clip_phase(p, p->outro_clip, seconds);
		bool return_to_scene = false;
		if (finished) {
			p->playing = false;
			obs_source_media_ended(p->self);
			return_to_scene = (sr_playback_effective_end_action(p) == SR_END_RETURN);
		}
		pthread_mutex_unlock(&p->mutex);
		if (return_to_scene) {
			char *prev = sr_scene_tracker_previous();
			if (prev) {
				sr_switch_to_scene_return(prev);
				bfree(prev);
			}
		}
		return;
	}

	/* PHASE_REPLAY */
	const uint64_t prev_playhead = p->playhead;
	const double delta_d = (double)seconds * 1e9 * (p->speed_percent / 100.0);
	const uint64_t delta = (uint64_t)delta_d;

	bool ended = false;
	if (p->backward) {
		if (p->playhead - p->replay.first_ts <= delta) {
			p->playhead = p->replay.first_ts;
			ended = true;
		} else {
			p->playhead -= delta;
		}
	} else {
		if (p->replay.last_ts - p->playhead <= delta) {
			p->playhead = p->replay.last_ts;
			ended = true;
		} else {
			p->playhead += delta;
		}
	}

	const size_t idx = frame_index_for_playhead(p);
	if ((int64_t)idx != p->cur_idx)
		sr_playback_output_frame_at(p, idx);

	sr_playback_output_audio(p, prev_playhead, p->playhead);

	bool return_to_scene = false;
	if (ended) {
		const int ea = sr_playback_effective_end_action(p);
		if (ea == SR_END_LOOP) {
			/* loop the replay footage only, skipping intro/outro */
			p->playhead = p->backward ? p->replay.last_ts : p->replay.first_ts;
			p->audio_idx = 0;
			p->cur_idx = -1;
		} else if (p->outro_clip) {
			sr_clip_set_output_size(p->outro_clip, p->replay.width, p->replay.height);
			sr_clip_rewind(p->outro_clip);
			p->clip_playhead = 0;
			p->phase = PHASE_OUTRO;
		} else {
			p->playing = false;
			obs_source_media_ended(p->self);
			return_to_scene = (ea == SR_END_RETURN);
		}
	}

	pthread_mutex_unlock(&p->mutex);

	/* switch scenes outside the lock to avoid re-entrancy on our own state */
	if (return_to_scene) {
		char *prev = sr_scene_tracker_previous();
		if (prev) {
			sr_switch_to_scene_return(prev);
			bfree(prev);
		}
	}
}

/* ------------------------------------------------------------------ */
/* hotkeys                                                             */

static void hk_capture_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		sr_playback_capture_replay(data);
}

static void hk_play_pause_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct sr_playback *p = data;
	if (!pressed)
		return;
	pthread_mutex_lock(&p->mutex);
	if (p->have_replay) {
		if (!p->playing) {
			p->paused = false;
			sr_playback_begin_replay_phase(p);
			p->playing = true;
		} else {
			p->paused = !p->paused;
		}
	}
	pthread_mutex_unlock(&p->mutex);
}

static void hk_restart_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct sr_playback *p = data;
	if (!pressed)
		return;
	pthread_mutex_lock(&p->mutex);
	if (p->have_replay) {
		p->paused = false;
		sr_playback_begin_replay_phase(p);
		p->playing = true;
	}
	pthread_mutex_unlock(&p->mutex);
}

static void set_speed(struct sr_playback *p, double speed)
{
	pthread_mutex_lock(&p->mutex);
	if (speed < 10.0)
		speed = 10.0;
	if (speed > 400.0)
		speed = 400.0;
	p->speed_percent = speed;
	pthread_mutex_unlock(&p->mutex);

	obs_data_t *settings = obs_source_get_settings(p->self);
	obs_data_set_double(settings, S_SPEED, speed);
	obs_data_release(settings);
}

static void hk_faster_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct sr_playback *p = data;
	if (pressed)
		set_speed(p, p->speed_percent * 1.5);
}

static void hk_slower_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct sr_playback *p = data;
	if (pressed)
		set_speed(p, p->speed_percent / 1.5);
}

static void hk_normal_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		set_speed(data, 100.0);
}

static void hk_half_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		set_speed(data, 50.0);
}

static void hk_quarter_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		set_speed(data, 25.0);
}

static void hk_reverse_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct sr_playback *p = data;
	if (!pressed)
		return;
	pthread_mutex_lock(&p->mutex);
	p->backward = !p->backward;
	pthread_mutex_unlock(&p->mutex);

	obs_data_t *settings = obs_source_get_settings(p->self);
	obs_data_set_bool(settings, S_BACKWARD, p->backward);
	obs_data_release(settings);
}

/* Returns a bstrdup of the most recently modified .mp4 in dir, or NULL. */
static char *find_newest_replay(const char *dir)
{
	if (!dir || !*dir)
		return NULL;

	struct dstr pattern = {0};
	dstr_copy(&pattern, dir);
	dstr_replace(&pattern, "\\", "/");
	if (pattern.len && dstr_end(&pattern) != '/')
		dstr_cat_ch(&pattern, '/');
	dstr_cat(&pattern, "*.mp4");

	char *newest = NULL;
	time_t newest_mtime = 0;
	os_glob_t *glob = NULL;
	if (os_glob(pattern.array, 0, &glob) == 0) {
		for (size_t i = 0; i < glob->gl_pathc; i++) {
			if (glob->gl_pathv[i].directory)
				continue;
			struct stat st;
			if (os_stat(glob->gl_pathv[i].path, &st) == 0 && (!newest || st.st_mtime > newest_mtime)) {
				newest_mtime = st.st_mtime;
				bfree(newest);
				newest = bstrdup(glob->gl_pathv[i].path);
			}
		}
		os_globfree(glob);
	}
	dstr_free(&pattern);
	return newest;
}

static void hk_play_last_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct sr_playback *p = data;
	if (!pressed)
		return;

	char *dir = sr_config_get_save_dir();
	char *newest = find_newest_replay(dir);
	bfree(dir);
	if (newest) {
		sr_playback_play_file(p->self, newest);
		bfree(newest);
	}
}

struct find_scene_ctx {
	obs_source_t *target;
	char *found_name;
};

static bool enum_scene_item_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	struct find_scene_ctx *ctx = param;
	if (obs_sceneitem_get_source(item) != ctx->target)
		return true;
	ctx->found_name = bstrdup(obs_source_get_name(obs_scene_get_source(scene)));
	return false;
}

/* Finds the scene this playback source lives in, so the "send to program"
 * hotkey can jump straight there without needing a separate scene picker. */
static char *sr_playback_find_containing_scene(struct sr_playback *p)
{
	struct obs_frontend_source_list scenes = {0};
	obs_frontend_get_scenes(&scenes);

	char *found = NULL;
	for (size_t i = 0; i < scenes.sources.num && !found; i++) {
		obs_scene_t *scene = obs_scene_from_source(scenes.sources.array[i]);
		struct find_scene_ctx ctx = {p->self, NULL};
		obs_scene_enum_items(scene, enum_scene_item_cb, &ctx);
		found = ctx.found_name;
	}

	obs_frontend_source_list_free(&scenes);
	return found;
}

/* Cuts the program output directly to this source's scene, bypassing preview
 * / Studio Mode, the same way OBS's own "switch to scene" hotkey does when
 * Studio Mode is off. */
static void hk_send_to_program_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;

	struct sr_playback *p = data;
	char *scene_name = sr_playback_find_containing_scene(p);
	if (scene_name) {
		sr_switch_to_scene(scene_name);
		bfree(scene_name);
	}
}

/* ------------------------------------------------------------------ */
/* source callbacks                                                    */

static const char *sr_playback_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("SportsReplay");
}

/* Reloads a clip when its configured path changed. Opening the file happens
 * outside the lock; only the pointer swap is guarded. */
static void reload_clip(struct sr_playback *p, struct sr_clip **clip, char **stored_path, const char *new_path)
{
	const bool same = (*stored_path && new_path && strcmp(*stored_path, new_path) == 0) ||
			  (!*stored_path && (!new_path || !*new_path));
	if (same)
		return;

	struct sr_clip *fresh = (new_path && *new_path) ? sr_clip_open(new_path) : NULL;

	pthread_mutex_lock(&p->mutex);
	struct sr_clip *old = *clip;
	*clip = fresh;
	bfree(*stored_path);
	*stored_path = bstrdup(new_path ? new_path : "");
	pthread_mutex_unlock(&p->mutex);

	sr_clip_close(old);
}

static void sr_playback_update(void *data, obs_data_t *settings)
{
	struct sr_playback *p = data;

	pthread_mutex_lock(&p->mutex);
	bfree(p->capture_source_name);
	p->capture_source_name = bstrdup(obs_data_get_string(settings, S_CAPTURE_SOURCE));
	p->speed_percent = obs_data_get_double(settings, S_SPEED);
	p->backward = obs_data_get_bool(settings, S_BACKWARD);
	p->end_action = (int)obs_data_get_int(settings, S_END_ACTION);
	p->autoplay = obs_data_get_bool(settings, S_AUTOPLAY);
	p->muted = obs_data_get_bool(settings, S_MUTED);
	pthread_mutex_unlock(&p->mutex);

	reload_clip(p, &p->intro_clip, &p->intro_path, obs_data_get_string(settings, S_INTRO_CLIP));
	reload_clip(p, &p->outro_clip, &p->outro_path, obs_data_get_string(settings, S_OUTRO_CLIP));
}

/* Called when the source goes live on the program output (i.e. the user cut
 * to the replay scene). With autoplay on, this grabs the last N seconds from
 * the capture source and plays them at the configured speed. */
static void sr_playback_activate(void *data)
{
	struct sr_playback *p = data;

	pthread_mutex_lock(&p->mutex);
	const bool skip = p->skip_next_autocapture;
	p->skip_next_autocapture = false;
	pthread_mutex_unlock(&p->mutex);
	if (skip || sr_scene_tracker_consume_returning())
		return;

	if (p->autoplay)
		sr_playback_capture_replay(p);
}

static void *sr_playback_create(obs_data_t *settings, obs_source_t *source)
{
	struct sr_playback *p = bzalloc(sizeof(struct sr_playback));
	p->self = source;
	p->speed_percent = 100.0;
	p->cur_idx = -1;
	p->end_action_override = -1;
	pthread_mutex_init(&p->mutex, NULL);

	p->hk_capture = obs_hotkey_register_source(source, "SportsReplay.Capture",
						   obs_module_text("Hotkey.Capture"), hk_capture_cb, p);
	p->hk_play_pause = obs_hotkey_register_source(source, "SportsReplay.PlayPause",
						      obs_module_text("Hotkey.PlayPause"), hk_play_pause_cb, p);
	p->hk_restart = obs_hotkey_register_source(source, "SportsReplay.Restart",
						   obs_module_text("Hotkey.Restart"), hk_restart_cb, p);
	p->hk_faster = obs_hotkey_register_source(source, "SportsReplay.Faster", obs_module_text("Hotkey.Faster"),
						  hk_faster_cb, p);
	p->hk_slower = obs_hotkey_register_source(source, "SportsReplay.Slower", obs_module_text("Hotkey.Slower"),
						  hk_slower_cb, p);
	p->hk_normal = obs_hotkey_register_source(source, "SportsReplay.NormalSpeed",
						  obs_module_text("Hotkey.NormalSpeed"), hk_normal_cb, p);
	p->hk_half = obs_hotkey_register_source(source, "SportsReplay.HalfSpeed",
						obs_module_text("Hotkey.HalfSpeed"), hk_half_cb, p);
	p->hk_quarter = obs_hotkey_register_source(source, "SportsReplay.QuarterSpeed",
						   obs_module_text("Hotkey.QuarterSpeed"), hk_quarter_cb, p);
	p->hk_reverse = obs_hotkey_register_source(source, "SportsReplay.ReverseToggle",
						   obs_module_text("Hotkey.ReverseToggle"), hk_reverse_cb, p);
	p->hk_play_last = obs_hotkey_register_source(source, "SportsReplay.PlayLast",
						     obs_module_text("Hotkey.PlayLast"), hk_play_last_cb, p);
	p->hk_send_to_program = obs_hotkey_register_source(source, "SportsReplay.SendToProgram",
							   obs_module_text("Hotkey.SendToProgram"),
							   hk_send_to_program_cb, p);

	sr_playback_update(p, settings);
	return p;
}

static void sr_playback_destroy(void *data)
{
	struct sr_playback *p = data;

	obs_hotkey_unregister(p->hk_capture);
	obs_hotkey_unregister(p->hk_play_pause);
	obs_hotkey_unregister(p->hk_restart);
	obs_hotkey_unregister(p->hk_faster);
	obs_hotkey_unregister(p->hk_slower);
	obs_hotkey_unregister(p->hk_normal);
	obs_hotkey_unregister(p->hk_half);
	obs_hotkey_unregister(p->hk_quarter);
	obs_hotkey_unregister(p->hk_reverse);
	obs_hotkey_unregister(p->hk_play_last);
	obs_hotkey_unregister(p->hk_send_to_program);

	if (p->have_replay)
		sr_replay_free(&p->replay);
	sr_decoder_destroy(p->decoder);
	sr_clip_close(p->intro_clip);
	sr_clip_close(p->outro_clip);
	bfree(p->intro_path);
	bfree(p->outro_path);
	bfree(p->capture_source_name);
	pthread_mutex_destroy(&p->mutex);
	bfree(p);
}

struct enum_capture_ctx {
	obs_property_t *list;
};

static void enum_filters_for_list(obs_source_t *parent, obs_source_t *child, void *param)
{
	struct enum_capture_ctx *ctx = param;
	if (strcmp(obs_source_get_unversioned_id(child), SR_CAPTURE_ID) == 0) {
		const char *name = obs_source_get_name(parent);
		obs_property_list_add_string(ctx->list, name, name);
	}
}

static bool enum_sources_for_list(void *param, obs_source_t *source)
{
	struct enum_capture_ctx *ctx = param;
	obs_source_enum_filters(source, enum_filters_for_list, ctx);
	return true;
}

static bool capture_button_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	sr_playback_capture_replay(data);
	return false;
}

static obs_properties_t *sr_playback_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, S_CAPTURE_SOURCE, obs_module_text("CaptureSource"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, obs_module_text("NoSource"), "");

	struct enum_capture_ctx ctx = {.list = list};
	obs_enum_sources(enum_sources_for_list, &ctx);

	obs_property_t *p = obs_properties_add_float_slider(props, S_SPEED, obs_module_text("Speed"), 10.0, 400.0,
							    5.0);
	obs_property_float_set_suffix(p, " %");

	obs_properties_add_bool(props, S_BACKWARD, obs_module_text("Backward"));

	obs_property_t *ea = obs_properties_add_list(props, S_END_ACTION, obs_module_text("EndAction"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(ea, obs_module_text("EndAction.Freeze"), SR_END_FREEZE);
	obs_property_list_add_int(ea, obs_module_text("EndAction.Return"), SR_END_RETURN);
	obs_property_list_add_int(ea, obs_module_text("EndAction.Loop"), SR_END_LOOP);

	obs_properties_add_bool(props, S_AUTOPLAY, obs_module_text("AutoPlay"));

	const char *media_filter = obs_module_text("MediaFilter");
	obs_properties_add_path(props, S_INTRO_CLIP, obs_module_text("IntroClip"), OBS_PATH_FILE, media_filter, NULL);
	obs_properties_add_path(props, S_OUTRO_CLIP, obs_module_text("OutroClip"), OBS_PATH_FILE, media_filter, NULL);
	obs_properties_add_bool(props, S_MUTED, obs_module_text("RunMuted"));


	if (data)
		obs_properties_add_button(props, "capture_now", obs_module_text("CaptureNow"), capture_button_clicked);

	char credit[256];
	obs_properties_add_text(props, "sr_credit", sr_plugin_credit_html(credit, sizeof(credit)), OBS_TEXT_INFO);

	return props;
}

static void sr_playback_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, S_SPEED, 100.0);
	obs_data_set_default_bool(settings, S_BACKWARD, false);
	obs_data_set_default_int(settings, S_END_ACTION, SR_END_FREEZE);
	obs_data_set_default_bool(settings, S_AUTOPLAY, true);
	obs_data_set_default_bool(settings, S_MUTED, false);
}

static uint32_t sr_playback_get_width(void *data)
{
	struct sr_playback *p = data;
	return p->have_replay ? p->replay.width : 0;
}

static uint32_t sr_playback_get_height(void *data)
{
	struct sr_playback *p = data;
	return p->have_replay ? p->replay.height : 0;
}

struct obs_source_info sr_playback_info = {
	.id = SR_PLAYBACK_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = sr_playback_get_name,
	.create = sr_playback_create,
	.destroy = sr_playback_destroy,
	.update = sr_playback_update,
	.get_defaults = sr_playback_defaults,
	.get_properties = sr_playback_properties,
	.activate = sr_playback_activate,
	.video_tick = sr_playback_tick,
	.get_width = sr_playback_get_width,
	.get_height = sr_playback_get_height,
};
