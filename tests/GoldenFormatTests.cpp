// SPDX-License-Identifier: AGPL-3.0-or-later
// What another tool needs to read Phazite's files (docs/versions-format.md, docs/project-format.md), pinned by golden
// files in tests/golden/format: the song's rules as data (song-rules.json); Phazite's own devices' parameters, with the
// names their choices are saved by (devices.json); and test vectors: a small song's pieces in a pack (song.pack) and what
// they hold, as tools/read-versions.pl lists them (song.txt), which CI checks that reader against.
#include "GoldenFiles.h"
#include "TestClips.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"
#include "model/versions/internal/VersionStore.h"

#include <juce_cryptography/juce_cryptography.h>

namespace
{
using namespace phazite::test;
namespace schema = phazite::schema;

juce::StringArray compareTexts (const juce::String& expected, const juce::String& actual)
{
    return compareLines (expected, actual, 40);
}

// A number as a description holds it, in the fewest digits that read back as its float.
juce::var asNumber (float value)
{
    return phazite::writeValue (value).getDoubleValue();
}

const char* getUnitName (phazite::devices::Unit unit)
{
    using phazite::devices::Unit;

    switch (unit)
    {
        case Unit::none:
            return "none";
        case Unit::decibels:
            return "decibels";
        case Unit::hertz:
            return "hertz";
        case Unit::milliseconds:
            return "milliseconds";
        case Unit::seconds:
            return "seconds";
        case Unit::percent:
            return "percent";
        case Unit::semitones:
            return "semitones";
        case Unit::cents:
            return "cents";
        case Unit::note:
            return "note";
        case Unit::ratio:
            return "ratio";
        case Unit::pan:
            return "pan";
        case Unit::choice:
            return "choice";
        case Unit::toggle:
            return "toggle";
        case Unit::degrees:
            return "degrees";
    }

    return "none";
}

juce::String toString (std::string_view text)
{
    return juce::String::fromUTF8 (text.data(), (int) text.size());
}

// The small song the test vectors hold: two tracks, one with one of Phazite's synths with a value set and a clip of 130
// notes (in runs), the other with a plug-in's element and its state's bytes; made through the model, so its IDs are the
// same each time.
phazite::Song::Ptr makeVectorSong()
{
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    phazite::setTrackName (*song, tracks[0], "Keys");
    phazite::setTrackName (*song, tracks[1], "Effects");
    phazite::setChannelVolume (*song, tracks[0], -6.0f);

    const auto synth = phazite::makeOwnDeviceState (*song, phazite::devices::Synth::typeID);
    REQUIRE (phazite::setInstrument (*song, tracks[0], synth).isValid());

    const auto clip = addMidiClip (*song, tracks[0], { at (0.0), at (32.0) }, "Arpeggio");

    for (int i = 0; i < 130; ++i)
        addMidiNote (*song, clip, 48 + (i * 7) % 24, at (i * 0.25), lasting (0.25), 64 + i % 48);

    juce::MemoryBlock state;

    for (int i = 0; i < 64; ++i)
        state.append (&i, 1);

    juce::ValueTree device (schema::DEVICE);
    device.setProperty (schema::deviceID, (juce::int64) song->makeID(), nullptr);
    device.setProperty (schema::deviceType, schema::deviceIsPlugin, nullptr);
    device.setProperty (schema::pluginName, "Test Effect", nullptr);
    device.setProperty (schema::pluginState, phazite::writeBytes (state), nullptr);
    REQUIRE (phazite::insertDeviceState (*song, tracks[1], device).isValid());
    return song;
}

// A pack's pieces as tools/read-versions.pl lists them: a line each, in the order of their names, with its name, its size
// and what it holds, its text or, for bytes, "base64:" and them.
juce::String listPieces (const phazite::PieceStore& pieces)
{
    auto names = pieces.getNames();
    names.sort (false);
    juce::String listing;

    for (const auto& name : names)
    {
        const auto piece = pieces.read (name);
        REQUIRE (piece.has_value());
        const auto isText = piece->getSize() > 0 && static_cast<const char*> (piece->getData())[0] == '<';
        listing << name << ' ' << juce::String ((juce::int64) piece->getSize()) << ' '
                << (isText ? piece->toString() : "base64:" + juce::Base64::toBase64 (piece->getData(), piece->getSize())) << '\n';
    }

    return listing;
}
} // namespace

