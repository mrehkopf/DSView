/* Regression tests for bus significance, validation and stage encoding. */
#include "../../pv/trigger/buspattern.h"

#include <iostream>

using namespace pv::trigger;

static int failures = 0;

static void check(bool success, const char *description)
{
    if (!success) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

int main()
{
    QSet<int> available;
    for (int channel = 0; channel < 32; ++channel)
        available.insert(channel);
    ChannelPattern result;
    const BusMapping reordered{5, 1, 12, 0};
    check(compile_bus_value(reordered, " 0xA ", available, result) == BusValueError::None,
        "optional prefix and surrounding whitespace");
    check(result == ChannelPattern{{5, '0'}, {1, '1'}, {12, '0'}, {0, '1'}},
        "D0 is least significant regardless of physical ordering");
    quint32 value = 0;
    check(read_bus_value(reordered, result, value) && value == 10, "reordered readback");

    const ChannelPattern before = result;
    for (const QString input : {"", "0x", "-1", "+1", "1 2", "GG", "0b10"}) {
        // 0b10 consists entirely of valid hexadecimal digits; it means 0xB10,
        // and must fail the 4-bit range check rather than be parsed as binary.
        const auto error = compile_bus_value(reordered, input, available, result);
        check(error != BusValueError::None && result == before, "invalid input leaves output untouched");
    }
    check(compile_bus_value(reordered, "10", available, result) == BusValueError::OutOfRange,
        "hexadecimal input is never silently decimal");
    check(compile_bus_value(reordered, "FFFFFFFFFFFFFFFFF", available, result) == BusValueError::OutOfRange,
        "integer overflow is rejected");
    check(compile_bus_value({}, "0", available, result) == BusValueError::EmptyBus, "empty bus");
    check(compile_bus_value({-1, -1}, "0", available, result) == BusValueError::EmptyBus, "clock-only bus");
    check(compile_bus_value({0, 1, 2, 3, 4}, "20", available, result) == BusValueError::OutOfRange,
        "non-nibble width range");
    check(compile_bus_value({0, 1, 2, 3, 4}, "1f", available, result) == BusValueError::None,
        "lowercase and non-nibble maximum");

    check(compile_bus_value({7, -1, 2}, "5", available, result) == BusValueError::None,
        "sparse zero hole accepted");
    check(result == ChannelPattern{{7, '1'}, {2, '1'}}, "holes are not compacted");
    check(compile_bus_value({7, -1, 2}, "7", available, result) == BusValueError::UnmappedBit,
        "one in an unassigned bit rejected");
    check(read_bus_value({7, -1, 2}, result, value) && value == 5, "sparse readback");
    available.remove(7);
    check(compile_bus_value({7, 2}, "0", available, result) == BusValueError::UnavailableChannel,
        "disabled physical channel rejected, even for a zero bit");
    available.insert(7);
    check(compile_bus_value({32}, "0", available, result) == BusValueError::UnavailableChannel,
        "unsupported physical index rejected");
    check(compile_bus_value({7, 7}, "1", available, result) == BusValueError::ConflictingBits,
        "conflicting aliases rejected");
    check(compile_bus_value({7, 7}, "3", available, result) == BusValueError::None && result.size() == 1,
        "consistent aliases coalesced");

    BusMapping full;
    for (int bit = 0; bit < 32; ++bit)
        full.append((bit * 13 + 5) % 32);
    for (quint32 word : {quint32(0), quint32(1), quint32(0x80000000), quint32(0xA5F01234), quint32(0xFFFFFFFF)}) {
        check(compile_bus_value(full, QString::number(word, 16), available, result) == BusValueError::None,
            "32-bit input accepted");
        check(read_bus_value(full, result, value) && value == word, "32-bit reordered roundtrip");
    }
    check(compile_bus_value(full, "100000000", available, result) == BusValueError::OutOfRange,
        "33rd bit rejected");
    result[full[0]] = 'X';
    check(!read_bus_value(full, result, value), "don't-care readback is custom");
    for (const QChar edge : {QChar('R'), QChar('F'), QChar('C')}) {
        result[full[0]] = edge;
        check(!read_bus_value(full, result, value), "edge readback is custom");
    }

    const QString low = "X X X X X X X X X X X X X X R X";
    const QString high = "X X X X X X X X X X X X X X X X";
    check(compile_bus_value({0, 15, 16, 31}, "a", available, result) == BusValueError::None,
        "cross-bank mapping");
    const QString patched_low = patch_stage_pattern(low, 0, result);
    const QString patched_high = patch_stage_pattern(high, 16, result);
    check(patched_low == "1 X X X X X X X X X X X X X R 0", "low bank preserves external clock edge");
    check(patched_high == "1 X X X X X X X X X X X X X X 0", "high bank uses physical offset");
    auto pattern = decode_stage_pattern(patched_low, 0);
    const auto upper = decode_stage_pattern(patched_high, 16);
    for (auto it = upper.cbegin(); it != upper.cend(); ++it)
        pattern.insert(it.key(), it.value());
    check(read_bus_value({0, 15, 16, 31}, pattern, value) && value == 10, "cross-bank stage readback");

    // Walking-one cases catch inversions and off-by-one errors at every position.
    for (int bit = 0; bit < 32; ++bit) {
        check(compile_bus_value(full, QString::number(quint32(1) << bit, 16), available, result) == BusValueError::None,
            "walking-one conversion");
        const auto decoded = decode_stage_pattern(patch_stage_pattern(high + " " + low, 0, result), 0);
        for (int other = 0; other < 32; ++other)
            check(decoded.value(full[other]) == (other == bit ? '1' : '0'), "walking-one physical placement");
    }

    // Stacking: a bus spanning both analyzers. Only the primary (0..31) can trigger;
    // the secondary's channels (32 + n) exist but are untriggerable.
    QSet<int> secondary;
    for (int channel = 32; channel < 64; ++channel)
        secondary.insert(channel);
    const BusMapping spanning{4, 5, 6, 7, 36, 37, 38, 39};
    check(compile_bus_value(spanning, "a5", available, result) == BusValueError::UnavailableChannel,
        "without an untriggerable set, secondary channels are still rejected");
    check(compile_bus_value(spanning, "a5", available, secondary, result) == BusValueError::None,
        "spanning bus accepted with untriggerable channels skipped");
    check(result == ChannelPattern{{4, '1'}, {5, '0'}, {6, '1'}, {7, '0'}},
        "only the primary's bits are assigned");
    check(compile_bus_value(spanning, "f5", available, secondary, result) == BusValueError::None &&
        result == ChannelPattern{{4, '1'}, {5, '0'}, {6, '1'}, {7, '0'}},
        "any level on skipped bits gives the same assignments");
    check(compile_bus_value(spanning, "100", available, secondary, result) == BusValueError::OutOfRange,
        "range still covers the full bus width");
    check(compile_bus_value({36, -1, 4}, "2", available, secondary, result) == BusValueError::UnmappedBit,
        "unassigned bits must still be zero");
    check(compile_bus_value({36, 36, 4}, "1", available, secondary, result) == BusValueError::None &&
        result == ChannelPattern{{4, '0'}}, "aliases on a skipped channel are not checked");
    available.remove(5);
    check(compile_bus_value(spanning, "0", available, secondary, result) == BusValueError::UnavailableChannel,
        "a disabled primary channel is still rejected");
    available.insert(5);
    check(compile_bus_value({36, 37}, "0", available, secondary, result) == BusValueError::NoTriggerChannel,
        "a bus entirely on the secondary has no trigger channel");
    check(compile_bus_value({-1, 36}, "0", available, secondary, result) == BusValueError::NoTriggerChannel,
        "no trigger channel with an unassigned bit");
    check(compile_bus_value({-1, -1}, "0", available, secondary, result) == BusValueError::EmptyBus,
        "a bus with no channels is still empty");

    check(untriggerable_bus_bits(spanning, secondary) == 0xF0, "untriggerable bit mask");
    check(untriggerable_bus_bits({36, -1, 4, 40}, secondary) == 0x9, "mask skips unassigned bits");
    check(untriggerable_bus_bits(spanning, {}) == 0, "no mask without stacking");
    check(bus_bit_names(0xF0) == QString::fromUtf8("D4–D7"), "bit range name");
    check(bus_bit_names(0x80000F01) == QString::fromUtf8("D0, D8–D11, D31"), "mixed bit names");
    check(bus_bit_names(0x6) == QString::fromUtf8("D1–D2"), "two-bit range");
    check(bus_bit_names(0) == QString(), "no bits");

    check(compile_bus_value(spanning, "a5", available, secondary, result) == BusValueError::None,
        "spanning value for readback");
    ChannelPattern applied = result;
    check(read_bus_value(spanning, applied, secondary, value) && value == 0x05,
        "readback skips secondary bits and reads them as 0");
    check(!read_bus_value(spanning, applied, value), "readback without the untriggerable set is custom");
    applied.insert(36, 'X');
    check(read_bus_value(spanning, applied, secondary, value) && value == 0x05,
        "don't-care on a skipped channel doesn't make the readback custom");
    check(!read_bus_value({36, 37}, applied, secondary, value), "no readback without a trigger channel");

    if (!failures)
        std::cout << "Bus trigger conversion and stage encoding checks passed.\n";
    return failures ? 1 : 0;
}
