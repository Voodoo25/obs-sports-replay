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

#include "sr-scene-tracker.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/threading.h>

/* How long a pending "return to previous scene" bounce stays valid. The
 * activation of the scene we bounce to does not necessarily happen inside
 * obs_frontend_set_current_scene() - with a transition in play it lands a
 * few frames later, on another thread - so the mark has to outlive the call
 * that set it. Kept short (a handful of frames) so an operator firing a
 * replay right after a bounce still gets a fresh capture; it is also cleared
 * as soon as it is consumed, or as soon as the program moves to any scene
 * other than the one we bounced to. */
#define SR_RETURN_WINDOW_NS 500000000ULL

/* How long the preview guard keeps watching after the replay leaves program.
 * OBS performs the preview/program swap when the transition finishes, which
 * is after the source is deactivated, so the guard has to outlive it. */
#define SR_PREVIEW_GUARD_TAIL_NS 5000000000ULL

static pthread_mutex_t g_mutex;
static bool g_started;
static char *g_current_scene;
static char *g_previous_scene;
static char *g_return_target; /* scene a bounce is on its way to, or NULL */
static uint64_t g_return_expires;
static char *g_preview_scene;         /* what the operator has lined up in preview */
static bool g_preview_guard;          /* a replay is on air: keep it out of preview */
static uint64_t g_preview_guard_ends; /* 0 = guard runs until the replay leaves program */

/* call with g_mutex held */
static void clear_return_mark(void)
{
	bfree(g_return_target);
	g_return_target = NULL;
	g_return_expires = 0;
}

static void on_program_scene_changed(void)
{
	obs_source_t *scene = obs_frontend_get_current_scene();
	if (!scene)
		return;
	const char *name = obs_source_get_name(scene);

	pthread_mutex_lock(&g_mutex);
	if (!g_current_scene || strcmp(g_current_scene, name) != 0) {
		bfree(g_previous_scene);
		g_previous_scene = g_current_scene; /* hand over ownership */
		g_current_scene = bstrdup(name);
	}
	/* the program went somewhere else: whatever bounce was in flight is
	 * no longer the reason this scene is live */
	if (g_return_target && strcmp(g_return_target, name) != 0)
		clear_return_mark();
	pthread_mutex_unlock(&g_mutex);

	obs_source_release(scene);
}

/* Records the scene sitting in preview. Studio mode only (the frontend hands
 * back NULL otherwise), and UI thread only. */
static void note_preview_scene(void)
{
	obs_source_t *scene = obs_frontend_get_current_preview_scene();

	pthread_mutex_lock(&g_mutex);
	bfree(g_preview_scene);
	g_preview_scene = scene ? bstrdup(obs_source_get_name(scene)) : NULL;
	pthread_mutex_unlock(&g_mutex);

	obs_source_release(scene);
}

/* call with g_mutex held */
static bool preview_guard_running(void)
{
	if (!g_preview_guard)
		return false;
	if (g_preview_guard_ends && os_gettime_ns() > g_preview_guard_ends) {
		g_preview_guard = false;
		g_preview_guard_ends = 0;
		return false;
	}
	return true;
}

static void on_preview_scene_changed(void)
{
	if (!obs_frontend_preview_program_mode_active())
		return;

	obs_source_t *scene = obs_frontend_get_current_preview_scene();
	if (!scene)
		return;
	const char *name = obs_source_get_name(scene);

	char *restore = NULL;

	pthread_mutex_lock(&g_mutex);
	/* OBS's "swap preview/program scenes after transitioning" drops the
	 * scene leaving program into preview, and it does that inside
	 * TransitionStopped() - before the program change reaches us - so
	 * g_current_scene still names the scene that just left the air. While
	 * a replay is on air, that scene is the replay itself (or the camera
	 * it cut into), and the operator never asked for either of them to
	 * take over the shot they had lined up. Undo it, putting back what was
	 * in preview - which is whatever the operator last picked, since a
	 * swap we undo never becomes g_preview_scene. */
	const bool swapped_in = preview_guard_running() && g_current_scene && strcmp(name, g_current_scene) == 0 &&
				g_preview_scene && strcmp(g_preview_scene, name) != 0;

	if (swapped_in)
		restore = bstrdup(g_preview_scene);
	else {
		bfree(g_preview_scene);
		g_preview_scene = bstrdup(name);
	}
	pthread_mutex_unlock(&g_mutex);

	obs_source_release(scene);

	if (!restore)
		return;

	obs_source_t *target = obs_get_source_by_name(restore);
	if (target) {
		obs_frontend_set_current_preview_scene(target);
		obs_source_release(target);
	}
	bfree(restore);
}

static void on_frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);

	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		on_program_scene_changed();
		break;
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		on_preview_scene_changed();
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		note_preview_scene();
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		pthread_mutex_lock(&g_mutex);
		bfree(g_preview_scene);
		g_preview_scene = NULL;
		pthread_mutex_unlock(&g_mutex);
		break;
	default:
		break;
	}
}

void sr_scene_tracker_start(void)
{
	pthread_mutex_init(&g_mutex, NULL);
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	g_started = true;
}

