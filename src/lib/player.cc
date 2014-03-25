/*
    Copyright (C) 2013-2014 Carl Hetherington <cth@carlh.net>

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

#include <stdint.h>
#include <algorithm>
#include "player.h"
#include "film.h"
#include "ffmpeg_decoder.h"
#include "audio_buffers.h"
#include "ffmpeg_content.h"
#include "image_decoder.h"
#include "image_content.h"
#include "sndfile_decoder.h"
#include "sndfile_content.h"
#include "subtitle_content.h"
#include "subrip_decoder.h"
#include "subrip_content.h"
#include "playlist.h"
#include "job.h"
#include "image.h"
#include "ratio.h"
#include "log.h"
#include "scaler.h"
#include "render_subtitles.h"
#include "dcp_video.h"
#include "config.h"
#include "content_video.h"

using std::list;
using std::cout;
using std::min;
using std::max;
using std::min;
using std::vector;
using std::pair;
using std::map;
using std::make_pair;
using boost::shared_ptr;
using boost::weak_ptr;
using boost::dynamic_pointer_cast;
using boost::optional;

Player::Player (shared_ptr<const Film> f, shared_ptr<const Playlist> p)
	: _film (f)
	, _playlist (p)
	, _have_valid_pieces (false)
	, _approximate_size (false)
	, _burn_subtitles (false)
{
	_playlist_changed_connection = _playlist->Changed.connect (bind (&Player::playlist_changed, this));
	_playlist_content_changed_connection = _playlist->ContentChanged.connect (bind (&Player::content_changed, this, _1, _2, _3));
	_film_changed_connection = _film->Changed.connect (bind (&Player::film_changed, this, _1));
	set_video_container_size (_film->frame_size ());
}

void
Player::setup_pieces ()
{
	list<shared_ptr<Piece> > old_pieces = _pieces;
	_pieces.clear ();

	ContentList content = _playlist->content ();

	for (ContentList::iterator i = content.begin(); i != content.end(); ++i) {

		if (!(*i)->paths_valid ()) {
			continue;
		}
		
		shared_ptr<Decoder> decoder;
		optional<FrameRateChange> frc;

		/* Work out a FrameRateChange for the best overlap video for this content, in case we need it below */
		DCPTime best_overlap_t;
		shared_ptr<VideoContent> best_overlap;
		for (ContentList::iterator j = content.begin(); j != content.end(); ++j) {
			shared_ptr<VideoContent> vc = dynamic_pointer_cast<VideoContent> (*j);
			if (!vc) {
				continue;
			}
			
			DCPTime const overlap = max (vc->position(), (*i)->position()) - min (vc->end(), (*i)->end());
			if (overlap > best_overlap_t) {
				best_overlap = vc;
				best_overlap_t = overlap;
			}
		}

		optional<FrameRateChange> best_overlap_frc;
		if (best_overlap) {
			best_overlap_frc = FrameRateChange (best_overlap->video_frame_rate(), _film->video_frame_rate ());
		} else {
			/* No video overlap; e.g. if the DCP is just audio */
			best_overlap_frc = FrameRateChange (_film->video_frame_rate(), _film->video_frame_rate ());
		}

		/* FFmpeg */
		shared_ptr<const FFmpegContent> fc = dynamic_pointer_cast<const FFmpegContent> (*i);
		if (fc) {
			decoder.reset (new FFmpegDecoder (fc, _film->log()));
			frc = FrameRateChange (fc->video_frame_rate(), _film->video_frame_rate());
		}

		/* ImageContent */
		shared_ptr<const ImageContent> ic = dynamic_pointer_cast<const ImageContent> (*i);
		if (ic) {
			/* See if we can re-use an old ImageDecoder */
			for (list<shared_ptr<Piece> >::const_iterator j = old_pieces.begin(); j != old_pieces.end(); ++j) {
				shared_ptr<ImageDecoder> imd = dynamic_pointer_cast<ImageDecoder> ((*j)->decoder);
				if (imd && imd->content() == ic) {
					decoder = imd;
				}
			}

			if (!decoder) {
				decoder.reset (new ImageDecoder (ic));
			}

			frc = FrameRateChange (ic->video_frame_rate(), _film->video_frame_rate());
		}

		/* SndfileContent */
		shared_ptr<const SndfileContent> sc = dynamic_pointer_cast<const SndfileContent> (*i);
		if (sc) {
			decoder.reset (new SndfileDecoder (sc));
			frc = best_overlap_frc;
		}

		/* SubRipContent */
		shared_ptr<const SubRipContent> rc = dynamic_pointer_cast<const SubRipContent> (*i);
		if (rc) {
			decoder.reset (new SubRipDecoder (rc));
			frc = best_overlap_frc;
		}

		_pieces.push_back (shared_ptr<Piece> (new Piece (*i, decoder, frc.get ())));
	}

	_have_valid_pieces = true;
}

