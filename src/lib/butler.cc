/*
    Copyright (C) 2016-2018 Carl Hetherington <cth@carlh.net>

    This file is part of DCP-o-matic.

    DCP-o-matic is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    DCP-o-matic is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with DCP-o-matic.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "butler.h"
#include "player.h"
#include "util.h"
#include "log.h"
#include "dcpomatic_log.h"
#include "cross.h"
#include "compose.hpp"
#include "exceptions.h"
#include <boost/weak_ptr.hpp>
#include <boost/shared_ptr.hpp>

using std::cout;
using std::pair;
using std::make_pair;
using std::string;
using boost::weak_ptr;
using boost::shared_ptr;
using boost::bind;
using boost::optional;
using boost::function;

/** Minimum video readahead in frames */
#define MINIMUM_VIDEO_READAHEAD 10
/** Maximum video readahead in frames; should never be exceeded (by much) unless there are bugs in Player */
#define MAXIMUM_VIDEO_READAHEAD 48
/** Minimum audio readahead in frames */
#define MINIMUM_AUDIO_READAHEAD (48000 * MINIMUM_VIDEO_READAHEAD / 24)
/** Maximum audio readahead in frames; should never be exceeded (by much) unless there are bugs in Player */
#define MAXIMUM_AUDIO_READAHEAD (48000 * MAXIMUM_VIDEO_READAHEAD / 24)

/** @param pixel_format Pixel format functor that will be used when calling ::image on PlayerVideos coming out of this
 *  butler.  This will be used (where possible) to prepare the PlayerVideos so that calling image() on them is quick.
 *  @param aligned Same as above for the `aligned' flag.
 *  @param fast Same as above for the `fast' flag.
 */
Butler::Butler (
	shared_ptr<Player> player,
	AudioMapping audio_mapping,
	int audio_channels,
	function<AVPixelFormat (AVPixelFormat)> pixel_format,
	bool aligned,
	bool fast
	)
	: _player (player)
	, _prepare_work (new boost::asio::io_service::work (_prepare_service))
	, _pending_seek_accurate (false)
	, _suspended (0)
	, _finished (false)
	, _died (false)
	, _stop_thread (false)
	, _audio_mapping (audio_mapping)
	, _audio_channels (audio_channels)
	, _disable_audio (false)
	, _pixel_format (pixel_format)
	, _aligned (aligned)
	, _fast (fast)
{
	_player_video_connection = _player->Video.connect (bind (&Butler::video, this, _1, _2));
	_player_audio_connection = _player->Audio.connect (bind (&Butler::audio, this, _1, _2, _3));
	_player_text_connection = _player->Text.connect (bind (&Butler::text, this, _1, _2, _3, _4));
	/* The butler must hear about things first, otherwise it might not sort out suspensions in time for
	   get_video() to be called in response to this signal.
	*/
	_player_change_connection = _player->Change.connect (bind (&Butler::player_change, this, _1, _3), boost::signals2::at_front);
	_thread = new boost::thread (bind (&Butler::thread, this));
#ifdef DCPOMATIC_LINUX
	pthread_setname_np (_thread->native_handle(), "butler");
#endif

	/* Create some threads to do work on the PlayerVideos we are creating; at present this is used to
	   multi-thread JPEG2000 decoding.
	*/

	LOG_TIMING("start-prepare-threads %1", boost::thread::hardware_concurrency() * 2);

	for (size_t i = 0; i < boost::thread::hardware_concurrency() * 2; ++i) {
		_prepare_pool.create_thread (bind (&boost::asio::io_service::run, &_prepare_service));
	}
}

Butler::~Butler ()
{
	{
		boost::mutex::scoped_lock lm (_mutex);
		_stop_thread = true;
	}

	_prepare_work.reset ();
	_prepare_pool.join_all ();
	_prepare_service.stop ();

	_thread->interrupt ();
	try {
		_thread->join ();
	} catch (boost::thread_interrupted& e) {
		/* No problem */
	}
	delete _thread;
}

