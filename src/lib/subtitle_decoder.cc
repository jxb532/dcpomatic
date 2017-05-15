/*
    Copyright (C) 2013-2017 Carl Hetherington <cth@carlh.net>

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

#include "subtitle_decoder.h"
#include "subtitle_content.h"
#include "util.h"
#include "log.h"
#include "compose.hpp"
#include <sub/subtitle.h>
#include <boost/shared_ptr.hpp>
#include <boost/foreach.hpp>
#include <boost/algorithm/string.hpp>
#include <iostream>

using std::list;
using std::cout;
using std::string;
using std::min;
using boost::shared_ptr;
using boost::optional;
using boost::function;

SubtitleDecoder::SubtitleDecoder (
	Decoder* parent,
	shared_ptr<const SubtitleContent> c,
	shared_ptr<Log> log
	)
	: DecoderPart (parent, log)
	, _content (c)
{

}

/** Called by subclasses when an image subtitle is starting.
 *  @param from From time of the subtitle.
 *  @param image Subtitle image.
 *  @param rect Area expressed as a fraction of the video frame that this subtitle
 *  is for (e.g. a width of 0.5 means the width of the subtitle is half the width
 *  of the video frame)
 */
void
SubtitleDecoder::emit_image_start (ContentTime from, shared_ptr<Image> image, dcpomatic::Rect<double> rect)
{
	ImageStart (ContentImageSubtitle (from, image, rect));
}

void
SubtitleDecoder::emit_text_start (ContentTime from, list<dcp::SubtitleString> s)
{
	/* We must escape < and > in strings, otherwise they might confuse our subtitle
	   renderer (which uses some HTML-esque markup to do bold/italic etc.)
	*/
	BOOST_FOREACH (dcp::SubtitleString& i, s) {
		string t = i.text ();
		boost::algorithm::replace_all (t, "<", "&lt;");
		boost::algorithm::replace_all (t, ">", "&gt;");
		i.set_text (t);
	}

	TextStart (ContentTextSubtitle (from, s));
	_position = from;
}

void
SubtitleDecoder::emit_text_start (ContentTime from, sub::Subtitle const & subtitle)
{
	/* See if our next subtitle needs to be placed on screen by us */
	bool needs_placement = false;
	optional<int> bottom_line;
	BOOST_FOREACH (sub::Line i, subtitle.lines) {
		if (!i.vertical_position.reference || i.vertical_position.reference.get() == sub::TOP_OF_SUBTITLE) {
			needs_placement = true;
			DCPOMATIC_ASSERT (i.vertical_position.line);
			if (!bottom_line || bottom_line.get() < i.vertical_position.line.get()) {
				bottom_line = i.vertical_position.line.get();
			}
		}
	}

	/* Find the lowest proportional position */
	optional<float> lowest_proportional;
	BOOST_FOREACH (sub::Line i, subtitle.lines) {
		if (i.vertical_position.proportional) {
			if (!lowest_proportional) {
				lowest_proportional = i.vertical_position.proportional;
			} else {
				lowest_proportional = min (lowest_proportional.get(), i.vertical_position.proportional.get());
			}
		}
	}

	list<dcp::SubtitleString> out;
	BOOST_FOREACH (sub::Line i, subtitle.lines) {
		BOOST_FOREACH (sub::Block j, i.blocks) {

			if (!j.font_size.specified()) {
				/* Fallback default font size if no other has been specified */
				j.font_size.set_points (48);
			}

			float v_position;
			dcp::VAlign v_align;
			if (needs_placement) {
				DCPOMATIC_ASSERT (i.vertical_position.line);
				/* This 1.015 is an arbitrary value to lift the bottom sub off the bottom
				   of the screen a bit to a pleasing degree.
				*/
				v_position = 1.015 -
					(1 + bottom_line.get() - i.vertical_position.line.get())
					* 1.2 * content()->line_spacing() * content()->y_scale() * j.font_size.proportional (72 * 11);

				v_align = dcp::VALIGN_TOP;
			} else {
				DCPOMATIC_ASSERT (i.vertical_position.proportional);
				DCPOMATIC_ASSERT (i.vertical_position.reference);
				v_position = i.vertical_position.proportional.get();

				if (lowest_proportional) {
					/* Adjust line spacing */
					v_position = ((v_position - lowest_proportional.get()) * content()->line_spacing()) + lowest_proportional.get();
				}

				switch (i.vertical_position.reference.get()) {
				case sub::TOP_OF_SCREEN:
					v_align = dcp::VALIGN_TOP;
					break;
				case sub::CENTRE_OF_SCREEN:
					v_align = dcp::VALIGN_CENTER;
					break;
				case sub::BOTTOM_OF_SCREEN:
					v_align = dcp::VALIGN_BOTTOM;
					break;
				default:
					v_align = dcp::VALIGN_TOP;
					break;
				}
			}

			dcp::Effect effect = dcp::NONE;
			if (content()->outline()) {
				effect = dcp::BORDER;
			} else if (content()->shadow()) {
				effect = dcp::SHADOW;
			}

			out.push_back (
				dcp::SubtitleString (
					string(TEXT_FONT_ID),
					j.italic,
					j.bold,
					j.underline,
					/* force the colour to whatever is configured */
					content()->colour(),
					j.font_size.points (72 * 11),
					1.0,
					dcp::Time (from.seconds(), 1000),
					/* XXX: hmm; this is a bit ugly (we don't know the to time yet) */
					dcp::Time (),
					0,
					dcp::HALIGN_CENTER,
					v_position,
					v_align,
					dcp::DIRECTION_LTR,
					j.text,
					effect,
					content()->effect_colour(),
					dcp::Time (content()->fade_in().seconds(), 1000),
					dcp::Time (content()->fade_out().seconds(), 1000)
					)
				);
		}
	}

	emit_text_start (from, out);
}

void
SubtitleDecoder::emit_stop (ContentTime to)
{
	Stop (to);
}

void
SubtitleDecoder::emit_text (ContentTimePeriod period, list<dcp::SubtitleString> s)
{
	emit_text_start (period.from, s);
	emit_stop (period.to);
}

void
SubtitleDecoder::emit_text (ContentTimePeriod period, sub::Subtitle const & s)
{
	emit_text_start (period.from, s);
	emit_stop (period.to);
}

void
SubtitleDecoder::seek ()
{
	_position = ContentTime ();
}
