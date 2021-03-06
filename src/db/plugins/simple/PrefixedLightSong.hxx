/*
 * Copyright (C) 2003-2014 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPD_DB_SIMPLE_PREFIXED_LIGHT_SONG_HXX
#define MPD_DB_SIMPLE_PREFIXED_LIGHT_SONG_HXX

#include "check.h"
#include "db/LightSong.hxx"
#include "fs/Traits.hxx"

#include <string>

class PrefixedLightSong : public LightSong {
	std::string buffer;

public:
	PrefixedLightSong(const LightSong &song, const char *base)
		:LightSong(song),
		 buffer(PathTraitsUTF8::Build(base, GetURI().c_str())) {
		uri = buffer.c_str();
		directory = nullptr;
	}
};

#endif
