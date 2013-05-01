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

#include "config.h"
#include "TagStream.hxx"
#include "tag/TagHandler.hxx"
#include "util/UriUtil.hxx"
#include "util/Error.hxx"
#include "decoder/DecoderList.hxx"
#include "decoder/DecoderPlugin.hxx"
#include "input/InputStream.hxx"
#include "thread/Mutex.hxx"
#include "thread/Cond.hxx"

#include <assert.h>

#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#include <boost/tokenizer.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
using namespace boost;

/**
 * Does the #DecoderPlugin support either the suffix or the MIME type?
 */
gcc_pure
static bool
CheckDecoderPlugin(const DecoderPlugin &plugin,
		   const char *suffix, const char *mime)
{
	return (mime != nullptr && plugin.SupportsMimeType(mime)) ||
		(suffix != nullptr && plugin.SupportsSuffix(suffix));
}

bool
tag_stream_scan(InputStream &is, const tag_handler &handler, void *ctx)
{
	assert(is.IsReady());

	const char *const suffix = uri_get_suffix(is.GetURI());
	const char *const mime = is.GetMimeType();

	if (suffix == nullptr && mime == nullptr)
		return false;

	return decoder_plugins_try([suffix, mime, &is,
				    &handler, ctx](const DecoderPlugin &plugin){
			is.LockRewind(IgnoreError());

			return CheckDecoderPlugin(plugin, suffix, mime) &&
				plugin.ScanStream(is, handler, ctx);
		});
}

bool
tag_stream_scan(const char *uri, const tag_handler &handler, void *ctx)
{
	Mutex mutex;
	Cond cond;

	InputStream *is = InputStream::OpenReady(uri, mutex, cond,
						 IgnoreError());
	if (is == nullptr)
		return false;

	bool success = tag_stream_scan(*is, handler, ctx);
	delete is;

	cmatch m;
	static const regex path_metadata(R"(\A.*?Media/(?:.*/)?([^/]+)/+([^/]+)/+(?:([\d.]+)\W*[^\w(](?:\b|(?=())))?([^/]+?)(?:.([^./]+))?$)");
	if (regex_match(uri, m, path_metadata)) {
		//if (song->tag == NULL)
			//song->tag = tag_new();

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

		tag_clear_items_by_type(ctx, TAG_ARTIST);
		//tag_clear_items_by_type(ctx, TAG_ALBUM);
		tag_handler_invoke_tag(&handler, ctx, TAG_ARTIST, m[1].str().c_str());
		tag_handler_invoke_tag(&handler, ctx, TAG_ALBUM, m[2].str().c_str());
		tag_handler_invoke_tag(&handler, ctx, TAG_TRACK, std::to_string(trk).c_str());
		tag_handler_invoke_tag(&handler, ctx, TAG_TITLE, m[5].str().c_str());
		tag_handler_invoke_tag(&handler, ctx, TAG_GENRE, m[6].str().c_str());

#ifdef REGEX_DEBUG
		g_message("art %s", m[1].str().c_str());
		g_message("alb %s", m[2].str().c_str());
		g_message("trk %d", trk);
		g_message("tit %s", m[4].str().c_str());
#endif
	}

	return success;
}
