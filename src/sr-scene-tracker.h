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

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tracks program scene changes so a replay can return to the scene that was
 * live before it. Registered once at module load. */
void sr_scene_tracker_start(void);
void sr_scene_tracker_stop(void);

/* Returns a bstrdup of the scene that was on program before the current one,
 * or NULL. Caller frees with bfree. */
char *sr_scene_tracker_previous(void);

/* Switches the program output to the named scene (on the UI thread). Takes a
 * copy of the name. */
void sr_switch_to_scene(const char *scene_name);

/* Same as sr_switch_to_scene(), but marks the switch as a "return to
 * previous scene" bounce, so a playback source landing there doesn't
 * auto-capture a fresh replay. */
void sr_switch_to_scene_return(const char *scene_name);

/* Reads and clears the "returning to previous scene" mark; true when the
 * activation happening right now was caused by sr_switch_to_scene_return()
 * or sr_switch_to_scene_of_source_return(). */
bool sr_scene_tracker_consume_returning(void);

/* Switches the program output to the scene holding the named source (a
 * camera), marked as a return bounce. Falls back to the previous scene when
 * no scene holds it. */
void sr_switch_to_scene_of_source_return(const char *source_name);

/* Name of the scene holding the named source, or NULL. Free with bfree().
 * Enumerates the scene list, so don't call it while holding a scene lock -
 * i.e. not from a source's activate() or video_tick(). */
char *sr_find_scene_with_source(const char *source_name);

#ifdef __cplusplus
}
#endif
