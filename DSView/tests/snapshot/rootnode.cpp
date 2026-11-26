/*
 * This file is part of DSView, licensed under GPLv2 or later.
 *
 * LogicSnapshot fed LA_CROSS_DATA up to its total sample count, with totals
 * on and off a root node boundary (2^30 samples). As soon as the last leaf
 * block of a node fills, append_cross_payload moves on to the first block of
 * the next node, so data that ends on a boundary must still find a node there.
 *
 * Each case is its own ctest entry, because an access past the root node
 * vector aborts (checked subscripts, live asserts). A case uses 136 MB per
 * channel and root node of data.
 */

#include "pv/data/logicsnapshot.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using pv::data::LogicSnapshot;

namespace LogicSnapshotTest {

// Declared a friend by LogicSnapshot, which keeps its layout private
class LargeData
{
public:
    static const uint64_t Leaf = LogicSnapshot::LeafBlockSamples;
    static const uint64_t Node = LogicSnapshot::RootNodeSamples;
};

}

namespace {

int failures = 0;

#define CHECK(cond) do { if (!(cond)) { failures++; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

const uint64_t Leaf = LogicSnapshotTest::LargeData::Leaf;
const uint64_t Node = LogicSnapshotTest::LargeData::Node;
const uint64_t MiB = 1 << 20;

uint64_t mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// The captured signal. Random words, except the last leaf block, which holds
// one edge at a known sample so the edge search near the end has something to
// find. Odd channels end high, even ones low.
struct Stream
{
    unsigned channels;
    uint64_t samples; // a multiple of 64
    uint64_t seed;

    uint64_t tail() const { return (samples - 1) / Leaf * Leaf; }
    uint64_t edge(unsigned ch) const { return samples - 1000 - 7 * ch; }
    bool ends_high(unsigned ch) const { return ch & 1; }
    uint64_t bytes() const { return samples / 8 * channels; }

    uint64_t word(unsigned ch, uint64_t w) const
    {
        const uint64_t first = w * 64;
        if (first < tail())
            return mix(seed * 0x2545F4914F6CDD1DULL ^ ((uint64_t)ch << 56) ^ w);

        const uint64_t high = ends_high(ch) ? ~0ULL : 0;
        const uint64_t e = edge(ch);
        if (first + 64 <= e)
            return ~high;
        if (first >= e)
            return high;
        const uint64_t after = ~0ULL << (e - first);
        return (high & after) | (~high & ~after);
    }

    bool level(unsigned ch, uint64_t s) const
    {
        return (word(ch, s / 64) >> (s % 64)) & 1;
    }

    // Bytes [off, off + len) of the interleaved LA_CROSS_DATA stream
    void fill(uint8_t *dst, uint64_t off, uint64_t len) const
    {
        while (len > 0){
            const uint64_t stream_word = off / 8;
            const uint64_t v = word(stream_word % channels, stream_word / channels);
            const uint64_t skip = off % 8;
            const uint64_t n = std::min(8 - skip, len);
            std::memcpy(dst, (const uint8_t*)&v + skip, n);
            dst += n;
            off += n;
            len -= n;
        }
    }
};

struct Probes
{
    std::vector<sr_channel> probes;
    GSList *list = NULL;

    explicit Probes(unsigned count) : probes(count)
    {
        for (unsigned i = 0; i < count; i++){
            std::memset(&probes[i], 0, sizeof(sr_channel));
            probes[i].index = i;
            probes[i].type = SR_CHANNEL_LOGIC;
            probes[i].enabled = TRUE;
            list = g_slist_append(list, &probes[i]);
        }
    }

