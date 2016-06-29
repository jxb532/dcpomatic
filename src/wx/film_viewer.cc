/*
    Copyright (C) 2012-2016 Carl Hetherington <cth@carlh.net>

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

/** @file  src/film_viewer.cc
 *  @brief A wx widget to view a preview of a Film.
 */

#include "lib/film.h"
#include "lib/ratio.h"
#include "lib/util.h"
#include "lib/job_manager.h"
#include "lib/image.h"
#include "lib/exceptions.h"
#include "lib/examine_content_job.h"
#include "lib/filter.h"
#include "lib/player.h"
#include "lib/player_video.h"
#include "lib/video_content.h"
#include "lib/video_decoder.h"
#include "lib/timer.h"
#include "lib/log.h"
#include "film_viewer.h"
#include "wx_util.h"
extern "C" {
#include <libavutil/pixfmt.h>
}
#include <dcp/exceptions.h>
#include <wx/tglbtn.h>
#include <iostream>
#include <iomanip>

using std::string;
using std::pair;
using std::min;
using std::max;
using std::cout;
using std::list;
using std::bad_alloc;
using std::make_pair;
using std::exception;
using boost::shared_ptr;
using boost::dynamic_pointer_cast;
using boost::weak_ptr;
using boost::optional;
using dcp::Size;