TEST_SUITE ("golden")
{
    TEST_CASE ("Golden format: the song's rules as data")
    {
        checkGoldenText ("format/song-rules.json", juce::JSON::toString (phazite::schema::describeSongRules()) + "\n", compareTexts);
    }

    TEST_CASE ("Golden format: Phazite's own devices' parameters, each choice by the name songs save it by")
    {
        juce::Array<juce::var> devices;

        for (const auto* info : phazite::getOwnDevices())
        {
            auto* device = new juce::DynamicObject();
            device->setProperty ("type", toString (info->typeID));
            device->setProperty ("name", toString (info->name));
            device->setProperty ("kind", info->kind == phazite::devices::DeviceKind::instrument ? "instrument" : "effect");
            juce::Array<juce::var> parameters;

            for (const auto& parameter : info->parameters)
            {
                auto* entry = new juce::DynamicObject();
                entry->setProperty ("id", toString (parameter.id));
                entry->setProperty ("name", toString (parameter.name));
                entry->setProperty ("unit", getUnitName (parameter.unit));
                entry->setProperty ("minimum", asNumber (parameter.minimum));
                entry->setProperty ("maximum", asNumber (parameter.maximum));
                entry->setProperty ("default", phazite::toSavedValue (parameter, parameter.defaultValue));

                if (parameter.unit == phazite::devices::Unit::choice)
                {
                    juce::Array<juce::var> choices;

                    for (int i = 0; i < (int) parameter.choices.size(); ++i)
                        choices.add (toString (phazite::devices::getChoiceName (parameter, i)));

                    entry->setProperty ("choices", choices);
                }

                parameters.add (juce::var (entry));
            }

            device->setProperty ("parameters", parameters);
            devices.add (juce::var (device));
        }

        checkGoldenText ("format/devices.json", juce::JSON::toString (devices) + "\n", compareTexts);
    }

    TEST_CASE ("Golden format: a small song's pieces in a pack, which another tool reads (test vectors)")
    {
        TemporaryFolder temporary;
        auto song = makeVectorSong();
        const auto versions = temporary.folder.getChildFile ("Versions");

        {
            phazite::VersionStore store (versions, "test-vectors");
            phazite::VersionRecord record;
            record.name = "Test vectors";
            record.saved = 1793800000000;
            record.phazite = "test";
            REQUIRE (store.writeVersion (record, phazite::splitSong (song->getState())).isNotEmpty());
            checkGoldenText ("format/song.txt", listPieces (store.getPieces()), compareTexts);
        }

        // The pack itself, which tools/read-versions.pl lists in CI: written while updating, and read here as it lists.
        const auto packs = versions.getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "*.pack");
        REQUIRE (packs.size() == 1);
        const auto golden = getGoldenFolder().getChildFile ("format").getChildFile ("song.pack");

        if (isUpdatingGoldenFiles())
        {
            REQUIRE (golden.getParentDirectory().createDirectory());
            REQUIRE (packs[0].copyFileTo (golden));
        }

        // Copied under its own name, the SHA-256 of its index, as a store finds a pack.
        REQUIRE (golden.existsAsFile());
        juce::MemoryBlock bytes;
        REQUIRE (golden.loadFileAsData (bytes));
        REQUIRE (bytes.getSize() > 16);
        const auto* end = static_cast<const char*> (bytes.getData()) + bytes.getSize() - 16;
        const auto count = (size_t) juce::ByteOrder::littleEndianInt (end);
        const auto indexStart = (size_t) juce::ByteOrder::littleEndianInt64 (end + 4);
        REQUIRE (indexStart + count * 48 <= bytes.getSize());
        const auto name = juce::SHA256 (static_cast<const char*> (bytes.getData()) + indexStart, count * 48).toHexString();

        const auto copy = temporary.folder.getChildFile ("Golden").getChildFile ("pieces");
        REQUIRE (copy.createDirectory());
        REQUIRE (golden.copyFileTo (copy.getChildFile (name + ".pack")));
        const phazite::PieceStore committed (copy);
        CHECK (committed.getNumPieces() > 0);
        CHECK (listPieces (committed) == listPieces (phazite::PieceStore (versions.getChildFile ("pieces"))));
    }
}