    ~Probes() { g_slist_free(list); }
};

// Starts a capture of total samples, feeds the stream as packets of the given
// sizes in turn, and ends the capture
void capture(LogicSnapshot &snap, const Stream &st, const Probes &probes,
             uint64_t total, const std::vector<uint64_t> &sizes)
{
    const uint64_t row = 8 * st.channels;
    const uint64_t largest = *std::max_element(sizes.begin(), sizes.end());
    std::vector<uint8_t> buf(largest + row);
    uint64_t off = 0;
    size_t i = 0;

    while (off < st.bytes()){
        const uint64_t left = st.bytes() - off;
        uint64_t len = std::min(sizes[i++ % sizes.size()], left);
        // A packet holds at least one word per channel
        if (left - len < row)
            len = left;
        st.fill(buf.data(), off, len);

        sr_datafeed_logic logic;
        std::memset(&logic, 0, sizeof(logic));
        logic.format = LA_CROSS_DATA;
        logic.length = len;
        logic.unitsize = 1;
        logic.data = buf.data();

        if (off == 0)
            snap.first_payload(logic, total, probes.list, true);
        else
            snap.append_payload(logic);
        CHECK(!snap.memory_failed());
        off += len;
    }

    snap.capture_ended();
}

// Walks the blocks of one channel and compares them with the end of the
// stream: the snapshot holds its last get_ring_sample_count() samples
void check_blocks(LogicSnapshot &snap, const Stream &st, unsigned ch)
{
    uint64_t pos = st.samples - snap.get_ring_sample_count();
    uint64_t bad = 0;

    for (int b = 0; b < snap.get_block_num(); b++){
        bool level = false;
        const uint64_t size = snap.get_block_size(b);
        const uint64_t *buf = (const uint64_t*)snap.get_block_buf(b, ch, level);
        CHECK(size % 8 == 0);
        CHECK(pos % 64 == 0);

        for (uint64_t j = 0; j < size / 8; j++){
            const uint64_t got = buf ? buf[j] : (level ? ~0ULL : 0);
            if (got != st.word(ch, pos / 64 + j))
                bad++;
        }
        pos += size * 8;
    }

    CHECK(pos == st.samples);
    CHECK(bad == 0);
}

void check_samples(LogicSnapshot &snap, const Stream &st, unsigned ch)
{
    std::vector<uint64_t> at = {0, 63, 64, Leaf - 1, Leaf, st.edge(ch) - 1, st.edge(ch),
                                st.samples - 65, st.samples - 64, st.samples - 1};
    for (uint64_t n = Node; n < st.samples; n += Node){
        at.push_back(n - 1);
        at.push_back(n);
    }
    std::mt19937_64 rng(ch + 1);
    for (int i = 0; i < 1000; i++)
        at.push_back(rng() % st.samples);

    uint64_t bad = 0;
    for (uint64_t s : at){
        if (snap.get_sample(s, ch) != st.level(ch, s))
            bad++;
    }
    CHECK(bad == 0);
}

// The mipmap of the last block, built as the capture reaches its total
void check_last_edge(LogicSnapshot &snap, const Stream &st, unsigned ch)
{
    const bool high = st.ends_high(ch);
    uint64_t index = st.edge(ch) - 3000;
    CHECK(snap.get_nxt_edge(index, !high, st.samples - 1, 1, ch));
    CHECK(index == st.edge(ch));

    index = st.edge(ch);
    CHECK(!snap.get_nxt_edge(index, high, st.samples - 1, 1, ch));
}

void check(LogicSnapshot &snap, const Stream &st, uint64_t total, bool loop)
{
    CHECK(snap.get_ring_sample_count() == total);
    if (!loop){
        CHECK(snap.get_sample_count() == total);
        CHECK(snap.get_block_num() == (int)((total + Leaf - 1) / Leaf));
    }

    for (unsigned ch = 0; ch < st.channels; ch++){
        check_blocks(snap, st, ch);
        // These index the stored samples, which a loop capture has moved on
        if (!loop){
            check_samples(snap, st, ch);
            check_last_edge(snap, st, ch);
        }
    }
}

void run(uint64_t total, unsigned channels, const std::vector<uint64_t> &sizes,
         bool loop = false, uint64_t fed = 0)
{
    Probes probes(channels);
    LogicSnapshot snap;
    snap.set_loop(loop);

    const Stream st = {channels, fed ? fed : total, 1};
    capture(snap, st, probes, total, sizes);
    check(snap, st, total, loop);
    snap.clear();
}

// A second capture with the same total and channels clears the nodes it
// already has instead of making new ones
void run_reuse()
{
    Probes probes(1);
    LogicSnapshot snap;

    for (uint64_t seed = 1; seed <= 2; seed++){
        const Stream st = {1, Node, seed};
        capture(snap, st, probes, Node, {MiB});
        check(snap, st, Node, false);
    }
    snap.clear();
}

}

int main(int argc, char **argv)
{
    const std::string name = argc == 2 ? argv[1] : "";

    if (name == "one-node")
        run(Node, 1, {MiB});
    else if (name == "two-nodes")
        run(2 * Node, 1, {MiB});
    else if (name == "mid-row")
        // The DSL driver's 1 MiB transfers end inside a row of three channels'
        // words, so the next packet starts by finishing the row
        run(Node, 3, {MiB});
    else if (name == "short-of-node")
        run(Node - Leaf, 1, {MiB});
    else if (name == "reuse")
        run_reuse();
    else if (name == "loop")
        // Three times round a one node ring
        run(Node, 1, {MiB}, true, 3 * Node);
    else {
        std::printf("usage: %s one-node|two-nodes|mid-row|short-of-node|reuse|loop\n", argv[0]);
        return 2;
    }

    std::printf("%s: %s\n", name.c_str(), failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