FilmViewer::FilmViewer (wxWindow* p)
	: wxPanel (p)
	, _panel (new wxPanel (this))
	, _outline_content (new wxCheckBox (this, wxID_ANY, _("Outline content")))
	, _left_eye (new wxRadioButton (this, wxID_ANY, _("Left eye"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP))
	, _right_eye (new wxRadioButton (this, wxID_ANY, _("Right eye")))
	, _slider (new wxSlider (this, wxID_ANY, 0, 0, 4096))
	, _back_button (new wxButton (this, wxID_ANY, wxT("<")))
	, _forward_button (new wxButton (this, wxID_ANY, wxT(">")))
	, _frame_number (new wxStaticText (this, wxID_ANY, wxT("")))
	, _timecode (new wxStaticText (this, wxID_ANY, wxT("")))
	, _play_button (new wxToggleButton (this, wxID_ANY, _("Play")))
	, _coalesce_player_changes (false)
	, _pending_player_change (false)
	, _last_get_accurate (true)
{
#ifndef __WXOSX__
	_panel->SetDoubleBuffered (true);
#endif

	_panel->SetBackgroundStyle (wxBG_STYLE_PAINT);

	_v_sizer = new wxBoxSizer (wxVERTICAL);
	SetSizer (_v_sizer);

	_v_sizer->Add (_panel, 1, wxEXPAND);

	wxBoxSizer* view_options = new wxBoxSizer (wxHORIZONTAL);
	view_options->Add (_outline_content, 0, wxRIGHT, DCPOMATIC_SIZER_GAP);
	view_options->Add (_left_eye, 0, wxLEFT | wxRIGHT, DCPOMATIC_SIZER_GAP);
	view_options->Add (_right_eye, 0, wxLEFT | wxRIGHT, DCPOMATIC_SIZER_GAP);
	_v_sizer->Add (view_options, 0, wxALL, DCPOMATIC_SIZER_GAP);

	wxBoxSizer* h_sizer = new wxBoxSizer (wxHORIZONTAL);

	wxBoxSizer* time_sizer = new wxBoxSizer (wxVERTICAL);
	time_sizer->Add (_frame_number, 0, wxEXPAND);
	time_sizer->Add (_timecode, 0, wxEXPAND);

	h_sizer->Add (_back_button, 0, wxALL, 2);
	h_sizer->Add (time_sizer, 0, wxEXPAND);
	h_sizer->Add (_forward_button, 0, wxALL, 2);
	h_sizer->Add (_play_button, 0, wxEXPAND);
	h_sizer->Add (_slider, 1, wxEXPAND);

	_v_sizer->Add (h_sizer, 0, wxEXPAND | wxALL, 6);

	_frame_number->SetMinSize (wxSize (84, -1));
	_back_button->SetMinSize (wxSize (32, -1));
	_forward_button->SetMinSize (wxSize (32, -1));

	_panel->Bind          (wxEVT_PAINT,                        boost::bind (&FilmViewer::paint_panel,     this));
	_panel->Bind          (wxEVT_SIZE,                         boost::bind (&FilmViewer::panel_sized,     this, _1));
	_outline_content->Bind(wxEVT_COMMAND_CHECKBOX_CLICKED,     boost::bind (&FilmViewer::refresh_panel,   this));
	_left_eye->Bind       (wxEVT_COMMAND_RADIOBUTTON_SELECTED, boost::bind (&FilmViewer::refresh,         this));
	_right_eye->Bind      (wxEVT_COMMAND_RADIOBUTTON_SELECTED, boost::bind (&FilmViewer::refresh,         this));
	_slider->Bind         (wxEVT_SCROLL_THUMBTRACK,            boost::bind (&FilmViewer::slider_moved,    this));
	_slider->Bind         (wxEVT_SCROLL_PAGEUP,                boost::bind (&FilmViewer::slider_moved,    this));
	_slider->Bind         (wxEVT_SCROLL_PAGEDOWN,              boost::bind (&FilmViewer::slider_moved,    this));
	_play_button->Bind    (wxEVT_COMMAND_TOGGLEBUTTON_CLICKED, boost::bind (&FilmViewer::play_clicked,    this));
	_timer.Bind           (wxEVT_TIMER,                        boost::bind (&FilmViewer::timer,           this));
	_back_button->Bind    (wxEVT_COMMAND_BUTTON_CLICKED,       boost::bind (&FilmViewer::back_clicked,    this));
	_forward_button->Bind (wxEVT_COMMAND_BUTTON_CLICKED,       boost::bind (&FilmViewer::forward_clicked, this));

	set_film (shared_ptr<Film> ());

	JobManager::instance()->ActiveJobsChanged.connect (
		bind (&FilmViewer::active_jobs_changed, this, _2)
		);

	setup_sensitivity ();
}

void
FilmViewer::set_film (shared_ptr<Film> film)
{
	if (_film == film) {
		return;
	}

	_film = film;

	_frame.reset ();

	update_position_slider ();
	update_position_label ();

	if (!_film) {
		return;
	}

	try {
		_player.reset (new Player (_film, _film->playlist ()));
		_player->set_fast ();
	} catch (bad_alloc) {
		error_dialog (this, _("There is not enough free memory to do that."));
		_film.reset ();
		return;
	}

	/* Always burn in subtitles, even if content is set not to, otherwise we won't see them
	   in the preview.
	*/
	_player->set_always_burn_subtitles (true);
	_player->set_ignore_audio ();
	_player->set_play_referenced ();

	_film_connection = _film->Changed.connect (boost::bind (&FilmViewer::film_changed, this, _1));

	_player_connection = _player->Changed.connect (boost::bind (&FilmViewer::player_changed, this, _1));

	calculate_sizes ();
	refresh ();

	setup_sensitivity ();
}

void
FilmViewer::refresh_panel ()
{
	_panel->Refresh ();
	_panel->Update ();
}

void
FilmViewer::get (DCPTime p, bool accurate)
{
	if (!_player) {
		return;
	}

	list<shared_ptr<PlayerVideo> > all_pv;
	try {
		all_pv = _player->get_video (p, accurate);
	} catch (exception& e) {
		error_dialog (this, wxString::Format (_("Could not get video for view (%s)"), std_to_wx(e.what()).data()));
	}

	if (!all_pv.empty ()) {
		try {
			shared_ptr<PlayerVideo> pv;
			if (all_pv.size() == 2) {
				/* We have 3D; choose the correct eye */
				if (_left_eye->GetValue()) {
					if (all_pv.front()->eyes() == EYES_LEFT) {
						pv = all_pv.front();
					} else {
						pv = all_pv.back();
					}
				} else {
					if (all_pv.front()->eyes() == EYES_RIGHT) {
						pv = all_pv.front();
					} else {
						pv = all_pv.back();
					}
				}
			} else {
				/* 2D; no choice to make */
				pv = all_pv.front ();
			}

			/* In an ideal world, what we would do here is:
			 *
			 * 1. convert to XYZ exactly as we do in the DCP creation path.
			 * 2. convert back to RGB for the preview display, compensating
			 *    for the monitor etc. etc.
			 *
			 * but this is inefficient if the source is RGB.  Since we don't
			 * (currently) care too much about the precise accuracy of the preview's
			 * colour mapping (and we care more about its speed) we try to short-
			 * circuit this "ideal" situation in some cases.
			 *
			 * The content's specified colour conversion indicates the colourspace
			 * which the content is in (according to the user).
			 *
			 * PlayerVideo::image (bound to PlayerVideo::always_rgb) will take the source
			 * image and convert it (from whatever the user has said it is) to RGB.
			 */

			_frame = pv->image (
				bind (&Log::dcp_log, _film->log().get(), _1, _2),
				bind (&PlayerVideo::always_rgb, _1),
				false, true
				);

			ImageChanged (pv);

			_position = pv->time ();
			_inter_position = pv->inter_position ();
			_inter_size = pv->inter_size ();
		} catch (dcp::DCPReadError& e) {
			/* This can happen on the following sequence of events:
			 * - load encrypted DCP
			 * - add KDM
			 * - DCP is examined again, which sets its "playable" flag to 1
			 * - as a side effect of the exam, the viewer is updated using the old pieces
			 * - the DCPDecoder in the old piece gives us an encrypted frame
			 * - then, the pieces are re-made (but too late).
			 *
			 * I hope there's a better way to handle this ...
			 */
			_frame.reset ();
			_position = p;
		}
	} else {
		_frame.reset ();
		_position = p;
	}

	refresh_panel ();

	_last_get_accurate = accurate;
}

void
FilmViewer::timer ()
{
	DCPTime const frame = DCPTime::from_frames (1, _film->video_frame_rate ());

	if ((_position + frame) >= _film->length ()) {
		_play_button->SetValue (false);
		check_play_state ();
	} else {
		get (_position + frame, true);
	}

	update_position_label ();
	update_position_slider ();
}

void
FilmViewer::paint_panel ()
{
	wxPaintDC dc (_panel);

	if (!_frame || !_film || !_out_size.width || !_out_size.height) {
		dc.Clear ();
		return;
	}

	wxImage frame (_out_size.width, _out_size.height, _frame->data()[0], true);
	wxBitmap frame_bitmap (frame);
	dc.DrawBitmap (frame_bitmap, 0, 0);

	if (_out_size.width < _panel_size.width) {
		wxPen p (GetBackgroundColour ());
		wxBrush b (GetBackgroundColour ());
		dc.SetPen (p);
		dc.SetBrush (b);
		dc.DrawRectangle (_out_size.width, 0, _panel_size.width - _out_size.width, _panel_size.height);
	}

	if (_out_size.height < _panel_size.height) {
		wxPen p (GetBackgroundColour ());
		wxBrush b (GetBackgroundColour ());
		dc.SetPen (p);
		dc.SetBrush (b);
		dc.DrawRectangle (0, _out_size.height, _panel_size.width, _panel_size.height - _out_size.height);
	}

	if (_outline_content->GetValue ()) {
		wxPen p (wxColour (255, 0, 0), 2);
		dc.SetPen (p);
		dc.SetBrush (*wxTRANSPARENT_BRUSH);
		dc.DrawRectangle (_inter_position.x, _inter_position.y, _inter_size.width, _inter_size.height);
	}
}

void
FilmViewer::slider_moved ()
{
	if (!_film) {
		return;
	}

	DCPTime t (_slider->GetValue() * _film->length().get() / 4096);
	/* Ensure that we hit the end of the film at the end of the slider */
	if (t >= _film->length ()) {
		t = _film->length() - DCPTime::from_frames (1, _film->video_frame_rate ());
	}
	get (t, false);
	update_position_label ();
}

void
FilmViewer::panel_sized (wxSizeEvent& ev)
{
	_panel_size.width = ev.GetSize().GetWidth();
	_panel_size.height = ev.GetSize().GetHeight();

	calculate_sizes ();
	refresh ();
	update_position_label ();
	update_position_slider ();
}

void
FilmViewer::calculate_sizes ()
{
	if (!_film || !_player) {
		return;
	}

	Ratio const * container = _film->container ();

	float const panel_ratio = _panel_size.ratio ();
	float const film_ratio = container ? container->ratio () : 1.78;

	if (panel_ratio < film_ratio) {
		/* panel is less widscreen than the film; clamp width */
		_out_size.width = _panel_size.width;
		_out_size.height = lrintf (_out_size.width / film_ratio);
	} else {
		/* panel is more widescreen than the film; clamp height */
		_out_size.height = _panel_size.height;
		_out_size.width = lrintf (_out_size.height * film_ratio);
	}

	/* Catch silly values */
	_out_size.width = max (64, _out_size.width);
	_out_size.height = max (64, _out_size.height);

	_player->set_video_container_size (_out_size);
}

void
FilmViewer::play_clicked ()
{
	check_play_state ();
}

void
FilmViewer::check_play_state ()
{
	if (!_film || _film->video_frame_rate() == 0) {
		return;
	}

	if (_play_button->GetValue()) {
		_timer.Start (1000 / _film->video_frame_rate());
	} else {
		_timer.Stop ();
	}
}

void
FilmViewer::update_position_slider ()
{
	if (!_film) {
		_slider->SetValue (0);
		return;
	}

	DCPTime const len = _film->length ();

	if (len.get ()) {
		int const new_slider_position = 4096 * _position.get() / len.get();
		if (new_slider_position != _slider->GetValue()) {
			_slider->SetValue (new_slider_position);
		}
	}
}

void
FilmViewer::update_position_label ()
{
	if (!_film) {
		_frame_number->SetLabel ("0");
		_timecode->SetLabel ("0:0:0.0");
		return;
	}

	double const fps = _film->video_frame_rate ();
	/* Count frame number from 1 ... not sure if this is the best idea */
	_frame_number->SetLabel (wxString::Format (wxT("%ld"), lrint (_position.seconds() * fps) + 1));
	_timecode->SetLabel (time_to_timecode (_position, fps));
}

void
FilmViewer::active_jobs_changed (optional<string> j)
{
	/* examine content is the only job which stops the viewer working */
	bool const a = !j || *j != "examine_content";
	_slider->Enable (a);
	_play_button->Enable (a);
}

void
FilmViewer::back_clicked ()
{
	DCPTime p = _position - DCPTime::from_frames (1, _film->video_frame_rate ());
	if (p < DCPTime ()) {
		p = DCPTime ();
	}

	get (p, true);
	update_position_label ();
	update_position_slider ();
}

void
FilmViewer::forward_clicked ()
{
	DCPTime p = _position + DCPTime::from_frames (1, _film->video_frame_rate ());
	if (p >= _film->length ()) {
		p = _position;
	}

	get (p, true);
	update_position_label ();
	update_position_slider ();
}

void
FilmViewer::player_changed (bool frequent)
{
	if (frequent) {
		return;
	}

	if (_coalesce_player_changes) {
		_pending_player_change = true;
		return;
	}

	calculate_sizes ();
	refresh ();
	update_position_label ();
	update_position_slider ();
}

void
FilmViewer::setup_sensitivity ()
{
	bool const c = _film && !_film->content().empty ();

	_slider->Enable (c);
	_back_button->Enable (c);
	_forward_button->Enable (c);
	_play_button->Enable (c);
	_outline_content->Enable (c);
	_frame_number->Enable (c);
	_timecode->Enable (c);

	_left_eye->Enable (c && _film->three_d ());
	_right_eye->Enable (c && _film->three_d ());
}

void
FilmViewer::film_changed (Film::Property p)
{
	if (p == Film::CONTENT || p == Film::THREE_D) {
		setup_sensitivity ();
	}
}

/** Re-get the current frame */
void
FilmViewer::refresh ()
{
	get (_position, _last_get_accurate);
}

void
FilmViewer::set_position (DCPTime p)
{
	_position = p;
	get (_position, true);
	update_position_label ();
	update_position_slider ();
}

void
FilmViewer::set_coalesce_player_changes (bool c)
{
	_coalesce_player_changes = c;

	if (c) {
		_pending_player_change = false;
	} else {
		if (_pending_player_change) {
			player_changed (false);
		}
	}
}
