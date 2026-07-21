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
#include <util/threading.h>

static pthread_mutex_t g_mutex;
static bool g_started;
static char *g_current_scene;
static char *g_previous_scene;
static bool g_returning;

static void on_frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);
	if (event != OBS_FRONTEND_EVENT_SCENE_CHANGED)
		return;

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
	pthread_mutex_unlock(&g_mutex);

	obs_source_release(scene);
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
	g_current_scene = NULL;
	g_previous_scene = NULL;
	g_returning = false;
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

static void switch_scene_task(void *param)
{
	char *name = param;
	obs_source_t *scene = obs_get_source_by_name(name);
	if (scene) {
		obs_frontend_set_current_scene(scene);
		obs_source_release(scene);
	}
	bfree(name);
}

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
		/* obs_frontend_set_current_scene() activates the target scene's
		 * sources synchronously, so bracket the flag tightly around it
		 * instead of leaving it set for some other, later activation
		 * to stumble into. */
		pthread_mutex_lock(&g_mutex);
		g_returning = true;
		pthread_mutex_unlock(&g_mutex);

		obs_frontend_set_current_scene(scene);

		pthread_mutex_lock(&g_mutex);
		g_returning = false;
		pthread_mutex_unlock(&g_mutex);

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
	pthread_mutex_lock(&g_mutex);
	const bool was = g_returning;
	g_returning = false;
	pthread_mutex_unlock(&g_mutex);
	return was;
}
