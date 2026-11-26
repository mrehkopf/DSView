/*
 * This file is part of the PulseView project.
 * DSView is based on PulseView.
 * 
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2014 DreamSourceLab <support@dreamsourcelab.com>
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <libsigrokdecode.h>
#include "../dsvdef.h" 
#include <boost/functional/hash.hpp>
#include <algorithm>
#include <vector>
#include <QAction>
#include <QStringList>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QScrollArea>
#include <QApplication>
#include "decodetrace.h"
#include "../sigsession.h"
#include "../data/decoderstack.h"
#include "../data/decode/decoder.h"
#include "../data/logicsnapshot.h"
#include "../data/decode/annotation.h"
#include "../data/decode/rowdata.h"
#include "../view/logicsignal.h"
#include "../view/view.h"
#include "../widgets/decodergroupbox.h"
#include "../widgets/decodermenu.h"
#include "../view/cursor.h"
#include "../toolbars/titlebar.h"
#include "../dsvdef.h"
#include "../ui/dscombobox.h"
#include "../ui/msgbox.h"
#include "../appcontrol.h"
#include "../dialogs/decoderoptionsdlg.h"
#include "../ui/langresource.h"
#include "../config/appconfig.h"
#include "../log.h"

using namespace boost;
using namespace std;
 
namespace pv {
namespace view {

const QColor DecodeTrace::DecodeColours[4] = {
	QColor(0xEF, 0x29, 0x29),	// Red
	QColor(0xFC, 0xE9, 0x4F),	// Yellow
	QColor(0x8A, 0xE2, 0x34),	// Green
	QColor(0x72, 0x9F, 0xCF)	// Blue
};

const QColor DecodeTrace::ErrorBgColour = QColor(0xEF, 0x29, 0x29);
const QColor DecodeTrace::NoDecodeColour = QColor(0x88, 0x8A, 0x85);

const int DecodeTrace::ArrowSize = 4;
const double DecodeTrace::EndCapWidth = 5;
const int DecodeTrace::DrawPadding = 100;

const QColor DecodeTrace::Colours[16] = {
	QColor(0xEF, 0x29, 0x29),
	QColor(0xF6, 0x6A, 0x32),
	QColor(0xFC, 0xAE, 0x3E),
	QColor(0xFB, 0xCA, 0x47),
	QColor(0xFC, 0xE9, 0x4F),
	QColor(0xCD, 0xF0, 0x40),
	QColor(0x8A, 0xE2, 0x34),
	QColor(0x4E, 0xDC, 0x44),
	QColor(0x55, 0xD7, 0x95),
	QColor(0x64, 0xD1, 0xD2),
	QColor(0x72, 0x9F, 0xCF),
	QColor(0xD4, 0x76, 0xC4),
	QColor(0x9D, 0x79, 0xB9),
	QColor(0xAD, 0x7F, 0xA8),
	QColor(0xC2, 0x62, 0x9B),
	QColor(0xD7, 0x47, 0x6F)
};

const QColor DecodeTrace::OutlineColours[16] = {
	QColor(0x77, 0x14, 0x14),
	QColor(0x7B, 0x35, 0x19),
	QColor(0x7E, 0x57, 0x1F),
	QColor(0x7D, 0x65, 0x23),
	QColor(0x7E, 0x74, 0x27),
	QColor(0x66, 0x78, 0x20),
	QColor(0x45, 0x71, 0x1A),
	QColor(0x27, 0x6E, 0x22),
	QColor(0x2A, 0x6B, 0x4A),
	QColor(0x32, 0x68, 0x69),
	QColor(0x39, 0x4F, 0x67),
	QColor(0x6A, 0x3B, 0x62),
	QColor(0x4E, 0x3C, 0x5C),
	QColor(0x56, 0x3F, 0x54),
	QColor(0x61, 0x31, 0x4D),
	QColor(0x6B, 0x23, 0x37)
};

// Official Catppuccin Frappe accent colours
const QColor DecodeTrace::CatppuccinFrappeColours[14] = {
	QColor(0xe7, 0x82, 0x84), // Red
	QColor(0xea, 0x99, 0x9c), // Maroon
	QColor(0xef, 0x9f, 0x76), // Peach
	QColor(0xe5, 0xc8, 0x90), // Yellow
	QColor(0xa6, 0xd1, 0x89), // Green
	QColor(0x81, 0xc8, 0xbe), // Teal
	QColor(0x99, 0xd1, 0xdb), // Sky
	QColor(0x85, 0xc1, 0xdc), // Sapphire
	QColor(0x8c, 0xaa, 0xee), // Blue
	QColor(0xba, 0xbb, 0xf1), // Lavender
	QColor(0xca, 0x9e, 0xe6), // Mauve
	QColor(0xf4, 0xb8, 0xe4), // Pink
	QColor(0xee, 0xbe, 0xbe), // Flamingo
	QColor(0xf2, 0xd5, 0xcf)  // Rosewater
};

// Official Catppuccin Latte accent colours
const QColor DecodeTrace::CatppuccinLatteColours[14] = {
	QColor(0xd2, 0x0f, 0x39), // Red
	QColor(0xe6, 0x45, 0x53), // Maroon
	QColor(0xfe, 0x64, 0x0b), // Peach
	QColor(0xdf, 0x8e, 0x1d), // Yellow
	QColor(0x40, 0xa0, 0x2b), // Green
	QColor(0x17, 0x92, 0x99), // Teal
	QColor(0x04, 0xa5, 0xe5), // Sky
	QColor(0x20, 0x9f, 0xb5), // Sapphire
	QColor(0x1e, 0x66, 0xf5), // Blue
	QColor(0x72, 0x87, 0xfd), // Lavender
	QColor(0x88, 0x39, 0xef), // Mauve
	QColor(0xea, 0x76, 0xcb), // Pink
	QColor(0xdd, 0x78, 0x78), // Flamingo
	QColor(0xdc, 0x8a, 0x78)  // Rosewater
};

QColor DecodeTrace::get_row_base_colour(int local_row)
{
    const QString &style = AppConfig::Instance().frameOptions.style;

    if (style == THEME_STYLE_FRAPPE)
        return CatppuccinFrappeColours[local_row % countof(CatppuccinFrappeColours)];
    else if (style == THEME_STYLE_LATTE)
        return CatppuccinLatteColours[local_row % countof(CatppuccinLatteColours)];

    return Colours[local_row % countof(Colours)];
}


DecodeTrace::DecodeTrace(pv::SigSession *session,
	pv::data::DecoderStack *decoder_stack, int index) :
	Trace(QString::fromUtf8(decoder_stack->stack().front()->decoder()->name), index, SR_CHANNEL_DECODER)
{
    assert(decoder_stack);

    _colour = QColor("default");
 
    _decode_start = 0;
    _decode_end  = INT64_MAX; 
    _decoder_stack = decoder_stack;
    _session = session;
    _delete_flag = false;
    _decode_cursor1 = 0;
    _decode_cursor2 = 0;
    _channels_left = 0;
    _header_rows = 0;

    connect(_decoder_stack, SIGNAL(new_decode_data()), this, SLOT(on_new_decode_data()));

    connect(_decoder_stack, SIGNAL(decode_done()), this, SLOT(on_decode_done()));
}

DecodeTrace::~DecodeTrace()
{   
    DESTROY_OBJECT(_decoder_stack);
}


void DecodeTrace::generate_annotation_colours(QColor baseColour, int local_row,
    const pv::data::decode::Annotation &a, QColor *res_fill, QColor *res_outline) {
    *res_fill = baseColour; 

    /* vary the color based on the trace color setting.
       for single channel decoders, keep the color for the first row close
       to the user setting. */
    int ch, cs, cl;
    int lightness_offset;
    int hue_offset;

    hue_offset = static_cast<int>(15.0 * (local_row ? 4 : 1) * ((a.type() % 3) - 1));
    lightness_offset = 10 * (local_row % 6) * ((a.type() % 3) - 1);

    res_fill->getHsl(&ch, &cs, &cl);
    ch = (360 + ((ch + hue_offset) % 360)) % 360;
    cl = max(0, min(255, (cl + lightness_offset)));
    res_fill->setHsl(ch, cs, cl);

    *res_outline = *res_fill;
    int luma = compute_colour_luminance(*res_fill);

    if(luma > 128) lightness_offset = -128;
    else lightness_offset = 80;

    res_outline->getHsl(&ch, &cs, &cl);
    cl = max(0, min(255, (cl + lightness_offset)));
    res_outline->setHsl(ch, cs, cl);
}

