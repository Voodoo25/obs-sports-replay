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

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the replay clip-bin dock and registers it with the OBS frontend.
 * Call once, after the frontend is ready (obs_module_post_load). */
void sr_dock_register(void);

/* Marks a saved replay as already gone to air, the same way launching one
 * from the dock does. For replays that go straight to program from a hotkey:
 * they were watched, so the panel has to show it. No-op if the dock isn't
 * up. Safe to call from any thread. */
void sr_dock_mark_played(const char *path);

#ifdef __cplusplus
}
#endif
