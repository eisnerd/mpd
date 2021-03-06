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

#include "config.h" /* must be first for large file support */
#include "DetachedSong.hxx"
#include "db/plugins/simple/Song.hxx"
#include "db/plugins/simple/Directory.hxx"
#include "storage/StorageInterface.hxx"
#include "storage/FileInfo.hxx"
#include "util/UriUtil.hxx"
#include "util/Error.hxx"
#include "fs/AllocatedPath.hxx"
#include "fs/Traits.hxx"
#include "fs/FileSystem.hxx"
#include "decoder/DecoderList.hxx"
#include "tag/Tag.hxx"
#include "tag/TagBuilder.hxx"
#include "tag/TagHandler.hxx"
#include "tag/TagId3.hxx"
#include "tag/ApeTag.hxx"
#include "TagFile.hxx"
#include "TagStream.hxx"

#include <assert.h>
#include <string.h>
#include <sys/stat.h>

#include "Log.hxx"
#include "db/update/UpdateDomain.hxx"

#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#include <boost/tokenizer.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
using namespace boost;

#ifdef ENABLE_DATABASE

Song *
Song::LoadFile(Storage &storage, const char *path_utf8, Directory &parent)
{
	assert(!uri_has_scheme(path_utf8));
	assert(strchr(path_utf8, '\n') == nullptr);

	Song *song = NewFile(path_utf8, parent);

	//in archive ?
	bool success = parent.device == DEVICE_INARCHIVE
		? song->UpdateFileInArchive(storage)
		: song->UpdateFile(storage);
	if (!success) {
		song->Free();
		return nullptr;
	}

	return song;
}

#endif

/**
 * Attempts to load APE or ID3 tags from the specified file.
 */
static bool
tag_scan_fallback(Path path,
		  const struct tag_handler *handler, void *handler_ctx)
{
	return tag_ape_scan2(path, handler, handler_ctx) ||
		tag_id3_scan(path, handler, handler_ctx);
}

#ifdef ENABLE_DATABASE

const char *pre = "Untagged/";
const char *ext = "External/";
bool
Song::UpdateFile(Storage &storage)
{
	const auto &relative_uri = GetURI();

	FileInfo info;
	if (!storage.GetInfo(relative_uri.c_str(), true, info, IgnoreError()))
		return false;

	if (!info.IsRegular())
		return false;

	{
		TagBuilder tag_builder;

		const auto path_fs = storage.MapFS(relative_uri.c_str());
		if (path_fs.IsNull()) {
			const auto absolute_uri =
				storage.MapUTF8(relative_uri.c_str());
			if (!tag_stream_scan(absolute_uri.c_str(),
						 full_tag_handler, &tag_builder))
				return false;
		} else {
			if (!tag_file_scan(path_fs, full_tag_handler, &tag_builder))
				return false;

			if (tag_builder.IsEmpty())
				tag_scan_fallback(path_fs, &full_tag_handler,
						  &tag_builder);
		}

		mtime = info.mtime;
		tag_builder.Commit(tag);
	}

	//g_message("Path %s, parent %s, uri %s", path_fs.c_str(), song->parent->GetPath(), song->uri);
	if (strncmp(pre, relative_uri.c_str(), strlen(pre)) == 0
	 || strncmp(ext, relative_uri.c_str(), strlen(ext)) == 0) {
		const char *a = tag.GetValue(TAG_ARTIST);
		TagBuilder tag_builder(tag);

		tag_builder.RemoveType(TAG_GENRE);
		if (a)
			tag_builder.AddItem(TAG_GENRE, a);
		tag_builder.RemoveType(TAG_ARTIST);
		tag_builder.RemoveType(TAG_TRACK);

		tag_builder.AddItem(TAG_TITLE, relative_uri.c_str());

		tag_builder.Commit(tag);
	} else {
		cmatch m;
		static const regex path_metadata(R"(\A.*?Media/(?:.*/)?([^/]+)/+([^/]+)/+(?:([\d.]+)\W*[^\w(](?:\b|(?=())))?([^/]+?)(?:.([^./]+))?$)");
		if (regex_match(relative_uri.c_str(), m, path_metadata)) {
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

			TagBuilder tag_builder(tag);

			tag_builder.RemoveType(TAG_ARTIST);
			tag_builder.RemoveType(TAG_ALBUM);

			tag_builder.AddItem(TAG_ARTIST, m[1].str().c_str());
			tag_builder.AddItem(TAG_ALBUM, m[2].str().c_str());
			tag_builder.AddItem(TAG_TRACK, std::to_string(trk).c_str());
			tag_builder.AddItem(TAG_TITLE, m[5].str().c_str());
			tag_builder.AddItem(TAG_GENRE, m[6].str().c_str());

			tag_builder.Commit(tag);

	#ifdef REGEX_DEBUG
			FormatDebug(update_domain, "art %s", m[1].str().c_str());
			FormatDebug(update_domain, "alb %s", m[2].str().c_str());
			FormatDebug(update_domain, "trk %d", trk);
			FormatDebug(update_domain, "tit %s", m[4].str().c_str());
	#endif
		}
	}

	return true;
}

bool
Song::UpdateFileInArchive(const Storage &storage)
{
	/* check if there's a suffix and a plugin */

	const char *suffix = uri_get_suffix(uri);
	if (suffix == nullptr)
		return false;

	if (!decoder_plugins_supports_suffix(suffix))
		return false;

	const auto path_fs = parent->IsRoot()
		? storage.MapFS(uri)
		: storage.MapChildFS(parent->GetPath(), uri);
	if (path_fs.IsNull())
		return false;

	TagBuilder tag_builder;
	if (!tag_stream_scan(path_fs.c_str(), full_tag_handler, &tag_builder))
		return false;

	tag_builder.Commit(tag);
	return true;
}

#endif

bool
DetachedSong::Update()
{
	if (IsAbsoluteFile()) {
		const AllocatedPath path_fs =
			AllocatedPath::FromUTF8(GetRealURI());

		struct stat st;
		if (!StatFile(path_fs, st) || !S_ISREG(st.st_mode))
			return false;

		TagBuilder tag_builder;
		if (!tag_file_scan(path_fs, full_tag_handler, &tag_builder))
			return false;

		if (tag_builder.IsEmpty())
			tag_scan_fallback(path_fs, &full_tag_handler,
					  &tag_builder);

		mtime = st.st_mtime;
		tag_builder.Commit(tag);
		return true;
	} else if (IsRemote()) {
		TagBuilder tag_builder;
		if (!tag_stream_scan(uri.c_str(), full_tag_handler,
				     &tag_builder))
			return false;

		mtime = 0;
		tag_builder.Commit(tag);
		return true;
	} else
		// TODO: implement
		return false;
}