void
Player::content_changed (weak_ptr<Content> w, int property, bool frequent)
{
	shared_ptr<Content> c = w.lock ();
	if (!c) {
		return;
	}

	if (
		property == ContentProperty::POSITION ||
		property == ContentProperty::LENGTH ||
		property == ContentProperty::TRIM_START ||
		property == ContentProperty::TRIM_END ||
		property == ContentProperty::PATH ||
		property == VideoContentProperty::VIDEO_FRAME_TYPE
		) {
		
		_have_valid_pieces = false;
		Changed (frequent);

	} else if (
		property == SubtitleContentProperty::SUBTITLE_X_OFFSET ||
		property == SubtitleContentProperty::SUBTITLE_Y_OFFSET ||
		property == SubtitleContentProperty::SUBTITLE_SCALE ||
		property == VideoContentProperty::VIDEO_CROP ||
		property == VideoContentProperty::VIDEO_SCALE ||
		property == VideoContentProperty::VIDEO_FRAME_RATE
		) {
		
		Changed (frequent);
	}
}

void
Player::playlist_changed ()
{
	_have_valid_pieces = false;
	Changed (false);
}

void
Player::set_video_container_size (dcp::Size s)
{
	_video_container_size = s;

	_black_image.reset (new Image (PIX_FMT_RGB24, _video_container_size, true));
	_black_image->make_black ();
}

void
Player::film_changed (Film::Property p)
{
	/* Here we should notice Film properties that affect our output, and
	   alert listeners that our output now would be different to how it was
	   last time we were run.
	*/

	if (p == Film::SCALER || p == Film::WITH_SUBTITLES || p == Film::CONTAINER || p == Film::VIDEO_FRAME_RATE) {
		Changed (false);
	}
}

list<PositionImage>
Player::process_content_image_subtitles (shared_ptr<SubtitleContent> content, list<shared_ptr<ContentImageSubtitle> > subs)
{
	list<PositionImage> all;
	
	for (list<shared_ptr<ContentImageSubtitle> >::const_iterator i = subs.begin(); i != subs.end(); ++i) {
		if (!(*i)->image) {
			continue;
		}

		dcpomatic::Rect<double> in_rect = (*i)->rectangle;
		dcp::Size scaled_size;
		
		in_rect.x += content->subtitle_x_offset ();
		in_rect.y += content->subtitle_y_offset ();
		
		/* We will scale the subtitle up to fit _video_container_size, and also by the additional subtitle_scale */
		scaled_size.width = in_rect.width * _video_container_size.width * content->subtitle_scale ();
		scaled_size.height = in_rect.height * _video_container_size.height * content->subtitle_scale ();
		
		/* Then we need a corrective translation, consisting of two parts:
		 *
		 * 1.  that which is the result of the scaling of the subtitle by _video_container_size; this will be
		 *     rect.x * _video_container_size.width and rect.y * _video_container_size.height.
		 *
		 * 2.  that to shift the origin of the scale by subtitle_scale to the centre of the subtitle; this will be
		 *     (width_before_subtitle_scale * (1 - subtitle_scale) / 2) and
		 *     (height_before_subtitle_scale * (1 - subtitle_scale) / 2).
		 *
		 * Combining these two translations gives these expressions.
		 */

		all.push_back (
			PositionImage (
				(*i)->image->scale (
					scaled_size,
					Scaler::from_id ("bicubic"),
					(*i)->image->pixel_format (),
					true
					),
				Position<int> (
					rint (_video_container_size.width * (in_rect.x + (in_rect.width * (1 - content->subtitle_scale ()) / 2))),
					rint (_video_container_size.height * (in_rect.y + (in_rect.height * (1 - content->subtitle_scale ()) / 2)))
					)
				)
			);
	}

	return all;
}

