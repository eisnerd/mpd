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

/* necessary because libavutil/common.h uses UINT64_C */
#define __STDC_CONSTANT_MACROS

#include "config.h"
#include "FfmpegMetaData.hxx"
#include "tag_table.h"
#include "tag_handler.h"
#include <boost/regex.hpp>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "ffmpeg"

static const struct tag_table ffmpeg_tags[] = {
	{ "year", TAG_DATE },
	{ "author-sort", TAG_ARTIST_SORT },
	{ "album_artist", TAG_ALBUM_ARTIST },
	{ "album_artist-sort", TAG_ALBUM_ARTIST_SORT },

	/* sentinel */
	{ NULL, TAG_NUM_OF_ITEM_TYPES }
};

static boost::regex delim("; +");

static void
ffmpeg_copy_metadata(enum tag_type type,
		     AVDictionary *m, const char *name,
		     const struct tag_handler *handler, void *handler_ctx)
{
	AVDictionaryEntry *mt = NULL;

	while ((mt = av_dict_get(m, name, mt, 0)) != NULL)
	{
		boost::cregex_token_iterator i(mt->value, mt->value + strlen(mt->value), delim, -1);
		boost::cregex_token_iterator j;
		while (i != j)
		tag_handler_invoke_tag(handler, handler_ctx,
				       type, (i++)->str().c_str());
	}
}

static void
ffmpeg_scan_pairs(AVDictionary *dict,
		  const struct tag_handler *handler, void *handler_ctx)
{
	AVDictionaryEntry *i = NULL;

	while ((i = av_dict_get(dict, "", i, AV_DICT_IGNORE_SUFFIX)) != NULL)
		if (strcmp(i->key, "---:com.apple.iTunes:replaygain_track_gain"))
			{}
		else
		tag_handler_invoke_pair(handler, handler_ctx,
					i->key, i->value);
}

void
ffmpeg_scan_dictionary(AVDictionary *dict,
		       const struct tag_handler *handler, void *handler_ctx)
{
	for (unsigned i = 0; i < TAG_NUM_OF_ITEM_TYPES; ++i)
		ffmpeg_copy_metadata(tag_type(i), dict, tag_item_names[i],
				     handler, handler_ctx);

	for (const struct tag_table *i = ffmpeg_tags;
	     i->name != NULL; ++i)
		ffmpeg_copy_metadata(i->type, dict, i->name,
				     handler, handler_ctx);

	if (handler->pair != NULL)
		ffmpeg_scan_pairs(dict, handler, handler_ctx);
}
