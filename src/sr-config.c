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

#include "sr-config.h"

#include <stdlib.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/threading.h>

static pthread_mutex_t g_mutex;
static char *g_save_dir;

/* Default location when the user hasn't chosen one: <Videos>/Sports Replay,
 * created if needed. Falls back to the plugin config dir. */
static char *default_save_dir(void)
{
	struct dstr d = {0};
	const char *home = getenv("USERPROFILE");
	if (home && *home) {
		dstr_copy(&d, home);
		dstr_replace(&d, "\\", "/");
		dstr_cat(&d, "/Videos/Sports Replay");
	} else {
		char *cfg = obs_module_config_path("replays");
		dstr_copy(&d, cfg ? cfg : "replays");
		bfree(cfg);
	}
	os_mkdirs(d.array);
	char *result = bstrdup(d.array);
	dstr_free(&d);
	return result;
}

void sr_config_init(void)
{
	pthread_mutex_init(&g_mutex, NULL);

	char *path = obs_module_config_path("config.json");
	obs_data_t *data = path ? obs_data_create_from_json_file(path) : NULL;
	bfree(path);

	const char *saved = data ? obs_data_get_string(data, "save_dir") : "";
	g_save_dir = (saved && *saved) ? bstrdup(saved) : default_save_dir();
	if (data)
		obs_data_release(data);
}

void sr_config_free(void)
{
	bfree(g_save_dir);
	g_save_dir = NULL;
	pthread_mutex_destroy(&g_mutex);
}

char *sr_config_get_save_dir(void)
{
	pthread_mutex_lock(&g_mutex);
	char *r = bstrdup(g_save_dir ? g_save_dir : "");
	pthread_mutex_unlock(&g_mutex);
	return r;
}

void sr_config_set_save_dir(const char *save_dir)
{
	pthread_mutex_lock(&g_mutex);
	bfree(g_save_dir);
	g_save_dir = bstrdup(save_dir ? save_dir : "");
	pthread_mutex_unlock(&g_mutex);

	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "save_dir", save_dir ? save_dir : "");
	char *path = obs_module_config_path("config.json");
	if (path)
		obs_data_save_json(data, path);
	bfree(path);
	obs_data_release(data);
}