/** Caller must hold a lock on _mutex */
bool
Butler::should_run () const
{
	if (_video.size() >= MAXIMUM_VIDEO_READAHEAD * 10) {
		/* This is way too big */
		optional<DCPTime> pos = _audio.peek();
		if (pos) {
			throw ProgrammingError
				(__FILE__, __LINE__, String::compose ("Butler video buffers reached %1 frames (audio is %2 at %3)", _video.size(), _audio.size(), pos->get()));
		} else {
			throw ProgrammingError
				(__FILE__, __LINE__, String::compose ("Butler video buffers reached %1 frames (audio is %2)", _video.size(), _audio.size()));
		}
	}

	if (_audio.size() >= MAXIMUM_AUDIO_READAHEAD * 10) {
		/* This is way too big */
		optional<DCPTime> pos = _audio.peek();
		if (pos) {
			throw ProgrammingError
				(__FILE__, __LINE__, String::compose ("Butler audio buffers reached %1 frames at %2 (video is %3)", _audio.size(), pos->get(), _video.size()));
		} else {
			throw ProgrammingError
				(__FILE__, __LINE__, String::compose ("Butler audio buffers reached %1 frames (video is %3)", _audio.size(), _video.size()));
		}
	}

	if (_video.size() >= MAXIMUM_VIDEO_READAHEAD * 2) {
		LOG_WARNING ("Butler video buffers reached %1 frames (audio is %2)", _video.size(), _audio.size());
	}

	if (_audio.size() >= MAXIMUM_AUDIO_READAHEAD * 2) {
		LOG_WARNING ("Butler audio buffers reached %1 frames (video is %2)", _audio.size(), _video.size());
	}

	if (_stop_thread || _finished || _died || _suspended) {
		/* Definitely do not run */
		return false;
	}

	if (_video.size() < MINIMUM_VIDEO_READAHEAD || (!_disable_audio && _audio.size() < MINIMUM_AUDIO_READAHEAD)) {
		/* Definitely do run: we need data */
		return true;
	}

	/* Run if we aren't full of video or audio */
	return (_video.size() < MAXIMUM_VIDEO_READAHEAD) && (_audio.size() < MAXIMUM_AUDIO_READAHEAD);
}

void
Butler::thread ()
try
{
	while (true) {
		boost::mutex::scoped_lock lm (_mutex);

		/* Wait until we have something to do */
		while (!should_run() && !_pending_seek_position) {
			_summon.wait (lm);
		}

		/* Do any seek that has been requested */
		if (_pending_seek_position) {
			_finished = false;
			_player->seek (*_pending_seek_position, _pending_seek_accurate);
			_pending_seek_position = optional<DCPTime> ();
		}

		/* Fill _video and _audio.  Don't try to carry on if a pending seek appears
		   while lm is unlocked, as in that state nothing will be added to
		   _video/_audio.
		*/
		while (should_run() && !_pending_seek_position) {
			lm.unlock ();
			bool const r = _player->pass ();
			lm.lock ();
			if (r) {
				_finished = true;
				_arrived.notify_all ();
				break;
			}
			_arrived.notify_all ();
		}
	}
} catch (boost::thread_interrupted) {
	/* The butler thread is being terminated */
	boost::mutex::scoped_lock lm (_mutex);
	_finished = true;
	_arrived.notify_all ();
} catch (...) {
	store_current ();
	boost::mutex::scoped_lock lm (_mutex);
	_died = true;
	_arrived.notify_all ();
}

pair<shared_ptr<PlayerVideo>, DCPTime>
Butler::get_video (Error* e)
{
	boost::mutex::scoped_lock lm (_mutex);

	if (_suspended) {
		if (e) {
			*e = AGAIN;
		}
		return make_pair(shared_ptr<PlayerVideo>(), DCPTime());
	}

	/* Wait for data if we have none */
	while (_video.empty() && !_finished && !_died) {
		_arrived.wait (lm);
	}

	if (_video.empty()) {
		if (e) {
			*e = NONE;
		}
		return make_pair(shared_ptr<PlayerVideo>(), DCPTime());
	}

	pair<shared_ptr<PlayerVideo>, DCPTime> const r = _video.get ();
	_summon.notify_all ();
	return r;
}

optional<TextRingBuffers::Data>
Butler::get_closed_caption ()
{
	boost::mutex::scoped_lock lm (_mutex);
	return _closed_caption.get ();
}