list<PositionImage>
Player::process_content_text_subtitles (list<shared_ptr<ContentTextSubtitle> > sub)
{
	list<PositionImage> all;
	for (list<shared_ptr<ContentTextSubtitle> >::const_iterator i = sub.begin(); i != sub.end(); ++i) {
		if (!(*i)->subs.empty ()) {
			all.push_back (render_subtitles ((*i)->subs, _video_container_size));
		}
	}

	return all;
}

void
Player::set_approximate_size ()
{
	_approximate_size = true;
}

shared_ptr<DCPVideo>
Player::get_video (DCPTime time, bool accurate)
{
	if (!_have_valid_pieces) {
		setup_pieces ();
	}
	
	list<shared_ptr<Piece> > ov = overlaps<VideoContent> (time);
	if (ov.empty ()) {
		/* No video content at this time: return a black frame */
		return shared_ptr<DCPVideo> (
			new DCPVideo (
				_black_image,
				EYES_BOTH,
				Crop (),
				_video_container_size,
				_video_container_size,
				Scaler::from_id ("bicubic"),
				Config::instance()->colour_conversions().front().conversion,
				time
				)
			);
	}

	/* Create a DCPVideo from the content's video at this time */

	shared_ptr<Piece> piece = ov.back ();
	shared_ptr<VideoDecoder> decoder = dynamic_pointer_cast<VideoDecoder> (piece->decoder);
	assert (decoder);
	shared_ptr<VideoContent> content = dynamic_pointer_cast<VideoContent> (piece->content);
	assert (content);

	shared_ptr<ContentVideo> dec = decoder->get_video (dcp_to_content_video (piece, time), accurate);

	dcp::Size image_size = content->scale().size (content, _video_container_size, _film->frame_size ());
	if (_approximate_size) {
		image_size.width &= ~3;
		image_size.height &= ~3;
	}

	shared_ptr<DCPVideo> dcp_video (
		new DCPVideo (
			dec->image,
			dec->eyes,
			content->crop (),
			image_size,
			_video_container_size,
			_film->scaler(),
			content->colour_conversion (),
			time
			)
		);

	/* Add subtitles */

	ov = overlaps<SubtitleContent> (time);
	list<PositionImage> sub_images;
	
	for (list<shared_ptr<Piece> >::const_iterator i = ov.begin(); i != ov.end(); ++i) {
		shared_ptr<SubtitleDecoder> subtitle_decoder = dynamic_pointer_cast<SubtitleDecoder> ((*i)->decoder);
		shared_ptr<SubtitleContent> subtitle_content = dynamic_pointer_cast<SubtitleContent> ((*i)->content);
		ContentTime const from = dcp_to_content_subtitle (*i, time);
		ContentTime const to = from + ContentTime::from_frames (1, content->video_frame_rate ());
		
		list<shared_ptr<ContentImageSubtitle> > image_subtitles = subtitle_decoder->get_image_subtitles (from, to);
		if (!image_subtitles.empty ()) {
			list<PositionImage> im = process_content_image_subtitles (
				subtitle_content,
				image_subtitles
				);

			copy (im.begin(), im.end(), back_inserter (sub_images));
		}

		if (_burn_subtitles) {
			list<shared_ptr<ContentTextSubtitle> > text_subtitles = subtitle_decoder->get_text_subtitles (from, to);
			if (!text_subtitles.empty ()) {
				list<PositionImage> im = process_content_text_subtitles (text_subtitles);
				copy (im.begin(), im.end(), back_inserter (sub_images));
			}
		}
	}

	if (!sub_images.empty ()) {
		dcp_video->set_subtitle (merge (sub_images));
	}

	return dcp_video;
}

