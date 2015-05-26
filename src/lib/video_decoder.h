/*
    Copyright (C) 2012-2015 Carl Hetherington <cth@carlh.net>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

/** @file  src/lib/video_decoder.h
 *  @brief VideoDecoder class.
 */

#ifndef DCPOMATIC_VIDEO_DECODER_H
#define DCPOMATIC_VIDEO_DECODER_H

#include "decoder.h"
#include "video_content.h"
#include "util.h"
#include "content_video.h"
#include <boost/signals2.hpp>
#include <boost/shared_ptr.hpp>

class VideoContent;
class ImageProxy;
class Image;

/** @class VideoDecoder
 *  @brief Parent for classes which decode video.
 */
class VideoDecoder : public virtual Decoder
{
public:
	VideoDecoder (boost::shared_ptr<const VideoContent> c);

	std::list<ContentVideo> get_video (Frame frame, bool accurate);

	boost::shared_ptr<const VideoContent> video_content () const {
		return _video_content;
	}

	void set_ignore_video ();

#ifdef DCPOMATIC_DEBUG
	int test_gaps;
#endif

protected:
	friend struct video_decoder_fill_test1;
	friend struct video_decoder_fill_test2;

	void seek (ContentTime time, bool accurate);
	void video (boost::shared_ptr<const ImageProxy>, Frame frame);
	std::list<ContentVideo> decoded_video (Frame frame);
	void fill_2d (Frame from, Frame to);
	void fill_3d (Frame from, Frame to, Eyes);

	boost::shared_ptr<const VideoContent> _video_content;
	std::list<ContentVideo> _decoded_video;
	boost::shared_ptr<Image> _black_image;
	boost::optional<ContentTime> _last_seek_time;
	bool _last_seek_accurate;

	/** true if this decoder should ignore all video; i.e. never produce any */
	bool _ignore_video;
};

#endif
