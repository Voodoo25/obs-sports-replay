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
#include <plugin-support.h>

#include <stdio.h>

/* Builds the "<a href="...">Sports Replay (version) by Systec</a>" credit
 * line shown at the bottom of the plugin's windows/dialogs, into buf.
 * Returns buf. Requires obs_module_text() to be usable (i.e. the OBS module
 * is loaded), so only include this from translation units compiled into the
 * main plugin module, not the standalone plugin-support library. */
static inline const char *sr_plugin_credit_html(char *buf, size_t size)
{
	snprintf(buf, size, "<a href=\"%s\">%s (%s) %s Systec</a>", PLUGIN_WEBSITE, obs_module_text("SportsReplay"),
		 PLUGIN_VERSION, obs_module_text("Credit.By"));
	return buf;
}
