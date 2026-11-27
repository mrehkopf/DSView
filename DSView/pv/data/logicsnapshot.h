/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
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


#ifndef DSVIEW_PV_DATA_LOGICSNAPSHOT_H
#define DSVIEW_PV_DATA_LOGICSNAPSHOT_H

#include <libsigrok.h> 
#include "snapshot.h"
#include <QString>
#include <utility>
#include <vector>
#include <map>

// Room for eight stacked 32-channel analyzers (LogicStackingMaxAnalyzers)
#define CHANNEL_MAX_COUNT 256

namespace LogicSnapshotTest {
class Pow2;
class Basic;
class LargeData;
class Pulses;
class LongPulses;
}

namespace pv {
namespace data {

class LogicSnapshot : public Snapshot
{
private:
    static const uint64_t ScaleLevel = 4;
    static const uint64_t ScalePower = 6;
    static const uint64_t Scale = 1 << ScalePower;
    static const uint64_t ScaleSize = Scale / 8;
    static const uint64_t RootScalePower = ScalePower;
    static const uint64_t RootScale = 1 << RootScalePower;
    static const uint64_t LeafBlockSpace = (Scale + Scale*Scale +
            Scale*Scale*Scale + Scale*Scale*Scale*Scale) / 8;

    static const uint64_t LeafBlockPower = ScaleLevel*ScalePower;
    static const uint64_t LeafBlockSamples = 1 << LeafBlockPower;
    static const uint64_t RootNodeSamples = LeafBlockSamples*RootScale;

    static const uint64_t RootMask = ~(~0ULL << RootScalePower) << LeafBlockPower;
    static const uint64_t LeafMask = ~(~0ULL << LeafBlockPower);
    static const uint64_t LevelMask[ScaleLevel];
    static const uint64_t LevelOffset[ScaleLevel];

    static const uint64_t MSB =  (1ULL << (Scale - 1));
    static const uint64_t LSB =  (1ULL);

private:
    struct RootNode
    {
        uint64_t tog;
        uint64_t first;
        uint64_t last;
        void *lbp[Scale];
    };

    struct BlockIndex
    {
        uint64_t    root_index;
        uint64_t    lbp_index;
    };

public:
    typedef std::pair<uint64_t, bool> EdgePair;

private:
    void init_all();

public:
    LogicSnapshot();

	virtual ~LogicSnapshot();

    void free_data();

    void clear();

    void init();   

    void first_payload(const sr_datafeed_logic &logic, uint64_t total_sample_count, GSList *channels, bool able_free);

	void append_payload(const sr_datafeed_logic &logic);

    const uint8_t * get_samples(uint64_t start_sample, uint64_t& end_sample, int sig_index, void **lbp=NULL);

    bool get_sample(uint64_t index, int sig_index);

    void capture_ended();

    bool get_display_edges(std::vector<std::pair<bool, bool>> &edges,
                           std::vector<std::pair<uint16_t, bool>> &togs,
                           uint64_t start, uint64_t end, uint16_t width,
                           uint16_t max_togs, double pixels_offset,
                           double min_length, uint16_t sig_index);

    bool get_nxt_edge(uint64_t &index, bool last_sample, uint64_t end,
                      double min_length, int sig_index);

    bool get_pre_edge(uint64_t &index, bool last_sample,
                      double min_length, int sig_index);

    bool has_data(int sig_index);
    int get_block_num();
    uint8_t *get_block_buf(int block_index, int sig_index, bool &sample);   
    uint64_t get_block_size(int block_index);
    
    // For a search on another thread (pv/search/snapshotsource.h). The
    // version changes whenever the data is cleared or replaced, and the
    // calls below fail (-1) once it no longer matches. 0/1 is the result.
    uint64_t data_version();
    int search_level(uint64_t version, uint64_t index, int sig_index);
    int search_next_change(uint64_t version, uint64_t pos, bool level, uint64_t last,
                           int sig_index, uint64_t &found);
    int search_has_channel(uint64_t version, int sig_index);

    inline void set_loop(bool bLoop){
        _is_loop = bLoop;
    }

    inline bool is_loop(){
        return _is_loop;
    }

    void decode_end();

    void free_decode_lpb(void *lbp);

    inline bool is_able_free(){
        return _able_free;
    } 

    inline uint64_t get_loop_offset(){
        return _loop_offset;
    }

    int get_block_index_with_sample(uint64_t sample_index, uint64_t *out_offset);

    enum CropResult {
        CropDone,
        CropInvalid,    // nothing done: bounds not allowed
        CropNoMemory,   // nothing done: no memory for the copy
    };

    /**
     * Keeps only the samples [start, end) and drops the rest; what was
     * sample start becomes sample 0. Both bounds must be multiples of 64
     * (whole words), and end at most get_ring_sample_count(). The capture
     * must be over and no decoder may be reading the data. The part kept
     * is copied first, so for a while it needs memory twice; if that
     * fails, the data stays as it was. A search reading the data sees the
     * version change.
     */
    CropResult crop(uint64_t start, uint64_t end);

private:
    // Sets up empty storage for the channels ch_index, total samples long,
    // as first_payload() does for a new capture. Throws std::bad_alloc.
    void init_channels(const std::vector<uint16_t> &ch_index, uint64_t total);