bool DecodeTrace::enabled()
{
	return true;
}
  
void DecodeTrace::set_view(pv::view::View *view)
{
	assert(view);
	Trace::set_view(view);
}

void DecodeTrace::paint_back(QPainter &p, int left, int right, QColor fore, QColor back)
{
    (void)back;

    QColor backFore = fore;
    backFore.setAlpha(View::BackAlpha);
    QPen pen(backFore);
    pen.setStyle(Qt::DotLine);
    p.setPen(pen);
    const double sigY = get_y() - (_totalHeight - _view->get_signalHeight())*0.5;
    p.drawLine(left, sigY, right, sigY);

    // --draw decode region control
    const double samples_per_pixel = _session->cur_snap_samplerate() * _view->scale();
    const double startX = _decode_start/samples_per_pixel - _view->x_offset();
    const double endX = _decode_end/samples_per_pixel - _view->x_offset();
    const double regionY = get_y() - _totalHeight*0.5 - ControlRectWidth;

    p.setBrush(View::Blue);
    p.drawLine(startX, regionY, startX, regionY + _totalHeight + ControlRectWidth);
    p.drawLine(endX, regionY, endX, regionY + _totalHeight + ControlRectWidth);
    const QPointF start_points[] = {
        QPointF(startX-ControlRectWidth, regionY),
        QPointF(startX+ControlRectWidth, regionY),
        QPointF(startX, regionY+ControlRectWidth)
    };
    const QPointF end_points[] = {
        QPointF(endX-ControlRectWidth, regionY),
        QPointF(endX+ControlRectWidth, regionY),
        QPointF(endX, regionY+ControlRectWidth)
    };
    p.drawPolygon(start_points, countof(start_points));
    p.drawPolygon(end_points, countof(end_points));
}

