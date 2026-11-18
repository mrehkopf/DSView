/*
 * This file is part of the PulseView project.
 * DSView is based on PulseView.
 * 
 * Copyright (C) 2014 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2016 DreamSourceLab <support@dreamsourcelab.com>
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

#include <math.h>
#include <assert.h>
#include <algorithm>

#include "rowdata.h"

using std::max;
using std::min;
using std::vector;

namespace pv {
namespace data {
namespace decode {

std::mutex RowData::_global_visitor_mutex;

RowData::RowData() :
    _max_annotation(0),
    _min_annotation(0),
    _ordered(true)
{
    _item_count = 0;
}

RowData::~RowData()
{
    //stack object can not destory the sources
}

void RowData::clear()
{
    std::lock_guard<std::mutex> lock(_global_visitor_mutex);

    //destroy objercts
    for (Annotation *p : _annotations){
        delete p;
    }
    _annotations.clear();
    _max_end.clear();
    _ordered = true;
    _item_count = 0;
    _min_annotation = 0;
    _max_annotation = 0;
}

uint64_t RowData::get_max_sample()
{
    std::lock_guard<std::mutex> lock(_global_visitor_mutex); 

	if (_annotations.empty())
		return 0;
	return _annotations.back()->end_sample();
}

uint64_t RowData::get_max_annotation()
{
    return _max_annotation;
}

uint64_t RowData::get_min_annotation()
{
    if (_min_annotation == 0)
        return 10;
    else
        return _min_annotation;
}

void RowData::get_annotation_subset(std::vector<pv::data::decode::Annotation*> &dest,
		                        uint64_t start_sample, uint64_t end_sample)
{  
    std::lock_guard<std::mutex> lock(_global_visitor_mutex);

    for (Annotation *p : _annotations)
    {
        if (p->end_sample() > start_sample && p->start_sample() <= end_sample)
        {
            dest.push_back(p);
        }			               
    }
}

bool RowData::get_annotation_spans(std::vector<AnnotationSpan> &dest,
                                   uint64_t start_sample, uint64_t end_sample,
                                   double samples_per_pixel, double merge_pixels,
                                   bool merge_instants)
{
    assert(samples_per_pixel > 0);

    std::lock_guard<std::mutex> lock(_global_visitor_mutex);

    if (!_ordered)
        return false;

    const size_t count = _annotations.size();
    const double merge_length = merge_pixels * samples_per_pixel;

    auto mergeable = [&](const Annotation *a) {
        const uint64_t length = a->end_sample() - a->start_sample();
        return length == 0 ? merge_instants : length < merge_length;
    };

    // The first annotation from index 'from' on that starts at or after 'sample'.
    auto first_starting_from = [&](size_t from, double sample) {
        auto it = std::lower_bound(_annotations.begin() + from, _annotations.end(), sample,
            [](const Annotation *a, double s) { return a->start_sample() < s; });
        return (size_t)(it - _annotations.begin());
    };

    // Annotations before this one all end before the range.
    size_t i = std::upper_bound(_max_end.begin(), _max_end.end(), start_sample) - _max_end.begin();

    while (i < count && _annotations[i]->start_sample() <= end_sample) {
        Annotation *a = _annotations[i++];

        if (a->end_sample() <= start_sample)
            continue;

        if (!mergeable(a)) {
            dest.push_back({a, NULL});
            continue;
        }

        Annotation *last = a;
        uint64_t run_end = a->end_sample();

        while (i < count && run_end <= end_sample) {
            // Annotations starting on a pixel the run already covers add
            // nothing to it, so jump past them. As none overlap, all but the
            // last of them end on that pixel too; the last may be wide and
            // gets drawn on its own.
            const double next_pixel = min((floor(run_end / samples_per_pixel) + 1) * samples_per_pixel,
                                          (double)end_sample + 1);
            const size_t next = first_starting_from(i, next_pixel);

            if (next > i) {
                Annotation *skipped = _annotations[next - 1];

                if (!mergeable(skipped)) {
                    // The run reaches up to the wide one.
                    if (next - 1 > i)
                        last = _annotations[next - 2];
                    i = next - 1;
                    break;
                }
                last = skipped;
                run_end = max(run_end, skipped->end_sample());
                i = next;
                continue;
            }

            // The next annotation starts on a later pixel. It continues the
            // run if it is narrow too and less than a pixel away.
            Annotation *b = _annotations[i];
            if (b->start_sample() > end_sample || !mergeable(b) ||
                b->start_sample() >= run_end + samples_per_pixel)
                break;

            last = b;
            run_end = max(run_end, b->end_sample());
            i++;
        }

        dest.push_back({a, last == a ? NULL : last});
    }

    return true;
}

uint64_t RowData::get_annotation_index(uint64_t start_sample)
{
    std::lock_guard<std::mutex> lock(_global_visitor_mutex);
    uint64_t index = 0;

     for (Annotation *p : _annotations){
         if (p->start_sample() > start_sample)
             break;
         index++;
     }

    return index;
}

bool RowData::push_annotation(Annotation *a)
{ 
    assert(a);

    std::lock_guard<std::mutex> lock(_global_visitor_mutex);

    try {
      if (!_max_end.empty() && a->start_sample() < _max_end.back())
        _ordered = false;

      _max_end.push_back(_max_end.empty() ? a->end_sample() : max(_max_end.back(), a->end_sample()));

      try {
        _annotations.push_back(a);
      } catch (const std::bad_alloc&) {
        _max_end.pop_back();
        throw;
      }

      _item_count = _annotations.size();
      _max_annotation = max(_max_annotation, a->end_sample() - a->start_sample());

      if (a->end_sample() != a->start_sample()){
        if (_min_annotation == 0){
            _min_annotation = a->end_sample() - a->start_sample();
        }
        else{
            _min_annotation = min(_min_annotation, a->end_sample() - a->start_sample());
        }
      }
          
      return true;
      
    } catch (const std::bad_alloc&) {
      return false;
    }
}
 

bool RowData::get_annotation(Annotation *ann, uint64_t index)
{
    assert(ann);

    std::lock_guard<std::mutex> lock(_global_visitor_mutex);

    if (index < _annotations.size()) {
        *ann = *_annotations[index]; //clone
        return true;
    } else {
        return false;
    }
}

} // decode
} // data
} // pv