void
Butler::seek (DCPTime position, bool accurate)
{
	boost::mutex::scoped_lock lm (_mutex);
	_awaiting = optional<DCPTime>();
	seek_unlocked (position, accurate);
}

void
Butler::seek_unlocked (DCPTime position, bool accurate)
{
	if (_died) {
		return;
	}

	_finished = false;
	_pending_seek_position = position;
	_pending_seek_accurate = accurate;

	{
		boost::mutex::scoped_lock lm (_buffers_mutex);
		_video.clear ();
		_audio.clear ();
		_closed_caption.clear ();
	}

	_summon.notify_all ();
}

void
Butler::prepare (weak_ptr<PlayerVideo> weak_video)
try
{
	shared_ptr<PlayerVideo> video = weak_video.lock ();
	/* If the weak_ptr cannot be locked the video obviously no longer requires any work */
	if (video) {
		LOG_TIMING("start-prepare in %1", thread_id());
		video->prepare (_pixel_format, _aligned, _fast);
		LOG_TIMING("finish-prepare in %1", thread_id());
	}
}
catch (...)
{
	store_current ();
	boost::mutex::scoped_lock lm (_mutex);
	_died = true;
}

void
Butler::video (shared_ptr<PlayerVideo> video, DCPTime time)
{
	boost::mutex::scoped_lock lm (_mutex);

	if (_pending_seek_position) {
		/* Don't store any video in this case */
		return;
	}

	_prepare_service.post (bind (&Butler::prepare, this, weak_ptr<PlayerVideo>(video)));

	boost::mutex::scoped_lock lm2 (_buffers_mutex);
	_video.put (video, time);
}

void
Butler::audio (shared_ptr<AudioBuffers> audio, DCPTime time, int frame_rate)
{
	{
		boost::mutex::scoped_lock lm (_mutex);
		if (_pending_seek_position || _disable_audio) {
			/* Don't store any audio in these cases */
			return;
		}
	}

	boost::mutex::scoped_lock lm2 (_buffers_mutex);
	_audio.put (remap (audio, _audio_channels, _audio_mapping), time, frame_rate);
}

/** Try to get `frames' frames of audio and copy it into `out'.  Silence
 *  will be filled if no audio is available.
 *  @return time of this audio, or unset if there was a buffer underrun.
 */
optional<DCPTime>
Butler::get_audio (float* out, Frame frames)
{
	optional<DCPTime> t = _audio.get (out, _audio_channels, frames);
	_summon.notify_all ();
	return t;
}

void
Butler::disable_audio ()
{
	boost::mutex::scoped_lock lm (_mutex);
	_disable_audio = true;
}

pair<size_t, string>
Butler::memory_used () const
{
	/* XXX: should also look at _audio.memory_used() */
	return _video.memory_used();
}

void
Butler::player_change (ChangeType type, bool frequent)
{
	boost::mutex::scoped_lock lm (_mutex);

	if (type == CHANGE_TYPE_PENDING) {
		++_suspended;
	} else if (type == CHANGE_TYPE_DONE) {
		--_suspended;
		if (_died || _pending_seek_position || frequent) {
			lm.unlock ();
			_summon.notify_all ();
			return;
		}

		DCPTime seek_to;
		DCPTime next = _video.get().second;
		if (_awaiting && _awaiting > next) {
			/* We have recently done a player_changed seek and our buffers haven't been refilled yet,
			   so assume that we're seeking to the same place as last time.
			*/
			seek_to = *_awaiting;
		} else {
			seek_to = next;
		}

		seek_unlocked (seek_to, true);
		_awaiting = seek_to;
	} else if (type == CHANGE_TYPE_CANCELLED) {
		--_suspended;
	}

	lm.unlock ();
	_summon.notify_all ();
}

void
Butler::text (PlayerText pt, TextType type, optional<DCPTextTrack> track, DCPTimePeriod period)
{
	if (type != TEXT_CLOSED_CAPTION) {
		return;
	}

	DCPOMATIC_ASSERT (track);

	boost::mutex::scoped_lock lm2 (_buffers_mutex);
	_closed_caption.put (pt, *track, period);
}
