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

/* The folder where replays are saved and read from, shared by auto-save and
 * the replay dock, persisted to the plugin config directory. The getter
 * returns a bstrdup the caller frees. */
void sr_config_init(void);
void sr_config_free(void);

char *sr_config_get_save_dir(void);
void sr_config_set_save_dir(const char *save_dir);

#ifdef __cplusplus
}
#endif