void DecodeTrace::paint_mid(QPainter &p, int left, int right, QColor fore, QColor back)
{
    using namespace pv::data::decode;

    assert(_decoder_stack);
    const QString err = _decoder_stack->error_message();
    if (!err.isEmpty())
    {
        draw_error(p, err, left, right);
    }

    const double scale = _view->scale();
    assert(scale > 0);

    double samplerate = _decoder_stack->samplerate();

    // Show sample rate as 1Hz when it is unknown
    if (samplerate == 0.0)
        samplerate = 1.0;

    const int64_t pixels_offset = _view->x_offset();
    const double samples_per_pixel = samplerate * scale;

    uint64_t start_sample = (uint64_t)max((left + pixels_offset) *
        samples_per_pixel, 0.0);
    uint64_t end_sample = (uint64_t)max((right + pixels_offset) *
        samples_per_pixel, 0.0);

    for(auto dec : _decoder_stack->stack()) {
        start_sample = max(dec->decode_start(), start_sample);
        end_sample = min(dec->decode_end(), end_sample);
        break;
    }

    if (end_sample < start_sample)
        return;

    const int annotation_height = _view->get_signalHeight();

    for_each_row([&](const Row *row, const QString &, int y, int local_row) {
        if (row == NULL) {
            draw_unshown_row(p, y, annotation_height, left, right, L_S(STR_PAGE_DLG, S_ID(IDS_DLG_UNSHOWN), "Unshown"), fore, back);
            return;
        }

        double min_annWidth;
        if (!row_detailed(*row, samples_per_pixel, min_annWidth)) {
            draw_nodetail(p, annotation_height, left, right, y, local_row, fore, back);
            return;
        }

        double last_x = -1;

        // Annotations narrower than a pixel next to each other
        // make a solid bar; draw those runs as one bar.
        const double merge_pixels = 1.0;
        const bool merge_instants = min(min_annWidth, (double)annotation_height) < merge_pixels;
        std::vector<AnnotationSpan> spans;

        if (_decoder_stack->get_annotation_spans(spans, *row,
                start_sample, end_sample, samples_per_pixel,
                merge_pixels, merge_instants)) {
            for (const AnnotationSpan &span : spans) {
                if (span.last == NULL)
                    draw_annotation(*span.ann, p,
                        annotation_height, left, right,
                        samples_per_pixel, pixels_offset, y,
                        local_row, min_annWidth, fore, back, last_x);
                else
                    draw_annotation_run(*span.ann, *span.last, p,
                        annotation_height, left, right,
                        samples_per_pixel, pixels_offset, y,
                        local_row, min_annWidth);
            }
        }
        else {
            // Overlapping or out of order: check every annotation.
            std::vector<Annotation*> annotations;
            _decoder_stack->get_annotation_subset(annotations, *row,
                start_sample, end_sample);

            for(Annotation *a : annotations){
                draw_annotation(*a, p,
                    annotation_height, left, right,
                    samples_per_pixel, pixels_offset, y,
                    local_row, min_annWidth, fore, back, last_x);
            }
        }
    });
}

void DecodeTrace::for_each_row(const std::function<void(const pv::data::decode::Row *row,
    const QString &heading, int y, int local_row)> &visit)
{
    using namespace pv::data::decode;

    const int annotation_height = _view->get_signalHeight();
    int y = get_y() - (_totalHeight - annotation_height)*0.5;

    for(auto dec :_decoder_stack->stack()) {
        if (!dec->shown()) {
            visit(NULL, QString::fromUtf8(dec->decoder()->name), y, 0);
            y += annotation_height;
            continue;
        }

        int local_row = 0;
        const std::map<const Row, bool> rows = _decoder_stack->get_rows_gshow();

        for (auto i = rows.begin(); i != rows.end(); i++) {
            if ((*i).first.decoder() == dec->decoder() &&
                _decoder_stack->has_annotations((*i).first)) {
                if ((*i).second) {
                    visit(&(*i).first, (*i).first.title(), y, local_row);
                    y += annotation_height;
                }
            }
            local_row++;
        }
    }
}

bool DecodeTrace::row_detailed(const pv::data::decode::Row &row,
    double samples_per_pixel, double &min_annWidth)
{
    const uint64_t min_annotation = _decoder_stack->get_min_annotation(row);
    min_annWidth = min_annotation / samples_per_pixel;

    const uint64_t max_annotation = _decoder_stack->get_max_annotation(row);
    const double max_annWidth = max_annotation / samples_per_pixel;

    return (max_annWidth > 100) ||
        (max_annWidth > 10 && (min_annWidth > 1 || samples_per_pixel < 50)) ||
        (max_annWidth == 0 && samples_per_pixel < 10);
}

