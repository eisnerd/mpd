/*
 * Copyright (C) 2003-2013 The Music Player Daemon Project
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

#include "config.h" /* must be first for large file support */

extern "C" {
#include "song.h"
}

#include "util/UriUtil.hxx"
#include "Directory.hxx"
#include "Mapper.hxx"
#include "fs/Path.hxx"
#include "fs/FileSystem.hxx"
#include "tag.h"
#include "input_stream.h"
#include "decoder_plugin.h"
#include "DecoderList.hxx"

extern "C" {
#include "tag_ape.h"
#include "tag_id3.h"
#include "tag_handler.h"
}

#include <glib.h>

#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>

#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#include <boost/tokenizer.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
using namespace boost;

struct song *
song_file_load(const char *path_utf8, Directory *parent)
{
	struct song *song;
	bool ret;

	assert((parent == NULL) == g_path_is_absolute(path_utf8));
	assert(!uri_has_scheme(path_utf8));
	assert(strchr(path_utf8, '\n') == NULL);

	song = song_file_new(path_utf8, parent);

	//in archive ?
	if (parent != NULL && parent->device == DEVICE_INARCHIVE) {
		ret = song_file_update_inarchive(song);
	} else {
		ret = song_file_update(song);
	}
	if (!ret) {
		song_free(song);
		return NULL;
	}

	return song;
}

/**
 * Attempts to load APE or ID3 tags from the specified file.
 */
static bool
tag_scan_fallback(const char *path,
		  const struct tag_handler *handler, void *handler_ctx)
{
	return tag_ape_scan2(path, handler, handler_ctx) ||
		tag_id3_scan(path, handler, handler_ctx);
}

const char *pre = "Untagged/";
const char *ext = "External/";
bool
song_file_update(struct song *song)
{
	const char *suffix;
	const struct decoder_plugin *plugin;
	struct stat st;
	struct input_stream *is = NULL;

	assert(song_is_file(song));

	/* check if there's a suffix and a plugin */

	suffix = uri_get_suffix(song->uri);
	if (suffix == NULL)
		return false;

	plugin = decoder_plugin_from_suffix(suffix, NULL);
	if (plugin == NULL)
		return false;

	const Path path_fs = map_song_fs(song);
	if (path_fs.IsNull())
		return false;

	if (song->tag != NULL) {
		tag_free(song->tag);
		song->tag = NULL;
	}

	//g_message("Path %s, parent %s, uri %s", path_fs.c_str(), song->parent->GetPath(), song->uri);
	if (strncmp(pre, song->parent->GetPath(), strlen(pre)) == 0
	 || strncmp(ext, song->parent->GetPath(), strlen(ext)) == 0)
	{
		song->tag = tag_new();
		tag_handler_invoke_tag(&full_tag_handler, song->tag, TAG_TITLE, song->uri);
		return true;
	}

	if (!StatFile(path_fs, st) || !S_ISREG(st.st_mode)) {
		return false;
	}

	song->mtime = st.st_mtime;

	Mutex mutex;
	Cond cond;

	do {
		/* load file tag */
		song->tag = tag_new();
		if (decoder_plugin_scan_file(plugin, path_fs.c_str(),
					     &full_tag_handler, song->tag))
			break;

		tag_free(song->tag);
		song->tag = NULL;

		/* fall back to stream tag */
		if (plugin->scan_stream != NULL) {
			/* open the input_stream (if not already
			   open) */
			if (is == NULL) {
				is = input_stream_open(path_fs.c_str(),
						       mutex, cond,
						       NULL);
			}

			/* now try the stream_tag() method */
			if (is != NULL) {
				song->tag = tag_new();
				if (decoder_plugin_scan_stream(plugin, is,
							       &full_tag_handler,
							       song->tag))
					break;

				tag_free(song->tag);
				song->tag = NULL;

				input_stream_lock_seek(is, 0, SEEK_SET, NULL);
			}
		}

		plugin = decoder_plugin_from_suffix(suffix, plugin);
	} while (plugin != NULL);

	if (is != NULL)
		input_stream_close(is);

		cmatch m;
		static const regex path_metadata("\\A.*?Media/(?:.*/)?([^/]+)/+([^/]+)/+(?:([\\d.]+)\\W*[^\\w(](?:\\b|(?=())))?([^/]+?)(?:.([^./]+))?$");
		if (regex_match(path_fs.c_str(), m, path_metadata)) {
			if (song->tag == NULL)
				song->tag = tag_new();	

			int trk = 0;
			std::string tk = m[3].str();
			//char_separator<char> sep(".");
			//tokenizer<char_separator<char>> tokens(m[3].str(), sep);
			std::vector<std::string> tokens;
			boost::split(tokens, tk, boost::is_any_of("."));
			BOOST_FOREACH(std::string t, tokens)
			{
				try
				{
					int x = boost::lexical_cast<int>(t);
					trk *= 100;
					trk += x;
				} catch( boost::bad_lexical_cast const& ) { }
			}

			tag_clear_items_by_type(song->tag, TAG_ARTIST);
			//tag_clear_items_by_type(song->tag, TAG_ALBUM);
			tag_handler_invoke_tag(&full_tag_handler, song->tag, TAG_ARTIST, m[1].str().c_str());
			tag_handler_invoke_tag(&full_tag_handler, song->tag, TAG_ALBUM, m[2].str().c_str());
			tag_handler_invoke_tag(&full_tag_handler, song->tag, TAG_TRACK, std::to_string(trk).c_str());
			tag_handler_invoke_tag(&full_tag_handler, song->tag, TAG_TITLE, m[5].str().c_str());
			tag_handler_invoke_tag(&full_tag_handler, song->tag, TAG_GENRE, m[6].str().c_str());

#ifdef REGEX_DEBUG
			g_message("art %s", m[1].str().c_str());
			g_message("alb %s", m[2].str().c_str());
			g_message("trk %d", trk);
			g_message("tit %s", m[4].str().c_str());
#endif
		}
	if (song->tag != NULL && tag_is_empty(song->tag))
		tag_scan_fallback(path_fs.c_str(), &full_tag_handler,
				  song->tag);

	return song->tag != NULL;
}

bool
song_file_update_inarchive(struct song *song)
{
	const char *suffix;
	const struct decoder_plugin *plugin;

	assert(song_is_file(song));

	/* check if there's a suffix and a plugin */

	suffix = uri_get_suffix(song->uri);
	if (suffix == NULL)
		return false;

	plugin = decoder_plugin_from_suffix(suffix, nullptr);
	if (plugin == NULL)
		return false;

	if (song->tag != NULL)
		tag_free(song->tag);

	//accept every file that has music suffix
	//because we don't support tag reading through
	//input streams
	song->tag = tag_new();

	return true;
}