void sr_scene_tracker_stop(void)
{
	if (!g_started)
		return;
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	pthread_mutex_lock(&g_mutex);
	bfree(g_current_scene);
	bfree(g_previous_scene);
	bfree(g_preview_scene);
	g_current_scene = NULL;
	g_previous_scene = NULL;
	g_preview_scene = NULL;
	g_preview_guard = false;
	g_preview_guard_ends = 0;
	clear_return_mark();
	pthread_mutex_unlock(&g_mutex);
	pthread_mutex_destroy(&g_mutex);
	g_started = false;
}

char *sr_scene_tracker_previous(void)
{
	char *result = NULL;
	pthread_mutex_lock(&g_mutex);
	if (g_previous_scene)
		result = bstrdup(g_previous_scene);
	pthread_mutex_unlock(&g_mutex);
	return result;
}

struct find_scene_ctx {
	const char *source_name;
	char *found_name;
};

static bool enum_scene_for_source(void *param, obs_source_t *scene_source)
{
	struct find_scene_ctx *ctx = param;
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (scene && obs_scene_find_source_recursive(scene, ctx->source_name)) {
		ctx->found_name = bstrdup(obs_source_get_name(scene_source));
		return false;
	}
	return true;
}

char *sr_find_scene_with_source(const char *source_name)
{
	if (!source_name || !*source_name)
		return NULL;

	struct find_scene_ctx ctx = {source_name, NULL};
	obs_enum_scenes(enum_scene_for_source, &ctx);
	return ctx.found_name;
}

static void switch_scene_task(void *param)
{
	char *name = param;
	obs_source_t *scene = obs_get_source_by_name(name);
	if (scene) {
		/* This is a replay scene going on air, and the transition that
		 * follows may swap preview out from under the operator before
		 * the source's activate() ever runs - that lands a frame later,
		 * from the video tick. Guard it here, where we are ahead of the
		 * switch and on the UI thread. */
		note_preview_scene();
		sr_scene_tracker_note_replay_launch();

		obs_frontend_set_current_scene(scene);
		obs_source_release(scene);
	}
	bfree(name);
}

/* Puts a replay scene on program, cutting past preview the way OBS's own
 * "switch to scene" hotkey does. */
void sr_switch_to_scene(const char *scene_name)
{
	if (!scene_name || !*scene_name)
		return;
	/* scene switching must happen on the UI thread */
	obs_queue_task(OBS_TASK_UI, switch_scene_task, bstrdup(scene_name), false);
}

static void switch_scene_return_task(void *param)
{
	char *name = param;
	obs_source_t *scene = obs_get_source_by_name(name);
	if (scene) {
		/* Mark the destination before the switch: the sources there are
		 * activated once the transition to it starts, which is after
		 * this call returns. */
		pthread_mutex_lock(&g_mutex);
		bfree(g_return_target);
		g_return_target = bstrdup(name);
		g_return_expires = os_gettime_ns() + SR_RETURN_WINDOW_NS;
		pthread_mutex_unlock(&g_mutex);

		obs_frontend_set_current_scene(scene);
		obs_source_release(scene);
	}
	bfree(name);
}

/* Same as sr_switch_to_scene(), but marks the activation as a "return to
 * previous scene" bounce: if the scene we land on itself holds a Sports
 * Replay source with autoplay + "return to previous" configured, that
 * source must not treat this as a deliberate trigger and auto-capture a
 * fresh replay - otherwise two such scenes ping-pong forever. */
void sr_switch_to_scene_return(const char *scene_name)
{
	if (!scene_name || !*scene_name)
		return;
	obs_queue_task(OBS_TASK_UI, switch_scene_return_task, bstrdup(scene_name), false);
}

bool sr_scene_tracker_consume_returning(void)
{
	bool match = false;

	pthread_mutex_lock(&g_mutex);
	if (g_return_target) {
		match = os_gettime_ns() <= g_return_expires;
		clear_return_mark();
	}
	pthread_mutex_unlock(&g_mutex);

	return match;
}

/* Runs on the UI thread: enumerating scenes here is safe, whereas doing it
 * from a source's activate()/video_tick() would take the scene list lock
 * while a scene lock is already held - the reverse of the order the dock
 * takes them in. */
static void switch_to_source_scene_task(void *param)
{
	char *source_name = param;

	char *scene = sr_find_scene_with_source(source_name);
	if (!scene) {
		blog(LOG_WARNING, "[sports-replay] '%s' is not in any scene, returning to the previous one",
		     source_name);
		scene = sr_scene_tracker_previous();
	}

	if (scene) {
		switch_scene_return_task(bstrdup(scene)); /* frees its argument */
		bfree(scene);
	}
	bfree(source_name);
}

void sr_switch_to_scene_of_source_return(const char *source_name)
{
	if (!source_name || !*source_name)
		return;
	obs_queue_task(OBS_TASK_UI, switch_to_source_scene_task, bstrdup(source_name), false);
}

void sr_scene_tracker_note_replay_launch(void)
{
	pthread_mutex_lock(&g_mutex);
	g_preview_guard = true;
	g_preview_guard_ends = 0; /* guard until the replay leaves program */
	pthread_mutex_unlock(&g_mutex);
}

void sr_scene_tracker_end_replay_guard(void)
{
	pthread_mutex_lock(&g_mutex);
	if (g_preview_guard && !g_preview_guard_ends)
		g_preview_guard_ends = os_gettime_ns() + SR_PREVIEW_GUARD_TAIL_NS;
	pthread_mutex_unlock(&g_mutex);
}
