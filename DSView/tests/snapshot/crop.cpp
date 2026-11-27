/*
 * This file is part of DSView, licensed under GPLv2 or later.
 *
 * LogicSnapshot::crop(), which keeps the samples [start, end) of a finished
 * capture. The capture is random words with some leaf blocks held at one
 * level (the snapshot stores those as a level only). After the crop, every
 * block, sample spot checks and the edge search must match the captured
 * signal from start on. Crops that aren't allowed, and one that runs out of
 * memory, must leave the data as it was.
 *
 * Each case is its own ctest entry, as in rootnode.cpp.
 */

#include "pv/data/logicsnapshot.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

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
uint64_t level_only_blocks = 0;

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

// The captured signal: random words, except in the leaf blocks listed in
// flat, where odd channels stay high and even ones low (two in a row make
// sure the copy has a whole block without edges after a shift). Random words are
// sparse in edges now and then (runs of equal words), so the edge search
// has gaps to cross.
struct Stream
{
    unsigned channels;
    uint64_t samples; // a multiple of 64
    std::vector<uint64_t> flat;

    uint64_t word(unsigned ch, uint64_t w) const
    {
        const uint64_t block = w * 64 / Leaf;
        if (std::find(flat.begin(), flat.end(), block) != flat.end())
            return (ch & 1) ? ~0ULL : 0;

        const uint64_t r = mix(((uint64_t)ch << 56) ^ w);
        // One word in eight copies the level of a run instead
        if ((r & 7) == 0)
            return (mix(((uint64_t)ch << 56) ^ (w / 256)) & 1) ? ~0ULL : 0;
        return r;
    }

    bool level(unsigned ch, uint64_t s) const
    {
        return (word(ch, s / 64) >> (s % 64)) & 1;
    }

    // The first sample after s, up to last, whose level differs from s's,
    // or last + 1
    uint64_t next_change(unsigned ch, uint64_t s, uint64_t last) const
    {
        const bool l = level(ch, s);
        const uint64_t same = l ? ~0ULL : 0;
        uint64_t i = s + 1;

        while (i <= last && i % 64 != 0){
            if (level(ch, i) != l)
                return i;
            i++;
        }
        while (i <= last){
            const uint64_t diff = word(ch, i / 64) ^ same;
            if (diff != 0){
                const uint64_t at = i + __builtin_ctzll(diff);
                return at <= last ? at : last + 1;
            }
            i += 64;
        }
        return last + 1;
    }