    // Copies count samples of channel sig_index from sample 'from' on, one
    // 64-bit word (64 samples, first in bit 0) to every stride-th element of
    // out. from and count are multiples of 64.
    void read_words(uint64_t from, uint64_t count, int sig_index,
                    uint64_t *out, unsigned int stride);

    int get_block_num_unlock();
    uint64_t get_block_size_unlock(int block_index);
    uint8_t *get_block_buf_unlock(int block_index, int sig_index, bool &sample);
    bool get_sample_unlock(uint64_t index, int sig_index);
    bool get_sample_self(uint64_t index, int sig_index);

    bool get_nxt_edge_unlock(uint64_t &index, bool last_sample, uint64_t end,
                      double min_length, int sig_index);
    bool get_nxt_edge_self(uint64_t &index, bool last_sample, uint64_t end,
                      double min_length, int sig_index);

    bool get_pre_edge_self(uint64_t &index, bool last_sample,
                      double min_length, int sig_index);

    int get_ch_order(int sig_index);

    void calc_mipmap(unsigned int order, uint8_t index0, uint8_t index1, uint64_t samples, bool isEnd);

    void append_cross_payload(const sr_datafeed_logic &logic);

    bool lbp_nxt_edge(uint64_t &index, uint64_t root_index, uint64_t lbp_tog, uint8_t lbp_tog_pos,
                      bool aft_tog, uint8_t aft_pos, bool last_sample, int sig_index);

    bool block_nxt_edge(uint64_t *lbp, uint64_t &index, uint64_t block_end, bool last_sample,
                        unsigned int min_level);

    bool lbp_pre_edge(uint64_t &index, uint64_t root_index, uint64_t lbp_tog, uint8_t &lbp_tog_pos,
                      bool pre_tog, uint8_t pre_pos, bool last_sample, int sig_index);

    bool block_pre_edge(uint64_t *lbp, uint64_t &index, bool last_sample,
                        unsigned int min_level, int sig_index);

    inline uint8_t bsf_folded (uint64_t bb)
    {
        static const uint8_t lsb_64_table[64] = {
            63, 30,  3, 32, 59, 14, 11, 33,
            60, 24, 50,  9, 55, 19, 21, 34,
            61, 29,  2, 53, 51, 23, 41, 18,
            56, 28,  1, 43, 46, 27,  0, 35,
            62, 31, 58,  4,  5, 49, 54,  6,
            15, 52, 12, 40,  7, 42, 45, 16,
            25, 57, 48, 13, 10, 39,  8, 44,
            20, 47, 38, 22, 17, 37, 36, 26
        };
        unsigned int folded;
        bb ^= bb - 1;
        folded = (int) bb ^ (bb >> 32);
        return lsb_64_table[folded * 0x78291ACF >> 26];
    }

    inline uint8_t bsr32(uint32_t bb)
    {
        static const uint8_t msb_256_table[256] = {
            0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
            4, 4, 4, 4, 4, 4, 4, 4,4, 4, 4, 4,4, 4, 4, 4,
            5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
            6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
            6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
            7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
            7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
            7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
            7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
       };
       uint8_t result = 0;

       if (bb > 0xFFFF) {
          bb >>= 16;
          result += 16;
       }
       if (bb > 0xFF) {
          bb >>= 8;
          result += 8;
       }

       return (result + msb_256_table[bb]);
    }

    inline uint8_t bsr64(uint64_t bb)
    {
        const uint32_t hb = bb >> 32;
        return hb ? 32 + bsr32((uint32_t)hb) : bsr32((uint32_t)bb);
    }

    void move_first_node_to_last();

    void free_head_blocks(int count);

private:
    std::vector<std::vector<struct RootNode>> _ch_data;
    uint8_t     _byte_fraction;
    uint16_t    _ch_fraction;
    uint8_t    *_dest_ptr;

    uint64_t    _last_sample[CHANNEL_MAX_COUNT];
    uint64_t    _last_calc_count[CHANNEL_MAX_COUNT];
    bool        _is_loop;
    volatile uint64_t   _loop_offset;
    bool        _able_free;
    std::vector<void*> _free_block_list;
    struct BlockIndex _cur_ref_block_indexs[CHANNEL_MAX_COUNT];
    int         _lst_free_block_index;
    uint64_t    _data_version;
 
	friend class LogicSnapshotTest::Pow2;
	friend class LogicSnapshotTest::Basic;
	friend class LogicSnapshotTest::LargeData;
	friend class LogicSnapshotTest::Pulses;
	friend class LogicSnapshotTest::LongPulses;
};

} // namespace data
} // namespace pv

#endif // DSVIEW_PV_DATA_LOGICSNAPSHOT_H