bool DecodeTrace::annotation_at(const QPoint &pt, AnnotationHover &hover)
{
    using namespace pv::data::decode;

    assert(_decoder_stack);

    if (_view == NULL || _delete_flag || abs(pt.y() - get_y()) > _totalHeight / 2)
        return false;

    // A new decode clears the annotation texts before the rows, on the
    // decode thread. Only read them once it's done, like the control
    // interface does.
    if (_session->is_decode_pending(this))
        return false;

    const double samplerate = _decoder_stack->samplerate();
    const double samples_per_pixel = (samplerate == 0.0 ? 1.0 : samplerate) * _view->scale();
    if (samples_per_pixel <= 0)
        return false;

    const int64_t pixels_offset = _view->x_offset();
    const int h = _view->get_signalHeight();
    const double x = pt.x();

    // paint_mid() draws nothing outside the first decoder's region.
    const double cursor_sample = (x + pixels_offset) * samples_per_pixel;
    for(auto dec : _decoder_stack->stack()) {
        if (cursor_sample < dec->decode_start() || cursor_sample > (double)dec->decode_end() + 1)
            return false;
        break;
    }

    // How far off an annotation the mouse may be, for instants and ranges
    // drawn as a line.
    const double tolerance = 3;
    bool found = false;
    bool row_hit = false;

    for_each_row([&](const Row *row, const QString &heading, int y, int local_row) {
        (void)heading;
        (void)local_row;

        const int top = y - h / 2;
        if (row_hit || pt.y() < top || pt.y() >= top + h)
            return;
        row_hit = true;

        double min_annWidth;
        if (row == NULL || !row_detailed(*row, samples_per_pixel, min_annWidth))
            return;

        // Drawn like draw_instant().
        const double instant_half = max(min(min_annWidth, (double)h) / 2, 0.5);
        const double reach = tolerance + instant_half;
        const uint64_t first = (uint64_t)max(floor((x - reach + pixels_offset) * samples_per_pixel), 0.0);
        const uint64_t last = (uint64_t)max(ceil((x + reach + pixels_offset) * samples_per_pixel), 0.0);

        std::vector<Annotation> candidates;
        _decoder_stack->get_annotations_near(candidates, *row, first, last);

        const Annotation *best = NULL;
        double best_dist = 0;

        for (const Annotation &a : candidates) {
            double lo = a.start_sample() / samples_per_pixel - pixels_offset;
            double hi = a.end_sample() / samples_per_pixel - pixels_offset;
            if (a.start_sample() == a.end_sample()) {
                lo -= instant_half;
                hi += instant_half;
            }

            const double dist = x < lo ? lo - x : (x > hi ? x - hi : 0);
            // Later ones are drawn on top, so they win ties.
            if (dist <= tolerance && (best == NULL || dist <= best_dist)) {
                best = &a;
                best_dist = dist;
            }
        }

        if (best == NULL)
            return;

        const std::vector<QString> &texts = best->annotations();
        // The texts run from longest to shortest; draw_range() shows the
        // first that fits.
        hover.text = texts.empty() ? QString() : texts.front();
        hover.row_title = row->title();
        hover.start_sample = best->start_sample();
        hover.end_sample = best->end_sample();
        hover.samplerate = samplerate;
        hover.top = top;
        hover.bottom = top + h;
        found = true;
    });

    return found;
}

void DecodeTrace::paint_fore(QPainter &p, int left, int right, QColor fore, QColor back)
{
	using namespace pv::data::decode;

    (void)p;
    (void)left;
	(void)right;
    (void)fore;
    (void)back;
}
 
void DecodeTrace::draw_annotation(const pv::data::decode::Annotation &a,
    QPainter &p, int h, int left, int right,
    double samples_per_pixel, double pixels_offset, int y,
    int local_row, double min_annWidth, QColor fore, QColor back, double &last_x)
{
    const double start = max(a.start_sample() / samples_per_pixel -
        pixels_offset, (double)left);
    const double end = min(a.end_sample() / samples_per_pixel -
        pixels_offset, (double)right);

    if (start > right + DrawPadding || end < left - DrawPadding){
		return;
    }

    if (end - last_x <= 0.5 && end - start < 1){
        return;
    }
    
    last_x = end;

    const QColor fill_base = _colour.isValid() ? _colour :
        get_row_base_colour(local_row);
    QColor fill, outline;
    generate_annotation_colours(fill_base, local_row, a, &fill, &outline);
    const QColor &text_color = get_text_colour(fill);

    if (_decoder_stack->get_mark_index() == (int64_t)(a.start_sample()+ a.end_sample())/2) {
        draw_search_mark(p, (start+end)/2);
    }

	if (a.start_sample() == a.end_sample()){
		draw_instant(a, p, fill, outline, text_color, h,
            start, y, min_annWidth);
    }
    else {
		draw_range(a, p, fill, outline, text_color, h,
            start, end, y, fore, back);
    
        if ((a.type()/100 == 2) && (end - start > 20)) 
        {
            for(auto dec : _decoder_stack->stack())
            {
                auto probes = dec->binded_probe_list();

                for (auto probe : probes) {
                    int type = dec->get_channel_type(probe);

                    if ((type == SRD_CHANNEL_COMMON) ||
                            ((type%100 != a.type()%100) && (type%100 != 0))){
                        continue;
                    }

                    const double mark_end = a.end_sample() / samples_per_pixel - pixels_offset;

                    for(auto s : _session->get_signals()) {
                        int binded_index = dec->binded_probe_index(probe);
                        if((s->get_index() == binded_index) && s->signal_type() == SR_CHANNEL_LOGIC) {
                            view::LogicSignal *logicSig = (view::LogicSignal*)s;
                            logicSig->paint_mark(p, start, mark_end, type/100);
                            break;
                        }
                    }
                }
            }
        }
    }
}

