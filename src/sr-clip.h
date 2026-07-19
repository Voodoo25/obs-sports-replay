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

#include <obs-module.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A short video file (sponsor bumper) played as an intro/outro to a replay.
 * Video only; audio streams are ignored. Frames are decoded on the fly and
 * scaled to a requested output size in I420. */
struct sr_clip;

/* Opens a video file. Returns NULL if it can't be opened or has no video. */
struct sr_clip *sr_clip_open(const char *path);
void sr_clip_close(struct sr_clip *c);

uint32_t sr_clip_width(const struct sr_clip *c);
uint32_t sr_clip_height(const struct sr_clip *c);

/* Sets the size that decoded frames are scaled to (usually the replay size). */
void sr_clip_set_output_size(struct sr_clip *c, uint32_t width, uint32_t height);

/* Restarts playback from the first frame. */
void sr_clip_rewind(struct sr_clip *c);

/* Advances the clip to playhead_ns (nanoseconds since the clip started).
 * When a new frame should be shown at this time, returns true and points
 * *out at an I420 AVFrame owned by the clip (valid until the next call).
 * Sets *ended to true once the clip has played through. */
bool sr_clip_advance(struct sr_clip *c, int64_t playhead_ns, AVFrame **out, bool *ended);

#ifdef __cplusplus
}
#endif
