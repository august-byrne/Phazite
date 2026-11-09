// SPDX-License-Identifier: AGPL-3.0-or-later
// A VST3's state as its format's own preset file (.vstpreset), which Phazite keeps and DAWproject holds, and as JUCE's host
// of VST3s takes it (sandbox/core/Vst3State.h): each read back as it was written, and turned into the other.
#include "TestHelpers.h"
#include "sandbox/core/Vst3State.h"

#include <cstring>

namespace
{
using phazite::sandbox::Vst3State;

Vst3State makeState (bool withController)
{
    Vst3State state;
    state.classID = "0123456789ABCDEF0123456789ABCDEF";
    state.component = juce::MemoryBlock ("processor state \0\x01\x02", 19);

    if (withController)
    {
        state.controller = juce::MemoryBlock ("controller", 10);
        state.hasController = true;
    }

    return state;
}

// The SDK's PresetFile layout, read by hand: "VST3", the version, the class ID, then where the list starts.
juce::int64 readListStart (const juce::MemoryBlock& preset)
{
    return (juce::int64) juce::ByteOrder::littleEndianInt64 (static_cast<const char*> (preset.getData()) + 40);
}
} // namespace

TEST_CASE ("A VST3's preset file is written as the SDK writes one, and read back as it was")
{
    for (const auto withController : { true, false })
    {
        const auto state = makeState (withController);
        const auto preset = phazite::sandbox::writeVst3Preset (state);

        const auto* bytes = static_cast<const char*> (preset.getData());
        CHECK (std::memcmp (bytes, "VST3", 4) == 0);
        CHECK (juce::ByteOrder::littleEndianInt (bytes + 4) == 1u);
        CHECK (juce::String::fromUTF8 (bytes + 8, 32) == state.classID);
        CHECK (std::memcmp (bytes + readListStart (preset), "List", 4) == 0);
        CHECK (phazite::sandbox::readVst3ClassID (preset) == state.classID);

        const auto read = phazite::sandbox::readVst3Preset (preset);
        REQUIRE (read.has_value());
        CHECK (read->classID == state.classID);
        CHECK (read->component == state.component);
        CHECK (read->hasController == withController);
        CHECK (read->controller == state.controller);
    }
}

TEST_CASE ("What isn't a VST3 preset file reads as none, and so does one whose chunks don't fit")
{
    CHECK_FALSE (phazite::sandbox::readVst3Preset (juce::MemoryBlock ("not a preset", 12)).has_value());
    CHECK (phazite::sandbox::readVst3ClassID (juce::MemoryBlock ("not a preset", 12)).isEmpty());

    auto preset = phazite::sandbox::writeVst3Preset (makeState (true));

    // The list said to start past the file's end.
    auto pastEnd = preset;
    const auto far = juce::ByteOrder::swapIfBigEndian ((juce::uint64) pastEnd.getSize() + 100);
    std::memcpy (static_cast<char*> (pastEnd.getData()) + 40, &far, sizeof (far));
    CHECK_FALSE (phazite::sandbox::readVst3Preset (pastEnd).has_value());

    // A chunk said to be longer than the file.
    auto tooLong = preset;
    const auto listStart = readListStart (tooLong);
    const auto huge = juce::ByteOrder::swapIfBigEndian ((juce::uint64) 1 << 40);
    std::memcpy (static_cast<char*> (tooLong.getData()) + listStart + 8 + 12, &huge, sizeof (huge));
    CHECK_FALSE (phazite::sandbox::readVst3Preset (tooLong).has_value());
}

TEST_CASE ("JUCE's wrapper of a VST3's state and the preset file hold the same streams, each turned into the other")
{
    const auto state = makeState (true);
    const auto wrapped = phazite::sandbox::writeJuceVst3State (state);

    // JUCE's binary block: its magic number, the text's size, the XML and a zero byte.
    const auto* bytes = static_cast<const char*> (wrapped.getData());
    CHECK (juce::ByteOrder::littleEndianInt (bytes) == 0x21324356u);
    CHECK ((size_t) juce::ByteOrder::littleEndianInt (bytes + 4) == wrapped.getSize() - 9);
    CHECK (bytes[wrapped.getSize() - 1] == 0);

    const auto read = phazite::sandbox::readJuceVst3State (wrapped, state.classID);
    REQUIRE (read.has_value());
    CHECK (read->component == state.component);
    CHECK (read->controller == state.controller);
    CHECK (read->hasController);

    // Through the preset file and back to JUCE's form: the same block.
    const auto again = phazite::sandbox::readVst3Preset (phazite::sandbox::writeVst3Preset (*read));
    REQUIRE (again.has_value());
    CHECK (phazite::sandbox::writeJuceVst3State (*again) == wrapped);

    CHECK_FALSE (phazite::sandbox::readJuceVst3State (juce::MemoryBlock ("VST3 but not JUCE's", 19), {}).has_value());
}