void DecodeTrace::draw_annotation_run(const pv::data::decode::Annotation &a,
    const pv::data::decode::Annotation &last,
    QPainter &p, int h, int left, int right,
    double samples_per_pixel, double pixels_offset, int y,
    int local_row, double min_annWidth)
{
    // One by one, ranges this narrow are drawn as a line at their start in
    // the outline colour, and instants as a narrow rounded rectangle in the
    // fill colour. Cover the same area.
    const bool instant = a.start_sample() == a.end_sample();
    const double pad = instant ? max(min(min_annWidth, (double)h) / 2, 0.5) : 0.5;
    const double start = max(a.start_sample() / samples_per_pixel -
        pixels_offset - pad, (double)left);
    const double end = min(last.start_sample() / samples_per_pixel -
        pixels_offset + pad, (double)right);

    if (start > right + DrawPadding || end < left - DrawPadding)
        return;

    const QColor fill_base = _colour.isValid() ? _colour :
        get_row_base_colour(local_row);
    QColor fill, outline;
    generate_annotation_colours(fill_base, local_row, a, &fill, &outline);

    const double width = max(end - start, 1.0);

    if (instant) {
        p.fillRect(QRectF(start, y - h / 2, width, h), fill);
    }
    else {
        // The lines' square caps reach half a pixel past both ends.
        const double top = y + .5 - h / 2;
        p.fillRect(QRectF(start, top - 0.5, width, 2 * (h / 2) + 1), outline);
    }

    const int64_t mark = _decoder_stack->get_mark_index();
    if (mark >= (int64_t)a.start_sample() && mark <= (int64_t)last.end_sample())
        draw_search_mark(p, mark / samples_per_pixel - pixels_offset);
}

void DecodeTrace::draw_search_mark(QPainter &p, int xpos)
{
    p.setPen(View::Blue);
    int ypos = get_y()+_totalHeight*0.5 + 1;
    const QPoint triangle[] = {
        QPoint(xpos, ypos),
        QPoint(xpos-1, ypos + 1),
        QPoint(xpos, ypos + 1),
        QPoint(xpos+1, ypos + 1),
        QPoint(xpos-2, ypos + 2),
        QPoint(xpos-1, ypos + 2),
        QPoint(xpos, ypos + 2),
        QPoint(xpos+1, ypos + 2),
        QPoint(xpos+2, ypos + 2),
    };
    p.drawPoints(triangle, 9);
}

void DecodeTrace::draw_nodetail(QPainter &p,
    int h, int left, int right, int y,
    int local_row, QColor fore, QColor back)
{
    (void)local_row;
    (void)back;

    const QRectF nodetail_rect(left, y - h/2 + 0.5, right - left, h);
    QString info = L_S(STR_PAGE_DLG, S_ID(ZOOM_IN_FOR_DETAILS), "Zoom in for details");
    int info_left = nodetail_rect.center().x() - p.boundingRect(QRectF(), 0, info).width();
    int info_right = nodetail_rect.center().x() + p.boundingRect(QRectF(), 0, info).width();
    int height = p.boundingRect(QRectF(), 0, info).height();

    p.setPen(fore);
    p.drawLine(left, y, info_left, y);
    p.drawLine(info_right, y, right, y);
    p.drawLine(info_left, y, info_left+5, y - height/2 + 0.5);
    p.drawLine(info_left, y, info_left+5, y + height/2 + 0.5);
    p.drawLine(info_right, y, info_right-5, y - height/2 + 0.5);
    p.drawLine(info_right, y, info_right-5, y + height/2 + 0.5);
    
    p.setPen(fore);
    p.drawText(nodetail_rect, Qt::AlignCenter | Qt::AlignVCenter, info);
}

void DecodeTrace::draw_instant(const pv::data::decode::Annotation &a, QPainter &p,
    QColor fill, QColor outline, QColor text_color, int h, double x, int y, double min_annWidth)
{
    (void)outline;

	const QString text = a.annotations().empty() ?
		QString() : a.annotations().back();
//	const double w = min((double)p.boundingRect(QRectF(), 0, text).width(),
//		0.0) + h;
    const double w = min(min_annWidth, (double)h);
	const QRectF rect(x - w / 2, y - h / 2, w, h);

    //p.setPen(outline);
    p.setPen(QPen(Qt::NoPen));
	p.setBrush(fill);
	p.drawRoundedRect(rect, h / 2, h / 2);

	p.setPen(text_color);
	p.drawText(rect, Qt::AlignCenter | Qt::AlignVCenter, text);
}

