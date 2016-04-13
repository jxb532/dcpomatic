/*
    Copyright (C) 2013-2016 Carl Hetherington <cth@carlh.net>

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

#include "timeline_content_view.h"

class SubtitleContent;

/** @class TimelineSubtitleContentView
 *  @brief Timeline view for SubtitleContent.
 */
class TimelineSubtitleContentView : public TimelineContentView
{
public:
	TimelineSubtitleContentView (Timeline& tl, boost::shared_ptr<Content> c);

private:
	bool active () const;
	wxColour background_colour () const;
	wxColour foreground_colour () const;
};
