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

#include "sr-scene-tracker.h"
#include "sr-config.h"
#include "sr-dock.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info sr_capture_info;
extern struct obs_source_info sr_playback_info;

bool obs_module_load(void)
{
	sr_config_init();
	obs_register_source(&sr_capture_info);
	obs_register_source(&sr_playback_info);
	obs_log(LOG_INFO, "Sports Replay loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	sr_scene_tracker_start();
	sr_dock_register();
}

void obs_module_unload(void)
{
	sr_scene_tracker_stop();
	sr_config_free();
	obs_log(LOG_INFO, "Sports Replay unloaded");
}

const char *obs_module_name(void)
{
	return "Sports Replay";
}

const char *obs_module_description(void)
{
	return obs_module_text("Description");
}