void DecodeTrace::draw_range(const pv::data::decode::Annotation &a, QPainter &p,
	QColor fill, QColor outline, QColor text_color, int h, double start,
    double end, int y, QColor fore, QColor back)
{
    (void)fore;
    (void)back;

    AppConfig &app = AppConfig::Instance();
    bool fontStretch = app.appOptions.decoderDynamicFontWidth;

	const double top = y + .5 - h / 2;
	const double bottom = y + .5 + h / 2;
	const std::vector<QString> annotations = a.annotations();

    p.setPen(outline);
    p.setBrush(fill);

    // If the two ends are within 2 pixel, draw a vertical line
    if (start + 2.0 > end)
	{
		p.drawLine(QPointF(start, top), QPointF(start, bottom));
		return;
	}

    double cap_width = min((end - start) / 4, EndCapWidth);

	QPointF pts[] = {
		QPointF(start, y + .5f),
		QPointF(start + cap_width, top),
		QPointF(end - cap_width, top),
		QPointF(end, y + .5f),
		QPointF(end - cap_width, bottom),
		QPointF(start + cap_width, bottom)
	};

    p.drawConvexPolygon(pts, countof(pts));

	if (annotations.empty())
		return;

    // Allow printing inside the caps a little
	QRectF rect(start + cap_width / 2, y - h / 2,
		end - start - cap_width, h);
	if (rect.width() <= 6)
		return;

	p.setPen(text_color);

	// Try to find an annotation that will fit
	QString best_annotation;
    int max_width = rect.width();
    QFontMetrics fm = p.fontMetrics();
    QFont condensed_font = p.font();
    double MIN_STRETCH = fontStretch
        ? app.appOptions.minDecoderFontWidthPercent : 100; // percent
    double MAX_STRETCH = fontStretch
        ? app.appOptions.maxDecoderFontWidthPercent : 100; // percent
    double stretch = 0;

    p.save();
	for(auto &a : annotations) {
        int w = fm.boundingRect(QRect(), 0, a).width();
        stretch = min((double)max_width / (double)w * 100.0, MAX_STRETCH);
        condensed_font.setStretch(max(MIN_STRETCH, stretch));
        QFontMetrics condensed_fm = QFontMetrics(condensed_font);
        int actual_width = condensed_fm.boundingRect(QRect(), 0, a).width();
        dsv_detail("[COND] Init stretch=%f, w=%d, width after stretch: %d", stretch, w,actual_width);
        while(actual_width > max_width + 1 && stretch >= MIN_STRETCH) {
            // Windows font rendering has coarse granularity for font stretching;
            // rendered width may exceed the stretch factor. Iteratively reduce
            stretch--;
            condensed_font.setStretch(stretch);
            condensed_fm = QFontMetrics(condensed_font);
            actual_width = condensed_fm.boundingRect(QRect(), 0, a).width();
            dsv_detail("New stretch: %f, new width: %d", stretch, actual_width);
        }
        if (stretch >= MIN_STRETCH) {
            // fitting annotation found with condensing
            best_annotation = a;
            break;
        }
	}

    dsv_detail("\n\nAnnotation: %s\nStretch: %f", best_annotation.toUtf8().constData(), stretch);
    dsv_detail("Allowed width: %d", max_width);
    int flags = Qt::AlignCenter;
    if (best_annotation.isEmpty()) {
        // no fitting annotation found, use the last one with maximum condensing
        // also alpha blend the right end of the text to indicate truncation
        QLinearGradient gradient(rect.right() - (rect.width() / 5), rect.top(),
            rect.right(), rect.top());
        gradient.setColorAt(0.0, text_color);
        gradient.setColorAt(1.0, QColor(text_color.red(), text_color.green(), text_color.blue(), 0));
        p.setPen(QPen(QBrush(gradient), 1));
		best_annotation = annotations.back();
        condensed_font.setStretch(MIN_STRETCH);
        dsv_detail("[CLIP] Width after stretch: %d", p.boundingRect(QRect(), 0, best_annotation).width());
        flags = Qt::AlignLeft | Qt::AlignVCenter;
    }

    p.setFont(condensed_font);
    p.drawText(rect, flags, best_annotation);
    p.restore();
}

void DecodeTrace::draw_error(QPainter &p, const QString &message,
	int left, int right)
{
	const int y = get_y();
    const int h = get_totalHeight();

    const QRectF text_rect(left, y - h/2 + 0.5, right - left, h);
    const QRectF bounding_rect = p.boundingRect(text_rect,
            Qt::AlignCenter, message);
    p.setPen(Qt::red);

    if (bounding_rect.width() < text_rect.width())
        p.drawText(text_rect, Qt::AlignCenter, L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DECODETRACE_ERROR1), "Error:")+message);
    else
        p.drawText(text_rect, Qt::AlignCenter, L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DECODETRACE_ERROR2), "Error: ..."));
}

void DecodeTrace::draw_unshown_row(QPainter &p, int y, int h, int left,
    int right, QString info, QColor fore, QColor back)
{
    (void)back;

    const QRectF unshown_rect(left, y - h/2 + 0.5, right - left, h);
    int info_left = unshown_rect.center().x() - p.boundingRect(QRectF(), 0, info).width();
    int info_right = unshown_rect.center().x() + p.boundingRect(QRectF(), 0, info).width();
    int height = p.boundingRect(QRectF(), 0, info).height();

    p.setPen(fore);
    p.drawLine(left, y, info_left, y);
    p.drawLine(info_right, y, right, y);
    p.drawLine(info_left, y, info_left+5, y - height/2 + 0.5);
    p.drawLine(info_left, y, info_left+5, y + height/2 + 0.5);
    p.drawLine(info_right, y, info_right-5, y - height/2 + 0.5);
    p.drawLine(info_right, y, info_right-5, y + height/2 + 0.5);

    p.setPen(fore);
    p.drawText(unshown_rect, Qt::AlignCenter | Qt::AlignVCenter, info);
}
  

void DecodeTrace::on_new_decode_data()
{
    decoded_progress(_decoder_stack->get_progress());

    if (_view && _view->session().is_stopped_status())
        _view->data_updated();
    if (_totalHeight/_view->get_signalHeight() != rows_size())
        _view->signals_changed(NULL);
    else if (shown_rows() != _header_rows)
        _view->header_updated(); // the header's arrows: a row came or went
}

int DecodeTrace::get_progress()
{
    return _decoder_stack->get_progress();
}

void DecodeTrace::on_decode_done()
{ 
    on_new_decode_data();
    _session->decode_done();
}
  
int DecodeTrace::rows_size()
{
    const int size = shown_rows();
    return size == 0 ? 1 : size;
}

