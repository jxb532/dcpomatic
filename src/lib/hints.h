/*
    Copyright (C) 2016-2019 Carl Hetherington <cth@carlh.net>

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

#include "signaller.h"
#include "player_text.h"
#include "types.h"
#include "dcp_text_track.h"
#include "dcpomatic_time.h"
#include <boost/weak_ptr.hpp>
#include <boost/signals2.hpp>
#include <vector>
#include <string>

class Film;

class Hints : public Signaller
{
public:
	explicit Hints (boost::weak_ptr<const Film> film);
	~Hints ();

	void start ();

	boost::signals2::signal<void (std::string)> Hint;
	boost::signals2::signal<void (std::string)> Progress;
	boost::signals2::signal<void (void)> Pulse;
	boost::signals2::signal<void (void)> Finished;

private:
	void thread ();
	void hint (std::string h);
	void text (PlayerText text, TextType type, DCPTimePeriod period);

	boost::weak_ptr<const Film> _film;
	boost::thread* _thread;

	bool _long_ccap;
	bool _overlap_ccap;
	bool _too_many_ccap_lines;
	boost::optional<DCPTimePeriod> _last;

	boost::mutex _mutex;
	bool _stop;
};
