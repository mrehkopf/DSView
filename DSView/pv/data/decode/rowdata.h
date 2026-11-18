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

#ifndef DSVIEW_PV_DATA_DECODE_ROWDATA_H
#define DSVIEW_PV_DATA_DECODE_ROWDATA_H

#include <vector> 
#include <mutex>

#include "annotation.h"

namespace pv {
namespace data {
namespace decode {

// One thing to draw in an annotation row: a single annotation, or a run of
// annotations too narrow to draw one by one, from ann to last. last is NULL
// for a single annotation.
struct AnnotationSpan
{
    Annotation *ann;
    Annotation *last;
};

class RowData
{
public:
	RowData();
    ~RowData();

public:
	uint64_t get_max_sample();

    uint64_t get_max_annotation();
    uint64_t get_min_annotation();	

    uint64_t get_annotation_index(uint64_t start_sample);

    bool push_annotation(Annotation *a);

    inline uint64_t get_annotation_size(){
        return _item_count;
    }

    bool get_annotation(pv::data::decode::Annotation *ann, uint64_t index);

     /**
	 * Extracts sorted annotations between two period into a vector.
	 */
	void get_annotation_subset(std::vector<pv::data::decode::Annotation*> &dest,
		                        uint64_t start_sample, uint64_t end_sample);

    /**
     * Like get_annotation_subset(), but for drawing at samples_per_pixel:
     * annotations narrower than merge_pixels (instants too, if merge_instants)
     * that follow each other with less than a pixel in between are returned
     * as one run, if there are at least two of them. The work grows with the
     * number of pixels, not annotations.
     * Returns false, leaving dest empty, if the annotations overlap or are
     * out of order; use get_annotation_subset() then.
     */
    bool get_annotation_spans(std::vector<AnnotationSpan> &dest,
                              uint64_t start_sample, uint64_t end_sample,
                              double samples_per_pixel, double merge_pixels,
                              bool merge_instants);

    void clear();

private:
    uint64_t        _max_annotation;
    uint64_t        _min_annotation;
    uint64_t        _item_count;
	std::vector<Annotation*> _annotations;
    // _max_end[i] is the largest end sample of annotations 0..i, so the first
    // annotation that can reach a sample is found by binary search.
    std::vector<uint64_t> _max_end;
    // Whether each annotation starts at or after the end of all earlier ones.
    bool            _ordered;
    static std::mutex _global_visitor_mutex;
};

}
} // data
} // pv

#endif // DSVIEW_PV_DATA_DECODE_ROWDATA_H