int DecodeTrace::shown_rows()
{
    using pv::data::decode::Decoder;
    int size = 0;

    for(auto dec : _decoder_stack->stack()) {
        if (dec->shown()) {
            auto rows = _decoder_stack->get_rows_gshow();

            for (auto i = rows.begin(); i != rows.end(); i++) {
                pv::data::decode::Row _row = (*i).first;
                if (_row.decoder() == dec->decoder() &&
                    _decoder_stack->has_annotations((*i).first) &&
                    (*i).second)
                    size++;
            }
        } 
        else {
            size++;
        }
    }
    return size;
}

QString DecodeTrace::channel_list_text(const QFontMetrics &fm, int avail)
{
    if (_index_list.empty())
        return QString();

    // The channels the decoder reads, as runs of consecutive indexes,
    // highest first ("31,28-0"). The bindings come in any order, e.g. from
    // a session file. When the runs don't fit, the channel count takes
    // their place rather than running into the decoder name.
    std::vector<int> indexes(_index_list.begin(), _index_list.end());
    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());

    QStringList runs;
    size_t i = indexes.size();
    while (i > 0) {
        const int high = indexes[--i];
        int low = high;
        while (i > 0 && indexes[i - 1] == low - 1)
            low = indexes[--i];
        runs.append(low == high ? QString::number(high)
                                : QString::number(high) + "-" + QString::number(low));
    }

    QString text = runs.join(",");
    if (fm.horizontalAdvance(text) > avail) {
        text = QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DECODER_CHANNEL_COUNT), "%1 ch"))
                   .arg(indexes.size());
        if (fm.horizontalAdvance(text) > avail)
            return QString();
    }
    return text;
}

// Text that doesn't fit fades out at the right, as annotations do.
static void draw_label_text(QPainter &p, const QRectF &rect,
    const QString &text, QColor colour)
{
    if (p.fontMetrics().horizontalAdvance(text) <= rect.width()) {
        p.setPen(colour);
        p.drawText(rect, Qt::AlignCenter | Qt::AlignVCenter, text);
        return;
    }

    QColor clear = colour;
    clear.setAlpha(0);
    QLinearGradient gradient(rect.right() - rect.width() / 5, rect.top(),
        rect.right(), rect.top());
    gradient.setColorAt(0.0, colour);
    gradient.setColorAt(1.0, clear);
    p.setPen(QPen(QBrush(gradient), 1));
    p.drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, text);
}

// The colour button and the name sit on the trace's centre line, as for
// other traces. Each row paint_mid() draws gets an outlined arrow pointing
// at it, with the row's name in it. On the centre line, the channel list
// goes between the name and that row's arrow, which takes the room left.
void DecodeTrace::paint_label(QPainter &p, int right, const QPoint pt, QColor fore)
{
    using pv::data::decode::Row;

    paint_colour_and_name(p, right, fore);
    _label_boxes.clear();

    if (_view == NULL)
        return;

    const QFontMetrics fm = p.fontMetrics();
    const int y = get_y();
    const int sq = get_squareWidth();
    const int margin = get_squareMargin();
    const int pad = margin + 2;
    const int box_right = right - sq / 2;
    const int min_box = sq;
    const int box_height = max(4, min(sq, _view->get_signalHeight() - 2));

    struct RowLabel {
        int y;
        QString text;
        QString short_text;     // in place of text if that doesn't fit
        QColor colour;
    };
    std::vector<RowLabel> labels;

    // A trace is named after the last decoder of its stack.
    const srd_decoder *named_decoder = _decoder_stack->stack().empty() ? NULL :
        _decoder_stack->stack().back()->decoder();

    for_each_row([&](const Row *row, const QString &heading, int row_y, int local_row) {
        RowLabel l;
        l.y = row_y;
        if (row == NULL) {
            // A decoder that isn't shown: its name, greyed out.
            l.text = heading;
            l.colour = fore;
            l.colour.setAlpha(View::BackAlpha);
        }
        else {
            // The rows of the decoder the trace is named after go without
            // the decoder's name; the other decoders' rows keep it if
            // there is room.
            l.short_text = row->desc();
            l.text = (row->decoder() == named_decoder) ? l.short_text : heading;
            l.colour = _colour.isValid() ? _colour : get_row_base_colour(local_row);
        }
        labels.push_back(l);
    });

    _header_rows = labels.size();

    // Nothing decoded yet: one arrow, as other traces have.
    if (labels.empty()) {
        RowLabel l;
        l.y = get_zero_vpos();
        l.text = "D";
        l.colour = _colour.isValid() ? _colour : get_row_base_colour(0);
        labels.push_back(l);
    }

    const QRectF name_rect = Trace::get_rect("name", y, right);
    const int line_left = name_rect.left() + 2 * margin +
        min((double)fm.horizontalAdvance(_name), name_rect.width());

    const QString channels = channel_list_text(fm,
        box_right - min_box - 2 * margin - line_left);
    const int centre_left = channels.isEmpty() ? line_left :
        line_left + fm.horizontalAdvance(channels) + 2 * margin;
    int channels_right = box_right - min_box - 2 * margin;

    for (const RowLabel &l : labels) {
        // Beside the name and channel list, or on a line of its own
        const bool centre = abs(l.y - y) < (sq + box_height) / 2;
        const int left_limit = centre ? centre_left : get_leftWidth();
        const int room = box_right - left_limit;
        QString text = l.text;
        if (fm.horizontalAdvance(text) + 2 * pad + 1 > room && !l.short_text.isEmpty())
            text = l.short_text;
        const int wanted = fm.horizontalAdvance(text) + 2 * pad + 1;
        const int box_width = max(min_box, min(wanted, room));

        const QRectF box(box_right - box_width + 0.5, l.y - box_height / 2 + 0.5,
            box_width - 1, box_height - 1);
        const QPointF points[] = {
            box.topLeft(),
            box.topRight(),
            QPointF(right - 0.5, box.center().y()),
            box.bottomRight(),
            box.bottomLeft()
        };
        const QRectF hit(box.left(), box.top(), right - box.left(), box.height());

        // A colour close to the header's background would hardly show as a
        // thin outline (yellow on the light theme, dark green on the dark
        // one): darken or lighten it.
        QColor outline = l.colour;
        const bool dark_back = compute_colour_luminance(fore) > 128;
        for (int i = 0; i < 4; i++) {
            const int luma = compute_colour_luminance(outline);
            if (dark_back && luma < 80)
                outline = outline.lighter(150);
            else if (!dark_back && luma > 160)
                outline = outline.darker(130);
            else
                break;
        }

        p.setPen(QPen(outline, 1));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(points, countof(points));

        if (hit.contains(pt) || selected()) {
            p.setPen(fore);
            paint_label_marker(p, box);
        }

        draw_label_text(p, box.adjusted(pad, 0, -pad, 0), text, fore);

        _label_boxes.push_back(hit);
        if (centre)
            channels_right = min(channels_right, (int)box.left() - 2 * margin);
    }

    // The channel list, right-aligned against the arrow beside it
    const QRectF chan_rect = get_rect(CHNLREG, y, right);
    _channels_left = max((double)line_left, channels_right - chan_rect.width());

    if (channels_right > _channels_left) {
        p.setPen(QPen(fore, 1, Qt::DashLine));
        p.drawLine(QPointF(_channels_left, chan_rect.bottom()),
            QPointF(channels_right, chan_rect.bottom()));
    }

    if (!channels.isEmpty()) {
        p.setPen(fore);
        p.drawText(QRectF(line_left, chan_rect.top(), channels_right - line_left,
            chan_rect.height()), Qt::AlignRight | Qt::AlignVCenter, channels);
    }
}

