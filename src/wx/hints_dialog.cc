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

#include "lib/film.h"
#include "lib/ratio.h"
#include "lib/video_content.h"
#include "lib/subtitle_content.h"
#include "lib/font.h"
#include "hints_dialog.h"
#include <wx/richtext/richtextctrl.h>
#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>

using boost::shared_ptr;
using boost::optional;
using boost::dynamic_pointer_cast;

HintsDialog::HintsDialog (wxWindow* parent, boost::weak_ptr<Film> film)
	: wxDialog (parent, wxID_ANY, _("Hints"))
	, _film (film)
{
	wxBoxSizer* sizer = new wxBoxSizer (wxVERTICAL);
	_text = new wxRichTextCtrl (this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize (400, 300), wxRE_READONLY);
	sizer->Add (_text, 1, wxEXPAND | wxALL, 6);

	wxSizer* buttons = CreateSeparatedButtonSizer (wxOK);
	if (buttons) {
		sizer->Add (buttons, wxSizerFlags().Expand().DoubleBorder());
	}

	SetSizer (sizer);
	sizer->Layout ();
	sizer->SetSizeHints (this);

	_text->GetCaret()->Hide ();

	boost::shared_ptr<Film> locked_film = _film.lock ();
	if (locked_film) {
		_film_changed_connection = locked_film->Changed.connect (boost::bind (&HintsDialog::film_changed, this));
		_film_content_changed_connection = locked_film->ContentChanged.connect (boost::bind (&HintsDialog::film_changed, this));
	}

	film_changed ();
}

void
HintsDialog::film_changed ()
{
	_text->Clear ();
	bool hint = false;

	boost::shared_ptr<Film> film = _film.lock ();
	if (!film) {
		return;
	}

	ContentList content = film->content ();

	_text->BeginStandardBullet (N_("standard/circle"), 1, 50);

	bool big_font_files = false;
	if (film->interop ()) {
		BOOST_FOREACH (shared_ptr<Content> i, content) {
			shared_ptr<SubtitleContent> s = dynamic_pointer_cast<SubtitleContent> (i);
			if (s) {
				BOOST_FOREACH (shared_ptr<Font> j, s->fonts ()) {
					for (int i = 0; i < FontFiles::VARIANTS; ++i) {
						optional<boost::filesystem::path> const p = j->file (static_cast<FontFiles::Variant> (i));
						if (p && boost::filesystem::file_size (p.get()) >= (640 * 1024)) {
							big_font_files = true;
						}
					}
				}
			}
		}
	}

	if (big_font_files) {
		hint = true;
		_text->WriteText (_("You have specified a font file which is larger than 640kB.  This is very likely to cause problems on playback."));
	}

	if (film->audio_channels() % 2) {
		hint = true;
		_text->WriteText (_("Your DCP has an odd number of audio channels.  This is very likely to cause problems on playback."));
		_text->Newline ();
	} else if (film->audio_channels() < 6) {
		hint = true;
		_text->WriteText (_("Your DCP has fewer than 6 audio channels.  This may cause problems on some projectors."));
		_text->Newline ();
	} else if (film->audio_channels() == 0) {
		/* Carsten Kurz reckons having no audio can be a problem */
		hint = true;
		_text->WriteText (_("Your DCP has no audio channels.  This is likely to cause problems on playback."));
		_text->Newline ();
	}

	int flat_or_narrower = 0;
	int scope = 0;
	BOOST_FOREACH (shared_ptr<const Content> i, content) {
		shared_ptr<const VideoContent> vc = dynamic_pointer_cast<const VideoContent> (i);
		if (vc) {
			Ratio const * r = vc->scale().ratio ();
			if (r && r->id() == "239") {
				++scope;
			} else if (r && r->id() != "239" && r->id() != "full-frame") {
				++flat_or_narrower;
			}
		}
	}

	if (scope && !flat_or_narrower && film->container()->id() == "185") {
		hint = true;
		_text->WriteText (_("All of your content is in Scope (2.39:1) but your DCP's container is Flat (1.85:1).  This will letter-box your content inside a Flat (1.85:1) frame.  You may prefer to set your DCP's container to Scope (2.39:1) in the \"DCP\" tab."));
		_text->Newline ();
	}

	if (!scope && flat_or_narrower && film->container()->id() == "239") {
		hint = true;
		_text->WriteText (_("All of your content is at 1.85:1 or narrower but your DCP's container is Scope (2.39:1).  This will pillar-box your content inside a Flat (1.85:1) frame.  You may prefer to set your DCP's container to Flat (1.85:1) in the \"DCP\" tab."));
		_text->Newline ();
	}

	if (film->video_frame_rate() != 24 && film->video_frame_rate() != 48) {
		hint = true;
		_text->WriteText (wxString::Format (_("Your DCP frame rate (%d fps) may cause problems in a few (mostly older) projectors.  Use 24 or 48 frames per second to be on the safe side."), film->video_frame_rate()));
		_text->Newline ();
	}

	if (film->j2k_bandwidth() >= 245000000) {
		hint = true;
		_text->WriteText (_("A few projectors have problems playing back very high bit-rate DCPs.  It is a good idea to drop the JPEG2000 bandwidth down to about 200Mbit/s; this is unlikely to have any visible effect on the image."));
		_text->Newline ();
	}

	if (film->interop() && film->video_frame_rate() != 24 && film->video_frame_rate() != 48) {
		hint = true;
		_text->WriteText (_("You are set up for an Interop DCP at a frame rate which is not officially supported.  You are advised to make a SMPTE DCP instead."));
		_text->Newline ();
	}

	int vob = 0;
	BOOST_FOREACH (shared_ptr<const Content> i, content) {
		if (boost::algorithm::starts_with (i->path(0).filename().string(), "VTS_")) {
			++vob;
		}
	}

	if (vob > 1) {
		hint = true;
		_text->WriteText (wxString::Format (_("You have %d files that look like they are VOB files from DVD. You should join them to ensure smooth joins between the files."), vob));
		_text->Newline ();
	}

	int three_d = 0;
	BOOST_FOREACH (shared_ptr<const Content> i, content) {
		shared_ptr<const VideoContent> vc = dynamic_pointer_cast<const VideoContent> (i);
		if (vc && vc->video_frame_type() != VIDEO_FRAME_TYPE_2D) {
			++three_d;
		}
	}

	if (three_d > 0 && !film->three_d()) {
		hint = true;
		_text->WriteText (_("You are using 3D content but your DCP is set to 2D.  Set the DCP to 3D if you want to play it back on a 3D system (e.g. Real-D, MasterImage etc.)"));
		_text->Newline ();
	}

	_text->EndSymbolBullet ();

	if (!hint) {
		_text->WriteText (_("There are no hints: everything looks good!"));
	}
}
