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

#include "sr-buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Muxes a captured replay's already-encoded packets into an mp4 file at
 * path, without re-encoding. Returns true on success. */
bool sr_save_replay(const struct sr_replay *r, const char *path);

#ifdef __cplusplus
}
#endif