shared_ptr<AudioBuffers>
Player::get_audio (DCPTime time, DCPTime length, bool accurate)
{
	if (!_have_valid_pieces) {
		setup_pieces ();
	}

	AudioFrame const length_frames = length.frames (_film->audio_frame_rate ());

	shared_ptr<AudioBuffers> audio (new AudioBuffers (_film->audio_channels(), length_frames));
	audio->make_silent ();
	
	list<shared_ptr<Piece> > ov = overlaps<AudioContent> (time);
	if (ov.empty ()) {
		return audio;
	}

	for (list<shared_ptr<Piece> >::iterator i = ov.begin(); i != ov.end(); ++i) {

		shared_ptr<AudioContent> content = dynamic_pointer_cast<AudioContent> ((*i)->content);
		assert (content);
		shared_ptr<AudioDecoder> decoder = dynamic_pointer_cast<AudioDecoder> ((*i)->decoder);
		assert (decoder);

		AudioFrame const content_time = dcp_to_content_audio (*i, time);

		/* Audio from this piece's decoder (which might be more than what we asked for) */
		shared_ptr<ContentAudio> all = decoder->get_audio (content_time, length_frames, accurate);

		/* Gain */
		if (content->audio_gain() != 0) {
			shared_ptr<AudioBuffers> gain (new AudioBuffers (all->audio));
			gain->apply_gain (content->audio_gain ());
			all->audio = gain;
		}

		/* Remap channels */
		shared_ptr<AudioBuffers> dcp_mapped (new AudioBuffers (_film->audio_channels(), all->audio->frames()));
		dcp_mapped->make_silent ();
		AudioMapping map = content->audio_mapping ();
		for (int i = 0; i < map.content_channels(); ++i) {
			for (int j = 0; j < _film->audio_channels(); ++j) {
				if (map.get (i, static_cast<dcp::Channel> (j)) > 0) {
					dcp_mapped->accumulate_channel (
						all->audio.get(),
						i,
						j,
						map.get (i, static_cast<dcp::Channel> (j))
						);
				}
			}
		}
		
		all->audio = dcp_mapped;
		
		/* Delay */
		/* XXX
		audio->dcp_time += content->audio_delay() * TIME_HZ / 1000;
		if (audio->dcp_time < 0) {
			int const frames = - audio->dcp_time * _film->audio_frame_rate() / TIME_HZ;
			if (frames >= audio->audio->frames ()) {
				return;
			}
			
			shared_ptr<AudioBuffers> trimmed (new AudioBuffers (audio->audio->channels(), audio->audio->frames() - frames));
			trimmed->copy_from (audio->audio.get(), audio->audio->frames() - frames, frames, 0);
			
			audio->audio = trimmed;
			audio->dcp_time = 0;
		}
		*/

		audio->accumulate_frames (all->audio.get(), all->frame - content_time, 0, min (AudioFrame (all->audio->frames()), length_frames));
	}

	return audio;
}

VideoFrame
Player::dcp_to_content_video (shared_ptr<const Piece> piece, DCPTime t) const
{
	/* s is the offset of t from the start position of this content */
	DCPTime s = t - piece->content->position ();
	s = DCPTime (max (int64_t (0), s.get ()));
	s = DCPTime (min (piece->content->length_after_trim().get(), s.get()));

	/* Convert this to the content frame */
	return DCPTime (s + piece->content->trim_start()).frames (_film->video_frame_rate()) * piece->frc.factor ();
}

AudioFrame
Player::dcp_to_content_audio (shared_ptr<const Piece> piece, DCPTime t) const
{
	/* s is the offset of t from the start position of this content */
	DCPTime s = t - piece->content->position ();
	s = DCPTime (max (int64_t (0), s.get ()));
	s = DCPTime (min (piece->content->length_after_trim().get(), s.get()));

	/* Convert this to the content frame */
	return DCPTime (s + piece->content->trim_start()).frames (_film->audio_frame_rate());
}

ContentTime
Player::dcp_to_content_subtitle (shared_ptr<const Piece> piece, DCPTime t) const
{
	/* s is the offset of t from the start position of this content */
	DCPTime s = t - piece->content->position ();
	s = DCPTime (max (int64_t (0), s.get ()));
	s = DCPTime (min (piece->content->length_after_trim().get(), s.get()));

	return ContentTime (s, piece->frc);
}

void
PlayerStatistics::dump (shared_ptr<Log> log) const
{
	log->log (String::compose ("Video: %1 good %2 skipped %3 black %4 repeat", video.good, video.skip, video.black, video.repeat));
	log->log (String::compose ("Audio: %1 good %2 skipped %3 silence", audio.good, audio.skip, audio.silence.seconds()));
}

PlayerStatistics const &
Player::statistics () const
{
	return _statistics;
}