int DecodeTrace::pt_in_rect(int y, int right, const QPoint &point)
{
    // The row arrows are the trace's label: they select and drag it.
    for (const QRectF &r : _label_boxes) {
        if (r.contains(point))
            return LABEL;
    }

    // The colour button and the name, but not the single label other
    // traces have, nor the channel list.
    const int action = Trace::pt_in_rect(y, right, point);
    if (action == LABEL || (action == NAME && point.x() >= _channels_left))
        return 0;
    return action;
}

QRectF DecodeTrace::get_rect(DecodeSetRegions type, int y, int right)
{
    const QSizeF name_size(right - get_leftWidth() - get_rightWidth(), SquareWidth);

    if (type == CHNLREG)
        return QRectF(
            get_leftWidth() + name_size.width() + Margin,
            y - SquareWidth / 2,
            SquareWidth * SquareNum, SquareWidth);
    else
        return QRectF(0, 0, 0, 0);
}

void DecodeTrace::frame_ended()
{
    const uint64_t last_samples = _session->cur_samplelimits() - 1;

    if (_decode_start > last_samples) {
        _decode_start = 0;
        _decode_cursor1 = 0;
    }

    if (_decode_cursor2 == 0 ||
        _decode_end > last_samples) {
        _decode_end = last_samples;
        _decode_cursor2 = 0;
    }

    decoder()->frame_ended();

    for(auto dec : _decoder_stack->stack()) {
        dec->set_decode_region(_decode_start, _decode_end);
        dec->commit();
    }
}

void* DecodeTrace::get_key_handel()
{
    return _decoder_stack->get_key_handel();
}

//to show decoder's property setting dialog
bool DecodeTrace::create_popup(bool isnew)
{ 
    (void)isnew;
    
    int ret = false;  //setting have changed flag
    bool bOpenDlg = true;

    while (bOpenDlg)
    {
        bOpenDlg = false;
        QWidget *top = AppControl::Instance()->GetTopWindow();
        dialogs::DecoderOptionsDlg dlg(top);
        dlg.set_cursor_range(_decode_cursor1, _decode_cursor2);
        dlg.load_options(this);

        int dlg_ret = dlg.exec();

        if (QDialog::Accepted == dlg_ret)
        {
            dlg.apply_setting();
            
            for(auto dec : _decoder_stack->stack())
            {
                if (dec->commit() || _decoder_stack->options_changed()) {
                    _decoder_stack->set_options_changed(true);
                    _decode_start = dec->decode_start();
                    _decode_end = dec->decode_end();
                    ret = true;
                }
            }

            dlg.get_cursor_range(_decode_cursor1, _decode_cursor2);

            // Reopen the dialog to select the required probes.
            if (ret && _decoder_stack->check_required_probes() == false)
            {
                QString errMsg = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODERSTACK_DECODE_WORK_ERROR),
                            "One or more required channels have not been specified");
                MsgBox::Show(errMsg);
    
                ret = false;
                bOpenDlg = true;
            }
        }

        if (dlg.is_reload_form()){
            ret = false;
            bOpenDlg = true;
        }
    }
 
    return ret;
}

} // namespace view
} // namespace pv