    uint64_t bytes() const { return samples / 8 * channels; }

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

// Starts a capture of total samples, feeds the stream in 1 MiB packets and
// ends the capture
void capture(LogicSnapshot &snap, const Stream &st, const Probes &probes, uint64_t total)
{
    const uint64_t row = 8 * st.channels;
    std::vector<uint8_t> buf(MiB + row);
    uint64_t off = 0;

    while (off < st.bytes()){
        const uint64_t left = st.bytes() - off;
        uint64_t len = std::min(MiB, left);
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

// The snapshot holds count samples of the stream from sample first on. A
// loop capture's blocks don't start on a leaf block of its samples, and its
// sample indexes have moved on, so only its blocks are compared.
void check(LogicSnapshot &snap, const Stream &st, uint64_t first, uint64_t count, bool loop = false)
{
    CHECK(snap.get_ring_sample_count() == count);
    if (!loop){
        CHECK(snap.get_sample_count() == count);
        CHECK(snap.get_block_num() == (int)((count + Leaf - 1) / Leaf));
    }
    CHECK(!snap.memory_failed());

    for (unsigned ch = 0; ch < st.channels; ch++){
        // Every word of every block. A block without edges, the last one
        // too, is stored as its level only, as a capture does.
        uint64_t pos = 0;
        uint64_t bad = 0;
        uint64_t bad_level_only = 0;

        for (int b = 0; b < snap.get_block_num(); b++){
            bool level = false;
            const uint64_t size = snap.get_block_size(b);
            const uint64_t *buf = (const uint64_t*)snap.get_block_buf(b, ch, level);
            const uint64_t w0 = (first + pos) / 64;
            bool flat = st.word(ch, w0) == 0 || st.word(ch, w0) == ~0ULL;

            for (uint64_t j = 0; j < size / 8; j++){
                const uint64_t want = st.word(ch, w0 + j);
                const uint64_t got = buf ? buf[j] : (level ? ~0ULL : 0);
                if (got != want)
                    bad++;
                flat = flat && want == st.word(ch, w0);
            }
            if (buf == NULL)
                level_only_blocks++;
            // A loop capture's first block isn't a whole block of its own
            if (!(loop && b == 0) && (buf == NULL) != flat){
                bad_level_only++;
                std::printf("block %d of channel %u: %s, flat %d\n", b, ch,
                            buf ? "stored" : "level only", flat);
            }
            pos += size * 8;
        }
        CHECK(pos == count);
        CHECK(bad == 0);
        CHECK(bad_level_only == 0);

        if (loop)
            continue;

        // Samples at the edges of blocks and words, and at random
        std::vector<uint64_t> at = {0, 1, 63, 64, count - 65, count - 64, count - 1};
        for (uint64_t n = Leaf; n < count; n += Leaf){
            at.push_back(n - 1);
            at.push_back(n);
        }
        std::mt19937_64 rng(ch + 1);
        for (int i = 0; i < 2000; i++)
            at.push_back(rng() % count);

        bad = 0;
        for (uint64_t s : at){
            if (snap.get_sample(s, ch) != st.level(ch, first + s))
                bad++;
        }
        CHECK(bad == 0);

        // The edge search, which uses the mipmaps
        bad = 0;
        for (int i = 0; i < 300; i++){
            const uint64_t s = i < 3 ? (uint64_t)i * (count - 1) / 2 : rng() % count;
            const uint64_t last = count - 1;
            const uint64_t want = st.next_change(ch, first + s, first + last) - first;
            uint64_t index = s;
            const bool found = snap.get_nxt_edge(index, st.level(ch, first + s), last, 1, ch);
            if (found != (want <= last) || (found && index != want))
                bad++;
        }
        CHECK(bad == 0);
    }
}

void crop_case(unsigned channels, uint64_t total, std::vector<uint64_t> flat,
               uint64_t start, uint64_t end, bool loop = false, uint64_t fed = 0)
{
    Probes probes(channels);
    LogicSnapshot snap;
    snap.set_loop(loop);

    const Stream st = {channels, fed ? fed : total, flat};
    capture(snap, st, probes, total);

    // A loop capture holds the last total samples it was fed
    const uint64_t first = st.samples - snap.get_ring_sample_count();
    check(snap, st, first, total, loop);

    const uint64_t version = snap.data_version();
    CHECK(snap.crop(start, end) == LogicSnapshot::CropDone);
    CHECK(snap.data_version() != version);
    level_only_blocks = 0;
    check(snap, st, first + start, end - start);
    // The copy has blocks stored as a level, so reading those is covered
    if (!flat.empty())
        CHECK(level_only_blocks > 0);

    // And once more, from the cropped data
    if (end - start > 2 * Leaf){
        const uint64_t s2 = 64 * 11;
        const uint64_t e2 = end - start - 64 * 5;
        CHECK(snap.crop(s2, e2) == LogicSnapshot::CropDone);
        check(snap, st, first + start + s2, e2 - s2);
    }

    snap.clear();
}

void crop_invalid()
{
    Probes probes(2);
    LogicSnapshot snap;
    const uint64_t total = 2 * Leaf + 64 * 100;
    const Stream st = {2, total, {1}};
    capture(snap, st, probes, total);

    const uint64_t version = snap.data_version();
    CHECK(snap.crop(32, total) == LogicSnapshot::CropInvalid);
    CHECK(snap.crop(0, total - 32) == LogicSnapshot::CropInvalid);
    CHECK(snap.crop(64, total + 64) == LogicSnapshot::CropInvalid);
    CHECK(snap.crop(128, 128) == LogicSnapshot::CropInvalid);
    CHECK(snap.crop(192, 128) == LogicSnapshot::CropInvalid);
    CHECK(snap.data_version() == version);
    check(snap, st, 0, total);

    LogicSnapshot empty;
    CHECK(empty.crop(0, 64) == LogicSnapshot::CropInvalid);

    snap.clear();
}

// Bytes of address space in use
uint64_t address_space()
{
    unsigned long pages = 0;
    FILE *f = std::fopen("/proc/self/statm", "r");
    if (f != NULL){
        if (std::fscanf(f, "%lu", &pages) != 1)
            pages = 0;
        std::fclose(f);
    }
    return (uint64_t)pages * sysconf(_SC_PAGESIZE);
}

// A copy that runs out of memory part way leaves the data as it was
void crop_no_memory()
{
#ifdef SNAPSHOT_TEST_ASAN
    std::printf("skipped: ASan reserves the address space this limits\n");
#else
    Probes probes(2);
    LogicSnapshot snap;
    const uint64_t total = 6 * Leaf;
    const Stream st = {2, total, {}};
    capture(snap, st, probes, total);
    const uint64_t version = snap.data_version();

    // Room for the chunk buffer and a few leaf blocks of the copy, which
    // needs twelve
    struct rlimit old_limit;
    CHECK(getrlimit(RLIMIT_AS, &old_limit) == 0);
    struct rlimit limit = old_limit;
    limit.rlim_cur = address_space() + 3 * (Leaf / 8 + 16 * 4096) * 2;
    CHECK(setrlimit(RLIMIT_AS, &limit) == 0);

    const LogicSnapshot::CropResult result = snap.crop(64, total);

    CHECK(setrlimit(RLIMIT_AS, &old_limit) == 0);
    CHECK(result == LogicSnapshot::CropNoMemory);
    CHECK(snap.data_version() == version);
    check(snap, st, 0, total);

    // With the memory back, it works
    CHECK(snap.crop(64, total) == LogicSnapshot::CropDone);
    check(snap, st, 64, total - 64);
    snap.clear();
#endif
}

}

int main(int argc, char **argv)
{
    const std::string name = argc == 2 ? argv[1] : "";

    if (name == "middle")
        // Into a flat block and out of another, inside the first and last
        crop_case(3, 5 * Leaf + 64 * 1000, {1, 2}, Leaf / 2 + 64 * 7, 4 * Leaf + 64 * 5);
    else if (name == "head")
        // Delete before: the first block kept starts inside a flat one
        crop_case(2, 4 * Leaf + 64 * 10, {1, 2}, Leaf + 64 * 3, 4 * Leaf + 64 * 10);
    else if (name == "tail")
        // Delete after: ends one word short of a block
        crop_case(2, 4 * Leaf, {1, 2}, 0, 3 * Leaf - 64);
    else if (name == "block")
        // Bounds on leaf blocks
        crop_case(1, 4 * Leaf, {0, 2}, Leaf, 3 * Leaf);
    else if (name == "node")
        // Across a root node boundary
        crop_case(1, Node + 2 * Leaf, {}, Node - Leaf - 64 * 9, Node + Leaf + 64);
    else if (name == "loop")
        // A loop capture, one node of ring fed twice and a half
        crop_case(1, Node, {}, 3 * Leaf + 64, Node - Leaf - 128, true, 2 * Node + Node / 2);
    else if (name == "invalid")
        crop_invalid();
    else if (name == "no-memory")
        crop_no_memory();
    else {
        std::printf("usage: %s middle|head|tail|block|node|loop|invalid|no-memory\n", argv[0]);
        return 2;
    }

    std::printf("%s: %s\n", name.c_str(), failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
