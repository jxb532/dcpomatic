/*
    Copyright (C) 2014-2016 Carl Hetherington <cth@carlh.net>

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

#ifndef DCPOMATIC_DCP_CONTENT_H
#define DCPOMATIC_DCP_CONTENT_H

/** @file  src/lib/dcp_content.h
 *  @brief DCPContent class.
 */

#include "single_stream_audio_content.h"
#include "subtitle_content.h"
#include <libcxml/cxml.h>
#include <dcp/encrypted_kdm.h>

class DCPContentProperty
{
public:
	static int const CAN_BE_PLAYED;
	static int const REFERENCE_VIDEO;
	static int const REFERENCE_AUDIO;
	static int const REFERENCE_SUBTITLE;
};

/** @class DCPContent
 *  @brief An existing DCP used as input.
 */
class DCPContent : public SingleStreamAudioContent, public SubtitleContent
{
public:
	DCPContent (boost::shared_ptr<const Film>, boost::filesystem::path p);
	DCPContent (boost::shared_ptr<const Film>, cxml::ConstNodePtr, int version);

	boost::shared_ptr<DCPContent> shared_from_this () {
		return boost::dynamic_pointer_cast<DCPContent> (Content::shared_from_this ());
	}

	boost::shared_ptr<const DCPContent> shared_from_this () const {
		return boost::dynamic_pointer_cast<const DCPContent> (Content::shared_from_this ());
	}

	DCPTime full_length () const;

	void examine (boost::shared_ptr<Job>);
	std::string summary () const;
	std::string technical_summary () const;
	void as_xml (xmlpp::Node *) const;
	std::string identifier () const;

	void set_default_colour_conversion ();
	std::list<DCPTime> reel_split_points () const;

	/* SubtitleContent */

	bool has_text_subtitles () const {
		boost::mutex::scoped_lock lm (_mutex);
		return _has_subtitles;
	}

	bool has_image_subtitles () const {
		return false;
	}

	double subtitle_video_frame_rate () const;

	boost::filesystem::path directory () const;

	bool encrypted () const {
		boost::mutex::scoped_lock lm (_mutex);
		return _encrypted;
	}

	void add_kdm (dcp::EncryptedKDM);

	boost::optional<dcp::EncryptedKDM> kdm () const {
		return _kdm;
	}

	bool can_be_played () const;

	void set_reference_video (bool r);

	bool reference_video () const {
		boost::mutex::scoped_lock lm (_mutex);
		return _reference_video;
	}

	bool can_reference_video (std::list<std::string> &) const;

	void set_reference_audio (bool r);

	bool reference_audio () const {
		boost::mutex::scoped_lock lm (_mutex);
		return _reference_audio;
	}

	bool can_reference_audio (std::list<std::string> &) const;

	void set_reference_subtitle (bool r);

	bool reference_subtitle () const {
		boost::mutex::scoped_lock lm (_mutex);
		return _reference_subtitle;
	}

	bool can_reference_subtitle (std::list<std::string> &) const;

protected:
	void add_properties (std::list<UserProperty>& p) const;

private:
	void read_directory (boost::filesystem::path);
	std::list<DCPTimePeriod> reels () const;
	template <class T> bool can_reference (std::string overlapping, std::list<std::string>& why_not) const;

	std::string _name;
	bool _has_subtitles;
	/** true if our DCP is encrypted */
	bool _encrypted;
	boost::optional<dcp::EncryptedKDM> _kdm;
	/** true if _kdm successfully decrypts the first frame of our DCP */
	bool _kdm_valid;
	/** true if the video in this DCP should be included in the output by reference
	 *  rather than by rewrapping.
	 */
	bool _reference_video;
	/** true if the audio in this DCP should be included in the output by reference
	 *  rather than by rewrapping.
	 */
	bool _reference_audio;
	/** true if the subtitle in this DCP should be included in the output by reference
	 *  rather than by rewrapping.
	 */
	bool _reference_subtitle;
};

#endif
