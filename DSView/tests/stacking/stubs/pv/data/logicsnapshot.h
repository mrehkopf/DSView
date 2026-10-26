/* Test stub: records the merged logic words instead of building mipmaps. */
#ifndef DSVIEW_TESTS_STACKING_STUB_LOGICSNAPSHOT_H
#define DSVIEW_TESTS_STACKING_STUB_LOGICSNAPSHOT_H

#include <libsigrok.h>
#include <vector>

namespace pv {
namespace data {

class LogicSnapshot
{
public:
    void first_payload(const sr_datafeed_logic &logic, uint64_t, GSList *, bool)
    {
        _words.clear();
        append_payload(logic);
    }

    void append_payload(const sr_datafeed_logic &logic)
    {
        const uint64_t *data = (const uint64_t*)logic.data;
        _words.insert(_words.end(), data, data + logic.length / sizeof(uint64_t));
    }

    bool memory_failed() const
    {
        return false;
    }

    const std::vector<uint64_t>& words() const
    {
        return _words;
    }

private:
    std::vector<uint64_t> _words;
};

}
}

#endif
