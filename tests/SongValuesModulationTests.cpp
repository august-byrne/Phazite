// SPDX-License-Identifier: AGPL-3.0-or-later
// What hears the song's modulation change hears it from one place (model/base/internal/SongValues.h:
// Listener::modulationChanged()), by the modulators' and macros' IDs (roadmap step S5.20, modulation's second piece). For each
// edit of modulation made through the model, its Undo and its Redo, the listener is told every modulator and macro that
// differs before and after, compared as values (getModulationValue(), independent of what tells it), with where it was and
// is, the parts that differ among those it was told and its value after as getModulatorValue() reads it; one told that
// doesn't differ was taken out and came back (a track moved); and nothing of another area. Another area's change tells it
// nothing. What listens to the song's root hears each change once: where the tree told nothing but modulation's, the
// project's count of changes and the versions' count of the song's changes go up by the calls the listener was told. The
// area tells once for each change of its value (modulation's switch): an edit's step once where one writer sets it, its Undo
// and its Redo once. And what plays takes a value set by hand at once, without a plan, and what makes a plan makes one, as
// before. The keeper's cases under UndoCheck (a container removed and brought back, a track moved into a folder and back, a
// container preset's macros taken in, a track duplicated, a lone macro refused), a set equal to what is held, the unsaved
// check, a version applied each way and as copied, and Swap's trial on a track with modulation.
//   phazite_tests.exe --test-case="Song values' modulation*"
#include "EnginePlayback.h"
#include "PerformanceCounts.h"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UndoCheck.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/SwapModel.h"
#include "model/devices/internal/Parameters.h"
#include "model/engine/internal/EngineContext.h"
#include "model/midi/MidiLinkModel.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/modulation/internal/Modulation.h"
#include "model/presets/PresetModel.h"
#include "model/presets/internal/Presets.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/VersionHistory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace
{
using Change = phazite::SongValues::ModulationChange;
using phazite::ModulatorID;
using phazite::ModulatorPlace;
using phazite::ModulatorType;
using phazite::ModulatorValue;
using phazite::SongModulation;
using phazite::test::at;
using phazite::test::lasting;

juce::String describe (const ModulatorPlace& place)
{
    return (place.onMaster ? juce::String ("master") : "track " + juce::String (place.track.getRaw()))
           + (place.container.isValid() ? ", container " + juce::String (place.container.getRaw()) : juce::String());
}

// What a modulator's or a macro's changes told, in all: whether it was there as the first told it and is as the last told it,
// where it was then and is now, the parts of all of them, and its value after the last.
struct Entry
{
    bool wasThere = false, isThere = false;
    ModulatorPlace before, after;
    std::uint16_t parts = 0;
    std::optional<ModulatorValue> now;
};

// How many calls the values' listener heard, of each area, and what it was told of the modulators, each by its ID.
struct Told final : phazite::SongValues::Listener
{
    void clipNotesChanged (const std::vector<phazite::ClipID>&) override { ++notes; }
    void clipsChanged (const std::vector<phazite::SongValues::ClipChange>&, const std::vector<phazite::TrackID>&) override { ++clips; }
    void tempoMapChanged() override { ++tempo; }
    void curvesChanged (const std::vector<phazite::ParameterRef>&) override { ++curves; }

    void modulationChanged (const std::vector<Change>& changes, const std::vector<ModulatorPlace>& owners) override
    {
        ++calls;
        CHECK_FALSE ((changes.empty() && owners.empty()));

        for (size_t i = 0; i < changes.size(); ++i)
        {
            const auto& change = changes[i];
            CHECK (std::none_of (changes.begin(), changes.begin() + (std::ptrdiff_t) i,
                                 [&change] (const Change& other) { return other.id == change.id; })); // each once in a call
            CHECK ((change.wasThere || change.isThere));
            CHECK ((change.now != nullptr) == change.isThere);

            if (change.now != nullptr)
            {
                CHECK (change.now->id == change.id);
                CHECK (change.now->place == change.after);
            }

            auto [found, first] = told.try_emplace (change.id.getRaw());
            auto& entry = found->second;

            if (first)
            {
                entry.wasThere = change.wasThere;
                entry.before = change.before;
            }

            entry.isThere = change.isThere;
            entry.after = change.after;
            entry.parts = (std::uint16_t) (entry.parts | change.parts);
            entry.now = change.now != nullptr ? std::optional (*change.now) : std::nullopt;
        }

        reordered.insert (reordered.end(), owners.begin(), owners.end());
    }

    int notes = 0, clips = 0, tempo = 0, curves = 0, calls = 0;
    std::map<std::uint64_t, Entry> told;
    std::vector<ModulatorPlace> reordered;
};

// What a listener of the song's root is told, as the test sees it, without asking the song: under a modulator (a MODULATOR
// or a MACRO, or what is in one; one added or taken out), and all else.
struct TreeHeard final : juce::ValueTree::Listener
{
    explicit TreeHeard (const juce::ValueTree& rootToHear)
        : root (rootToHear)
    {
        root.addListener (this);
    }

    ~TreeHeard() override { root.removeListener (this); }

    static bool isModulator (const juce::ValueTree& element)
    {
        return element.hasType (phazite::schema::MODULATOR) || element.hasType (phazite::schema::MACRO);
    }

    static bool isUnderModulator (juce::ValueTree element)
    {
        for (; element.isValid(); element = element.getParent())
            if (isModulator (element))
                return true;

        return false;
    }

    void hear (bool underModulator) { ++(underModulator ? modulation : other); }

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override { hear (isUnderModulator (tree)); }
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
    {
        hear (isModulator (child) || isUnderModulator (parent));
    }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
    {
        hear (isModulator (child) || isUnderModulator (parent));
    }
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override { hear (isUnderModulator (parent)); }

    juce::ValueTree root;
    int modulation = 0, other = 0;
};

bool holdSameNumbers (const ModulatorValue& a, const ModulatorValue& b)
{
    if (a.values.size() != b.values.size())
        return false;

    for (const auto& [index, held] : a.values)
        if (const auto* other = b.values.find (index); other == nullptr || ! juce::exactlyEqual (held, *other))
            return false;

    return true;
}

bool holdSameSteps (const ModulatorValue& a, const ModulatorValue& b)
{
    if (a.steps.size() != b.steps.size())
        return false;

    for (size_t i = 0; i < a.steps.size(); ++i)
        if (! juce::exactlyEqual (a.steps[i], b.steps[i]))
            return false;

    return true;
}

// The parts in which a modulator differs between two values of the song's modulation, worked out here (independent of the
// keeper): none where it is the same, `made` where it comes or goes or is somewhere else.
std::uint16_t findDifferingParts (const ModulatorValue* was, const ModulatorValue* is)
{
    if (was == nullptr || is == nullptr)
        return was == is ? 0 : Change::made;

    std::uint16_t parts = 0;
    const auto add = [&parts] (std::uint16_t part) { parts = (std::uint16_t) (parts | part); };

    if (! (was->place == is->place))
        add (Change::made);

    if (was->kind != is->kind)
        add (Change::kind);

    if (was->name != is->name)
        add (Change::name);

    if (was->off != is->off)
        add (Change::off);

    if (! holdSameNumbers (*was, *is))
        add (was->values.empty() != is->values.empty() ? (std::uint16_t) (Change::valuesSetByHand | Change::valuesAppeared)
                                                       : (std::uint16_t) Change::valuesSetByHand);

    if (! holdSameSteps (*was, *is))
        add (Change::steps);

    if (! juce::exactlyEqual (was->value, is->value))
        add (Change::macroValue);

    if (! (was->modulations == is->modulations))
        add (Change::modulations);

    return parts;
}

// Each ID of either value.
std::vector<std::uint64_t> getAllIDs (const SongModulation& before, const SongModulation& after)
{
    std::vector<std::uint64_t> ids;

    for (const auto* modulation : { &before, &after })
        for (const auto& [id, value] : modulation->modulators)
            if (std::find (ids.begin(), ids.end(), id) == ids.end())
                ids.push_back (id);

    return ids;
}

// The owners that hold those they held before in another order: each owner's list in both values, the IDs in both, compared.
std::vector<ModulatorPlace> findReordered (const SongModulation& before, const SongModulation& after)
{
    std::vector<ModulatorPlace> owners;

    for (const auto& [id, value] : before.modulators)
    {
        const auto& owner = value.place;
        const auto* was = before.findList (owner);
        const auto* is = after.findList (owner);

        if (was == nullptr || is == nullptr
            || std::any_of (owners.begin(), owners.end(), [&owner] (const ModulatorPlace& each) { return each == owner; }))
            continue;

        std::vector<ModulatorID> keptBefore, keptAfter;

        for (const auto& each : *was)
            if (std::find (is->begin(), is->end(), each) != is->end())
                keptBefore.push_back (each);

        for (const auto& each : *is)
            if (std::find (was->begin(), was->end(), each) != was->end())
                keptAfter.push_back (each);

        if (keptBefore != keptAfter)
            owners.push_back (owner);
    }

    return owners;
}

// A song in a project of its own, with its versions, the values' listener and a listener of its root recording what they
// are told.
struct Watched
{
    Watched()
    {
        REQUIRE (phazite::test::waitForVersionHistory (s.context));
        song.getValues()->addListener (&told);
    }

    ~Watched() { song.getValues()->removeListener (&told); }

    // What a change told: the values' listener; the root's listener, under the modulators and of everything else; how far
    // the project's count of changes and the versions' count of the song's changes went up meanwhile; and the song's
    // modulation before it and after.
    struct Heard
    {
        Told told;
        int modulation = 0, other = 0;
        juce::int64 project = 0, versions = 0;
        SongModulation before, after;
        std::map<std::uint64_t, std::optional<ModulatorValue>> read; // each modulator told, as getModulatorValue() reads it after
    };

    Heard hear (const std::function<void()>& change)
    {
        auto* versions = phazite::getVersionHistory (s.context);
        REQUIRE (versions != nullptr);
        told = {};
        tree.modulation = tree.other = 0;
        const auto projectBefore = s.getProject().getChangeCount();
        const auto versionsBefore = versions->countSongChanges();
        auto before = phazite::getModulationValue (song);
        change();
        auto after = phazite::getModulationValue (song);
        Heard heard;
        heard.told = told;
        heard.modulation = tree.modulation;
        heard.other = tree.other;
        heard.project = s.getProject().getChangeCount() - projectBefore;
        heard.versions = versions->countSongChanges() - versionsBefore;
        heard.before = std::move (before);
        heard.after = std::move (after);

        for (const auto& entry : heard.told.told)
            heard.read[entry.first] = phazite::getModulatorValue (song, ModulatorID::fromRaw (entry.first));

        return heard;
    }

    // A change as one undo step, then its Undo, then its Redo: what each told.
    std::vector<Heard> hearEach (const juce::String& name, const std::function<void()>& change)
    {
        return { hear (
                     [&]
                     {
                         const phazite::UndoStep step (song, name);
                         change();
                     }),
                 hear ([this] { phazite::undo (song); }), hear ([this] { phazite::redo (song); }) };
    }

    phazite::test::TestSong s;
    phazite::Song& song = *phazite::findSong (s.context);
    Told told;
    TreeHeard tree { song.getState() };
};

// What a change and its Undo and Redo told, each against the song's modulation before and after: every modulator that
// differs told, as it is after (there or not, where, its value as getModulatorValue() reads it), with at least the parts that
// differ; one told that doesn't differ as it is after too (one taken out and back, as a track JUCE moves); every owner whose
// modulators came in another order told; nothing of another area unless othersToo; once at most for each change of the
// area's value: once for the step where it sets the area once (severalSets false: one writer that works its whole next value
// out), and once for its Undo and its Redo, which put the area's whole value back; and, where the tree told nothing else, the
// project's and the versions' counts up by the calls the listener was told.
void checkHeard (const juce::String& name, const std::vector<Watched::Heard>& heard, bool othersToo = false, bool severalSets = false)
{
    INFO (name.toStdString());
    const std::array<const char*, 3> moments { "the step", "its Undo", "its Redo" };
    REQUIRE (heard.size() <= moments.size());

    for (size_t i = 0; i < heard.size(); ++i)
    {
        INFO (moments[i]);
        const auto& each = heard[i];
        const auto ids = getAllIDs (each.before, each.after);
        auto differs = false;

        for (const auto id : ids)
        {
            INFO ("modulator " << id);
            const auto* was = each.before.find (ModulatorID::fromRaw (id));
            const auto* is = each.after.find (ModulatorID::fromRaw (id));
            const auto parts = findDifferingParts (was, is);
            const auto found = each.told.told.find (id);

            if (parts == 0 && found == each.told.told.end())
                continue;

            differs = differs || parts != 0;
            REQUIRE (found != each.told.told.end());
            const auto& entry = found->second;
            CHECK ((entry.parts & parts) == parts);
            CHECK (entry.isThere == (is != nullptr));

            // One told that doesn't differ was taken out and came back, as it was.
            if (parts == 0)
            {
                CHECK (entry.wasThere);
                CHECK ((entry.parts & Change::made) != 0);
            }

            if (was != nullptr && parts != 0)
                CHECK (entry.before == was->place);

            if (is != nullptr)
            {
                CHECK_MESSAGE (entry.after == is->place, describe (entry.after) << " told, " << describe (is->place) << " now");
                REQUIRE (entry.now.has_value());
                CHECK (*entry.now == *is);
                const auto& read = each.read.at (id);
                REQUIRE (read.has_value());
                CHECK (*entry.now == *read);
            }
        }

        // Nothing told of a modulator the song had neither before nor after.
        for (const auto& [id, entry] : each.told.told)
            CHECK (std::find (ids.begin(), ids.end(), id) != ids.end());

        for (const auto& owner : findReordered (each.before, each.after))
        {
            differs = true;
            CHECK_MESSAGE (std::any_of (each.told.reordered.begin(), each.told.reordered.end(),
                                        [&owner] (const ModulatorPlace& told) { return told == owner; }),
                           describe (owner) << " not told reordered");
        }

        // Where nothing differs, nothing told but what was taken out and came back (checked above); where something does, told;
        // once for each change of the area's value.
        if (differs)
            CHECK (each.told.calls > 0);

        if (i > 0 || ! severalSets)
            CHECK (each.told.calls <= 1);

        if (! othersToo)
        {
            CHECK (each.told.clips == 0);
            CHECK (each.told.notes == 0);
            CHECK (each.told.tempo == 0);
            CHECK (each.told.curves == 0);
        }

        // Heard once: what listens to the song's root counts what it was told, and its tree's callbacks nothing more.
        if (each.other == 0)
        {
            CHECK (each.project == each.told.calls);
            CHECK (each.versions == each.told.calls);
        }
    }
}

// Whether any change told is of these parts, in a step's hearing.
bool toldAny (const Watched::Heard& heard, ModulatorID id, std::uint16_t parts)
{
    const auto found = heard.told.told.find (id.getRaw());
    return found != heard.told.told.end() && (found->second.parts & parts) != 0;
}

phazite::ParameterRef parameterOf (ModulatorID modulator, const juce::String& id)
{
    return { phazite::DeviceID::fromRaw (modulator.getRaw()), id };
}

phazite::ParameterRef valueOf (ModulatorID macro)
{
    return parameterOf (macro, juce::String (macro.getRaw())); // a macro's one parameter has its own ID
}
} // namespace

TEST_CASE ("Song values' modulation: each edit of a modulator or a macro through the model tells what changed of it, its Undo "
           "and its Redo too, and nothing of another area")
{
    Watched w;
    auto& song = w.song;
    auto& context = w.s.context;
    const auto track = w.s.firstTrack;
    const auto volume = w.s.getVolumeParameter (track);
    const auto pan = w.s.getPanParameter (track);
    ModulatorID lfo, steps, macro;

    const auto added = w.hearEach ("Add LFO", [&] { lfo = phazite::addModulator (context, track, ModulatorType::lfo); });
    checkHeard ("Add LFO", added);
    REQUIRE (lfo.isValid());
    CHECK (toldAny (added[0], lfo, Change::made));
    CHECK_FALSE (added[0].told.told.at (lfo.getRaw()).wasThere);
    CHECK (added[1].told.told.at (lfo.getRaw()).wasThere);
    CHECK_FALSE (added[1].told.told.at (lfo.getRaw()).isThere);

    checkHeard ("Add Step Sequencer",
                w.hearEach ("Add Step Sequencer", [&] { steps = phazite::addModulator (context, track, ModulatorType::steps); }));
    REQUIRE (steps.isValid());
    REQUIRE (phazite::getModulatorValue (song, steps)->values.empty());

    // Its first value set by hand, another, one set back to its default, then the last.
    const auto first = w.hearEach ("Change Steps", [&] { phazite::setValueSetByHand (song, parameterOf (steps, "numSteps"), 8.0f); });
    checkHeard ("Change Steps", first);
    CHECK (toldAny (first[0], steps, Change::valuesAppeared));
    CHECK (toldAny (first[1], steps, Change::valuesAppeared));
    checkHeard ("Change Depth",
                w.hearEach ("Change Depth", [&] { phazite::setValueSetByHand (song, parameterOf (steps, "depth"), 0.5f); }));
    checkHeard ("Change Depth Back",
                w.hearEach ("Change Depth Back", [&] { phazite::setValueSetByHand (song, parameterOf (steps, "depth"), 1.0f); }));
    const auto last = w.hearEach ("Change Steps Back", [&] { phazite::setValueSetByHand (song, parameterOf (steps, "numSteps"), 16.0f); });
    checkHeard ("Change Steps Back", last);
    CHECK (toldAny (last[0], steps, Change::valuesAppeared));
    CHECK (phazite::getModulatorValue (song, steps)->values.empty());

    checkHeard ("Change Step", w.hearEach ("Change Step", [&] { phazite::setStep (context, steps, 3, 0.25f); }));
    checkHeard ("Change Rate", w.hearEach ("Change Rate", [&] { phazite::setValueSetByHand (song, parameterOf (lfo, "rate"), 2.0f); }));
    const auto off = w.hearEach ("Turn Modulator Off", [&] { phazite::setModulatorOn (context, lfo, false); });
    checkHeard ("Turn Modulator Off", off);
    CHECK (toldAny (off[0], lfo, Change::off));

    // A macro: added, renamed, turned, its modulations, removed.
    checkHeard ("Add Macro", w.hearEach ("Add Macro", [&] { macro = phazite::addModulator (context, track, ModulatorType::macro); }));
    REQUIRE (macro.isValid());
    const auto renamed = w.hearEach ("Rename Macro", [&] { phazite::renameMacro (context, macro, "Air"); });
    checkHeard ("Rename Macro", renamed);
    CHECK (toldAny (renamed[0], macro, Change::name));
    const auto turned = w.hearEach ("Change Macro", [&] { phazite::setValueSetByHand (song, valueOf (macro), 0.75f); });
    checkHeard ("Change Macro", turned);
    CHECK (toldAny (turned[0], macro, Change::macroValue));

    const auto modulated = w.hearEach ("Modulate Volume", [&] { phazite::modulate (context, macro, volume, 0.5f); });
    checkHeard ("Modulate Volume", modulated);
    CHECK (toldAny (modulated[0], macro, Change::modulations));
    checkHeard ("Modulate Pan", w.hearEach ("Modulate Pan", [&] { phazite::modulate (context, lfo, pan, 0.25f); }));
    checkHeard ("Change Modulation", w.hearEach ("Change Modulation", [&] { phazite::modulate (context, macro, volume, -0.3f); }));
    checkHeard ("Remove Modulation", w.hearEach ("Remove Modulation", [&] { phazite::removeModulation (context, macro, volume); }));
    checkHeard ("Remove Macro", w.hearEach ("Remove Macro", [&] { phazite::removeModulator (context, macro); }));
    checkHeard ("Remove LFO", w.hearEach ("Remove LFO", [&] { phazite::removeModulator (context, lfo); }));

    // A value set as it is: no change, nothing told.
    const auto same = w.hear ([&] { phazite::setStep (context, steps, 3, 0.25f); });
    CHECK (same.told.calls == 0);
    CHECK (same.project == 0);

    // The master's: told by its place.
    const auto master = phazite::getMasterID (song);
    ModulatorID onMaster;
    const auto masters = w.hearEach ("Add LFO", [&] { onMaster = phazite::addModulator (context, master, ModulatorType::lfo); });
    checkHeard ("Add the Master's LFO", masters);
    REQUIRE (onMaster.isValid());
    CHECK (masters[0].told.told.at (onMaster.getRaw()).after.onMaster);
    checkHeard ("Change the Master's LFO",
                w.hearEach ("Change Rate", [&] { phazite::setValueSetByHand (song, parameterOf (onMaster, "rate"), 3.0f); }));
}

TEST_CASE ("Song values' modulation: what takes modulators along tells them: a container with its macros, a track with its "
           "modulators removed, duplicated, moved into a folder, a folder made a bus and back, a version applied")
{
    Watched w;
    auto& song = w.song;
    auto& context = w.s.context;
    const auto track = w.s.firstTrack;
    const auto lfo = w.s.addModulator (track, ModulatorType::lfo);
    w.s.modulate (lfo, w.s.getPanParameter (track), 0.5f);
    const auto macro = w.s.addModulator (track, ModulatorType::macro);
    w.s.setMacroValue (macro, 0.25f);

    // A container comes with its eight macros, which go with it, and come back with it.
    phazite::DeviceID container;
    const auto withMacros = w.hearEach ("Add Container", [&] { container = phazite::addContainer (context, track); });
    checkHeard ("Add Container", withMacros, true);
    REQUIRE (container.isValid());
    const auto macros = phazite::getContainerMacros (song, container);
    REQUIRE (macros.size() == 8);

    for (const auto id : macros)
    {
        CHECK (withMacros[0].told.told.count (id.getRaw()) == 1);
        CHECK (withMacros[0].told.told.at (id.getRaw()).after.container == container);
        CHECK (withMacros[0].told.told.at (id.getRaw()).after.track == track);
    }

    checkHeard ("Change Container Macro",
                w.hearEach ("Change Macro", [&] { phazite::setValueSetByHand (song, valueOf (macros[2]), 0.5f); }));
    const auto removed = w.hearEach ("Remove Container", [&] { phazite::removeDevice (song, container); });
    checkHeard ("Remove Container", removed, true);
    CHECK (removed[0].told.told.size() == 8);
    phazite::undo (song); // the container back

    // The track with them all, and its own: taken out, brought back.
    const auto trackRemoved = w.hearEach ("Delete Track", [&] { phazite::deleteTracks (song, { track }, nullptr); });
    checkHeard ("Delete Track", trackRemoved, true);
    CHECK (trackRemoved[0].told.told.size() == 10);
    CHECK (trackRemoved[1].told.told.size() == 10);
    phazite::undo (song); // the track back

    // Duplicated: the copy's, under their new IDs, on the copy.
    std::vector<phazite::TrackID> copies;
    const auto duplicated = w.hearEach ("Duplicate Track", [&] { copies = phazite::duplicateTracks (song, { track }, nullptr); });
    checkHeard ("Duplicate Track", duplicated, true);
    REQUIRE (copies.size() == 1);
    CHECK (duplicated[0].told.told.size() == 10);

    for (const auto& [id, entry] : duplicated[0].told.told)
        CHECK (entry.after.track == copies.front());

    // Into a folder, and out: the same modulators, on the same track (JUCE takes it out and puts it in again: two changes of
    // the area's value in the step, its modulators taken out and given back, each told once).
    phazite::TrackID folder;

    {
        const phazite::UndoStep step (song, "Add Folder");
        folder = phazite::insertFolder (song, copies.front(), nullptr);
    }

    checkHeard ("Into the Folder", w.hearEach ("Into the Folder", [&] { phazite::moveTrackIntoFolder (song, track, folder); }), true, true);
    CHECK (phazite::getParentFolder (song, track) == folder);

    // A folder's volume's modulation given to the fader that takes its place, each way.
    const auto bus = w.s.addBus ("Bus", { copies.front() });
    const auto onBus = w.s.addModulator (bus, ModulatorType::lfo);
    w.s.modulate (onBus, w.s.getVolumeParameter (bus), 0.5f);
    const auto toPlain = w.hearEach ("Make Plain Folder", [&] { phazite::makePlainFolder (song, bus); });
    checkHeard ("Make Plain Folder", toPlain, true);
    CHECK (toldAny (toPlain[0], onBus, Change::modulations));
    checkHeard ("Make Bus", w.hearEach ("Make Bus", [&] { phazite::makeBus (song, bus); }), true);

    // A version with other modulation applied, each way, in a step and outside the history.
    const auto before = phazite::test::copyWholeSong (song);

    {
        const phazite::UndoStep step (song, "Other Modulation");
        phazite::setValueSetByHand (song, valueOf (macro), 0.9f);
        phazite::removeModulator (song, lfo);
        phazite::addModulator (song, bus, ModulatorType::random);
    }

    const auto after = phazite::test::copyWholeSong (song);
    const auto apply = [&song] (const juce::ValueTree& version)
    {
        phazite::makeSongLike (song, version);
        phazite::removeBrokenReferences (song);
    };

    checkHeard ("Bring Back", w.hearEach ("Bring Back", [&] { apply (before); }), true);
    checkHeard ("Bring Back Again", w.hearEach ("Bring Back Again", [&] { apply (after); }), true);
    checkHeard ("Another Branch", { w.hear ([&] { phazite::makeSongTheSame (song, before); }) }, true);
    checkHeard ("Back on the Branch", { w.hear ([&] { phazite::makeSongTheSame (song, after); }) }, true);
}

TEST_CASE ("Song values' modulation: a version applied whose master has another ID, which it sets in place, tells the master's "
           "modulators by their place, and what is told after it is what the song holds")
{
    // A version sets a track's ID never in place (a track of another ID is another track, taken out and added: the case above),
    // the master's always (it is one of its kind: ApplySong.cpp, makeLike()). The master's modulators' and its containers'
    // macros' place names no ID (ModulatorPlace::onMaster), so the ID set changes nothing of what is told of them, then or
    // after.
    Watched w;
    auto& song = w.song;
    auto& context = w.s.context;
    const auto master = phazite::getMasterID (song);
    const auto lfo = w.s.addModulator (master, ModulatorType::lfo);
    const auto container = phazite::addContainer (context, master);
    REQUIRE (container.isValid());
    const auto macros = phazite::getContainerMacros (song, container);
    REQUIRE (macros.size() == 8);

    // The version: the LFO's rate and a macro of the container set, the master's ID another (one the song made, so that the
    // version's count of IDs is past it).
    const auto otherID = phazite::TrackID::fromRaw (song.makeID());

    {
        const phazite::UndoStep step (song, "Change Rate");
        phazite::setValueSetByHand (song, parameterOf (lfo, "rate"), 2.0f);
        phazite::setValueSetByHand (song, valueOf (macros[1]), 0.5f);
    }

    auto version = phazite::test::copyWholeSong (song);
    phazite::undo (song);
    auto versionMaster = version.getChildWithName (phazite::schema::MASTER);
    REQUIRE (versionMaster.isValid());
    versionMaster.setProperty (phazite::schema::trackID, (juce::int64) otherID.getRaw(), nullptr);

    const auto applied = w.hearEach ("Apply Version", [&] { phazite::makeSongLike (song, version); });
    checkHeard ("Apply Version", applied, true);
    CHECK (phazite::getMasterID (song) == otherID);

    for (const auto id : { lfo, macros[1] })
    {
        INFO ("modulator " << id.getRaw());

        for (const auto& heard : applied)
        {
            REQUIRE (heard.told.told.count (id.getRaw()) == 1);
            const auto& entry = heard.told.told.at (id.getRaw());
            CHECK (entry.after.onMaster);
            CHECK_FALSE (entry.after.track.isValid());
        }
    }

    // After it, the master's modulators told as the song holds them: each told is what getOwnedModulators() then reads.
    const auto changed = w.hearEach ("Change Rate", [&] { phazite::setValueSetByHand (song, parameterOf (lfo, "rate"), 3.0f); });
    checkHeard ("Change Rate", changed);
    const auto turned = w.hearEach ("Change Macro", [&] { phazite::setValueSetByHand (song, valueOf (macros[1]), 0.25f); });
    checkHeard ("Change Macro", turned);

    const auto isOwned = [&song] (const Watched::Heard& heard, ModulatorID id, const ModulatorPlace& owner)
    {
        const auto owned = phazite::getOwnedModulators (song, owner);
        const auto found = std::find_if (owned.begin(), owned.end(), [id] (const ModulatorValue& each) { return each.id == id; });
        return found != owned.end() && heard.told.told.count (id.getRaw()) == 1 && heard.told.told.at (id.getRaw()).now == *found;
    };

    CHECK (isOwned (changed[2], lfo, { .onMaster = true }));
    CHECK (isOwned (turned[2], macros[1], { .onMaster = true, .container = container }));
}

TEST_CASE ("Song values' modulation: another area's change tells nothing of modulation, nor its Undo and its Redo")
{
    Watched w;
    auto& song = w.song;
    const auto track = w.s.firstTrack;
    const auto lfo = w.s.addModulator (track, ModulatorType::lfo);
    w.s.modulate (lfo, w.s.getPanParameter (track), 0.5f);
    const auto clip = w.s.addMidiClip (track, 0.0, 4.0);
    REQUIRE (clip.isValid());
    const auto eq = w.s.addEffect (track, phazite::devices::Equalizer::typeID);

    const auto check = [&w] (const juce::String& name, const std::function<void()>& change)
    {
        INFO (name.toStdString());

        for (const auto& heard : w.hearEach (name, change))
            CHECK (heard.told.calls == 0);
    };

    check ("Add Clip", [&] { CHECK (phazite::insertMidiClip (song, track, { at (8.0), at (12.0) }, nullptr).isValid()); });
    check ("Add Note", [&] { CHECK (phazite::test::addMidiNote (song, clip, 60, at (1.0), lasting (1.0)).isValid()); });
    check ("Add Automation Point", [&] { phazite::addPoint (song, w.s.getVolumeParameter (track), { at (0.0), 0.5f }); });
    check ("Change Gain", [&] { phazite::setValueSetByHand (song, { eq, "output" }, -3.0f); });
    check ("Rename Track", [&] { phazite::setTrackName (song, track, "Renamed"); });
    check ("Change Volume", [&] { phazite::setChannelVolume (song, track, -6.0f); });
    check ("Change Tempo", [&] { phazite::setTempo (song, 100.0); });
    check ("Add Effect", [&] { w.s.addEffect (track, phazite::devices::Equalizer::typeID); });
    check ("Add Track", [&] { CHECK (phazite::insertTrack (song, track, nullptr).isValid()); });
}

TEST_CASE ("Song values' modulation: a value set by hand, a step, a switch and a macro's value give the engine no plan, and what "
           "a modulator holds else one, as before")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    REQUIRE (s.getProject().save().wasOk());
    phazite::test::PlanCounter plans (s.context);
    ModulatorID lfo, steps, macro;

    struct Edit
    {
        const char* what;
        std::function<void()> make;
        int plans;
    };

    // In the order they are made, each on the song as the ones before left it.
    const std::vector<Edit> edits {
        { "an LFO added", [&] { lfo = phazite::addModulator (s.context, track, ModulatorType::lfo); }, 1 },
        { "a step sequencer added", [&] { steps = phazite::addModulator (s.context, track, ModulatorType::steps); }, 1 },
        // The first value a modulator has set by hand added its VALUES element, which the session took as any element added:
        // a plan (SongValues.h: ModulationChange::valuesAppeared).
        { "a modulator's first value set by hand", [&] { s.setModulatorParameter (steps, "numSteps", 8.0f); }, 1 },
        { "another value set by hand", [&] { s.setModulatorParameter (steps, "depth", 0.5f); }, 0 },
        { "an LFO's rate set by hand", [&] { s.setModulatorParameter (lfo, "rate", 2.0f); }, 0 },
        { "a step set", [&] { phazite::setStep (s.context, steps, 2, -0.5f); }, 0 },
        { "a modulator switched off", [&] { phazite::setModulatorOn (s.context, lfo, false); }, 0 },
        { "a macro added", [&] { macro = phazite::addModulator (s.context, track, ModulatorType::macro); }, 1 },
        { "the macro turned", [&] { s.setMacroValue (macro, 0.5f); }, 0 },
        { "its Undo", [&] { phazite::undo (song); }, 0 },
        { "its Redo", [&] { phazite::redo (song); }, 0 },
        { "the macro renamed", [&] { phazite::renameMacro (s.context, macro, "Air"); }, 0 },
        { "a modulation made", [&] { s.modulate (macro, s.getVolumeParameter (track), 0.5f); }, 1 },
        { "its amount changed", [&] { s.modulate (macro, s.getVolumeParameter (track), -0.5f); }, 1 },
        { "its Undo", [&] { phazite::undo (song); }, 1 },
        { "its Redo", [&] { phazite::redo (song); }, 1 },
        { "the modulation removed", [&] { phazite::removeModulation (s.context, macro, s.getVolumeParameter (track)); }, 1 },
        { "the depth set back", [&] { s.setModulatorParameter (steps, "depth", 1.0f); }, 0 },
        { "the last value set back", [&] { s.setModulatorParameter (steps, "numSteps", 16.0f); }, 1 },
        { "a container added, with its macros", [&] { phazite::addContainer (s.context, track); }, 1 },
        { "a modulator removed", [&] { phazite::removeModulator (s.context, lfo); }, 1 },
    };

    for (const auto& edit : edits)
    {
        INFO (std::string (edit.what));
        edit.make();
        CHECK (plans.take() == edit.plans);
    }
}

TEST_CASE ("Song values' modulation: a modulator switched off, its steps set and its value set by hand play in the next block, "
           "from the value the song's values tell")
{
    phazite::test::TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);

    // An LFO on the volume, at 1 Hz; a step sequencer on the pan, its steps all at 0.
    const auto lfo = song.addModulator (track, ModulatorType::lfo);
    song.setModulatorParameter (lfo, "rateType", 0.0f);
    song.modulate (lfo, song.getVolumeParameter (track), 0.5f);
    const auto steps = song.addModulator (track, ModulatorType::steps);

    for (int i = 0; i < 16; ++i)
        phazite::setStep (song.context, steps, i, 0.0f);

    song.modulate (steps, song.getPanParameter (track), 1.0f);
    phazite::test::EnginePlayback playback;
    const auto volume = phazite::getVolume (song.context, track);
    const auto pan = phazite::getPan (song.context, track);
    song.run ("transport.playStop");
    playback.play (0.3);
    CHECK (std::abs (volume.getValue() - volume.getBaseValue()) > 0.01f);
    CHECK (pan.getValue() == doctest::Approx (pan.getBaseValue()).epsilon (0.001));

    // Off: the volume plays its value.
    phazite::setModulatorOn (song.context, lfo, false);
    playback.play (0.05);
    CHECK (volume.getValue() == doctest::Approx (volume.getBaseValue()).epsilon (0.001));

    // Its steps all up: the pan moves right; its depth turned the other way, left.
    for (int i = 0; i < 16; ++i)
        phazite::setStep (song.context, steps, i, 1.0f);

    playback.play (0.05);
    const auto right = pan.getValue();
    CHECK (right > pan.getBaseValue() + 0.1f);
    song.setModulatorParameter (steps, "depth", -1.0f);
    playback.play (0.05);
    CHECK (pan.getValue() < pan.getBaseValue() - 0.1f);
    song.run ("transport.playStop");
}

TEST_CASE ("Song values' modulation: the unsaved check compares the modulators it was told of with the saved copy, as the whole "
           "comparison does: changed and changed back is saved again, past mostToCompare of them too")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    const auto track = s.firstTrack;
    const auto macro = s.addModulator (track, ModulatorType::macro);
    const auto lfo = s.addModulator (track, ModulatorType::lfo);
    REQUIRE (project.save().wasOk());
    REQUIRE_FALSE (project.hasUnsavedChanges());

    // What the quick check says, against the whole comparison, after the edits.
    const auto check = [&project] (const juce::String& name, bool unsaved, const std::function<void()>& edits)
    {
        INFO (name.toStdString());
        edits();
        CHECK (project.isWholeAsSaved() == ! unsaved);
        CHECK (project.hasUnsavedChanges() == unsaved);

        if (unsaved)
            REQUIRE (project.save().wasOk());
    };

    check ("a macro turned and turned back", false,
           [&]
           {
               s.setMacroValue (macro, 0.5f);
               s.setMacroValue (macro, 0.0f);
           });
    check ("an LFO's rate set and its Undo", false,
           [&]
           {
               s.setModulatorParameter (lfo, "rate", 2.0f);
               phazite::undo (song);
           });
    check ("a modulator added and its adding undone", false,
           [&]
           {
               s.addModulator (track, ModulatorType::random);
               phazite::undo (song);
           });
    check ("a modulation made and taken away", false,
           [&]
           {
               s.modulate (lfo, s.getPanParameter (track), 0.5f);
               phazite::removeModulation (s.context, lfo, s.getPanParameter (track));
           });
    check ("a macro renamed", true, [&] { phazite::renameMacro (s.context, macro, "Air"); });
    check ("a step sequencer added", true, [&] { s.addModulator (track, ModulatorType::steps); });
    check ("the LFO switched off", true, [&] { phazite::setModulatorOn (s.context, lfo, false); });

    // More modulators than are compared one by one: the tracks and the master compared whole.
    std::vector<ModulatorID> many;

    {
        const phazite::UndoStep step (song, "Add LFOs");

        for (size_t i = 0; i < phazite::Project::mostToCompare + 6; ++i)
            many.push_back (phazite::addModulator (song, track, ModulatorType::lfo));
    }

    REQUIRE (project.save().wasOk());
    const auto setRates = [&song, &many] (float rate)
    {
        const phazite::UndoStep step (song, "Change Rates");

        for (const auto id : many)
            phazite::setValueSetByHand (song, parameterOf (id, "rate"), rate);
    };

    check ("many changed and changed back", false,
           [&]
           {
               setRates (3.0f);
               setRates (1.0f);
           });
    check ("many changed", true, [&] { setRates (3.0f); });
}

TEST_CASE ("Song values' modulation: the unsaved check compares the order of a modulator's owner where one was taken out and put "
           "back at another place")
{
    // A modulator taken out of its track and put back last, in two steps before a comparison: the same value, on the same
    // track, but after the other, which the saved copy has after it. No edit puts one back with its ID (an Undo puts it where
    // it was): the value is taken out and set again through modulation's setters, as a version's pass sets one.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    const auto track = s.firstTrack;
    const auto first = s.addModulator (track, ModulatorType::lfo);
    const auto second = s.addModulator (track, ModulatorType::random);
    REQUIRE (project.save().wasOk());
    REQUIRE_FALSE (project.hasUnsavedChanges());
    REQUIRE (phazite::getModulators (song, track) == std::vector { first, second });

    const auto putBackLast = [&song, first]
    {
        const auto value = phazite::getModulatorValue (song, first);
        REQUIRE (value.has_value());

        {
            const phazite::UndoStep step (song, "Remove Modulator");
            REQUIRE (phazite::eraseModulator (song, first));
        }

        const phazite::UndoStep step (song, "Add Modulator");
        REQUIRE (phazite::setModulator (song, *value));
    };

    // Both undone: as saved.
    putBackLast();
    REQUIRE (phazite::getModulators (song, track) == std::vector { second, first });
    phazite::undo (song);
    phazite::undo (song);
    REQUIRE (phazite::getModulators (song, track) == std::vector { first, second });
    CHECK (project.isWholeAsSaved());
    CHECK_FALSE (project.hasUnsavedChanges());

    // Put back last: changed.
    putBackLast();
    CHECK_FALSE (project.isWholeAsSaved());
    CHECK (project.hasUnsavedChanges());
}

TEST_CASE ("Song values' modulation: the keeper keeps a container's macros and a track's modulators with their elements, takes a "
           "new container's in, and refuses a lone macro, each one exact undo step")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto& values = *song.getValues();
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    const auto lfo = s.addModulator (track, ModulatorType::lfo);
    s.modulate (lfo, s.getPanParameter (track), 0.5f);
    const auto container = phazite::addContainer (context, track);
    REQUIRE (container.isValid());
    const auto macros = phazite::getContainerMacros (song, container);
    REQUIRE (macros.size() == 8);
    s.setMacroValue (macros[0], 0.25f);
    phazite::test::UndoCheck undo (context);

    // The tree holds none of them; each is the area's, on its place.
    const auto holdsNoElement = [&song]
    {
        const std::function<bool (const juce::ValueTree&)> none = [&none] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
                if (child.hasType (phazite::schema::MODULATOR) || child.hasType (phazite::schema::MACRO) || ! none (child))
                    return false;

            return true;
        };

        return none (song.getState());
    };

    CHECK (holdsNoElement());

    // (b) A container removed, its macros with it, and brought back by its Undo, their values and places as they were.
    const auto before = phazite::getModulationValue (song);
    undo.checkSeveral ("Remove Device", [&] { phazite::removeDevice (song, container); });
    CHECK (phazite::getContainerMacros (song, container).empty());
    phazite::undo (song);
    CHECK (phazite::getModulationValue (song) == before);
    CHECK (phazite::getModulatorValue (song, macros[0])->value == doctest::Approx (0.25f));
    phazite::redo (song);
    phazite::undo (song);

    // (a) The track, with its modulators and its container, moved into a folder and out: the same values, the same element
    // given them back each time (JUCE takes a track out and puts it in again), their place its ID.
    phazite::TrackID folder;
    undo.checkSeveral ("a folder", [&] { folder = phazite::insertFolder (song, other, nullptr); });
    undo.checkSeveral ("Into the Folder", [&] { phazite::moveTrackIntoFolder (song, track, folder); });
    REQUIRE (phazite::getParentFolder (song, track) == folder);
    CHECK (phazite::getModulationValue (song) == before);
    undo.checkSeveral ("Ungroup Folder", [&] { phazite::ungroupFolder (song, folder); });
    CHECK (phazite::getModulationValue (song) == before);
    CHECK (holdsNoElement());

    // (c) A container from a preset of the one there: its eight macros, made as values with it and set by its inserter in its
    // one step, none taken in by the keeper's safety net; its Undo and its Redo take them out and back.
    const auto preset = phazite::makeDevicePreset (song, container);
    const auto adoptedBefore = values.countModulationAdopted();
    phazite::DeviceID made;
    undo.check ("Add Device",
                [&]
                {
                    const phazite::UndoStep step (song, "Add Device");
                    made = phazite::addDeviceFromPreset (song, phazite::getPluginHost (phazite::test::getEngineContext()), other, preset,
                                                         nullptr);
                });
    REQUIRE (made.isValid());
    const auto madeMacros = phazite::getContainerMacros (song, made);
    REQUIRE (madeMacros.size() == 8);
    CHECK (values.countModulationAdopted() == adoptedBefore);
    CHECK (phazite::getModulatorValue (song, madeMacros[0])->value == doctest::Approx (0.25f));
    CHECK (phazite::getModulatorValue (song, madeMacros[0])->place.track == other);
    CHECK (holdsNoElement());
    phazite::undo (song);
    CHECK (phazite::getContainerMacros (song, made).empty());
    phazite::redo (song);
    CHECK (phazite::getContainerMacros (song, made).size() == 8);

    // (a) with copies: a track duplicated, its modulators and its container's macros given to the copy under new IDs, which
    // modulate the copy's own.
    std::vector<phazite::TrackID> copies;
    undo.checkSeveral ("Duplicate Track", [&] { copies = phazite::duplicateTracks (song, { track }, nullptr); });
    REQUIRE (copies.size() == 1);
    const auto copied = phazite::getModulators (song, copies.front());
    REQUIRE (copied.size() == 1);
    CHECK (copied.front() != lfo);
    REQUIRE (phazite::getModulations (song, copied.front()).size() == 1);
    CHECK (phazite::getModulations (song, copied.front())[0].parameter == s.getPanParameter (copies.front()));
    CHECK (holdsNoElement());
    undo.walkBackAndForth();

    // (d) A lone macro added to a container in a step: refused, the song's modulation as it was; its step's Undo takes the
    // element out again. Outside the history: read in and taken out.
    const auto held = phazite::getModulationValue (song);
    auto containerState = phazite::findDeviceState (song, container);
    const auto loneID = song.makeID();
    juce::ValueTree lone (phazite::schema::MACRO);
    lone.setProperty (phazite::schema::modulatorID, (juce::int64) loneID, nullptr);
    lone.setProperty (phazite::schema::macroName, "Lone", nullptr);

    {
        const phazite::test::ExpectedAssertion refused ("ModulationArea.cpp");
        const phazite::UndoStep step (song, "A Lone Macro");
        containerState.appendChild (lone.createCopy(), &song.getUndoManager());
#if JUCE_DEBUG
        CHECK (refused.getCount() == 1);
#endif
    }

    CHECK (phazite::getModulationValue (song) == held);
    phazite::undo (song);
    CHECK (holdsNoElement());

    {
        const phazite::Song::OutsideHistory outside (song);
        containerState.appendChild (lone.createCopy(), nullptr);
    }

    REQUIRE (phazite::getModulatorValue (song, ModulatorID::fromRaw (loneID)).has_value());
    CHECK (phazite::getModulatorValue (song, ModulatorID::fromRaw (loneID))->place.container == container);
    CHECK (holdsNoElement());
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' modulation: a set equal to what the song holds makes no undo step and tells nothing; the unsaved check "
           "agrees with the whole comparison after an edit and its Undo")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    const auto track = s.firstTrack;
    const auto lfo = s.addModulator (track, ModulatorType::lfo);
    const auto macro = s.addModulator (track, ModulatorType::macro);
    REQUIRE (project.save().wasOk());
    Told told;
    song.getValues()->addListener (&told);

    {
        const phazite::UndoStep step (song, "Nothing");
        CHECK_FALSE (phazite::setModulator (song, *phazite::getModulatorValue (song, lfo)));
        CHECK_FALSE (phazite::setAllModulation (song, phazite::getModulationValue (song)));
        CHECK_FALSE (phazite::setModulatorOrder (song, { .track = track }, { lfo, macro }));
        s.setMacroValue (macro, 0.0f);
        CHECK (song.getUndoManager().getNumActionsInCurrentTransaction() == 0);
    }

    CHECK (told.calls == 0);
    song.getValues()->removeListener (&told);

    // (The quick check asked only after both: once it finds an unsaved change, a save clears it, not an Undo.)
    s.setMacroValue (macro, 0.5f);
    CHECK_FALSE (project.isWholeAsSaved());
    phazite::undo (song);
    CHECK_FALSE (project.hasUnsavedChanges());
    CHECK (project.isWholeAsSaved());
}

TEST_CASE ("Song values' modulation: a version with other modulation applied each way, and the song as copied, which writes "
           "nothing")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto track = s.firstTrack;
    const auto lfo = s.addModulator (track, ModulatorType::lfo);
    s.modulate (lfo, s.getPanParameter (track), 0.5f);
    const auto container = phazite::addContainer (context, track);
    const auto macros = phazite::getContainerMacros (song, container);
    REQUIRE (macros.size() == 8);
    const auto version = phazite::test::copyWholeSong (song);

    {
        const phazite::UndoStep step (song, "Other Modulation");
        phazite::setValueSetByHand (song, valueOf (macros[3]), 0.75f);
        phazite::removeModulator (song, lfo);
        phazite::addModulator (song, track, ModulatorType::random);
    }

    const auto since = phazite::test::copyWholeSong (song);
    phazite::test::UndoCheck undo (context);

    for (const auto* other : { &version, &since, &version })
    {
        undo.checkSeveral ("Apply Version",
                           [&]
                           {
                               const phazite::UndoStep step (song, "Apply Version");
                               phazite::makeSongLike (song, *other);
                               phazite::removeBrokenReferences (song);
                           });
        CHECK (phazite::readModulationValue (*other) == phazite::getModulationValue (song));
        phazite::test::checkSong (song);
    }

    // The song as copied writes nothing (asserted in Debug builds: ApplySong.cpp), its modulation the value it was.
    const auto held = phazite::getModulationValue (song);
    phazite::makeSongAsCopied (song, phazite::test::copyWholeSong (song));
    CHECK (phazite::getModulationValue (song).isSameAs (held));
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' modulation: Swap's trial of a preset on a track with modulation, put back and kept, leaves its modulation "
           "as it was")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto synth = s.setInstrument (track, "phaziteSynth");
    const auto lfo = s.addModulator (track, ModulatorType::lfo);
    s.modulate (lfo, { synth, "cutoff" }, 0.5f);
    const auto macro = s.addModulator (track, ModulatorType::macro);
    s.modulate (macro, { synth, "resonance" }, 0.25f);
    const auto folder = s.getSoundsFolder();
    REQUIRE (phazite::saveDevicePreset (s.context, synth, folder.getChildFile ("Same.phazitepreset")));
    const auto held = phazite::getModulationValue (song);

    {
        phazite::Swap swap (s.context, synth);
        REQUIRE (swap.isOpen());
        CHECK (swap.tryFile (folder.getChildFile ("Same.phazitepreset")));
        CHECK (phazite::getModulationValue (song) == held);
        swap.putBack();
    }

    CHECK (phazite::getModulationValue (song) == held);

    {
        phazite::Swap swap (s.context, synth);
        CHECK (swap.tryFile (folder.getChildFile ("Same.phazitepreset")));
        swap.keep();
    }

    CHECK (phazite::getModulationValue (song) == held);
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' modulation: a learning device's removal heard by the MIDI linker's tree callback looks at no modulation")
{
    // The linker hears a device taken out of the tree (model/midi/internal/MidiLinks.h), and asks the tree alone whether the
    // parameter it learns is still there (Parameters.h: locateParameterInTree()): no tree callback reads an area. The device's
    // element taken out by hand, outside the history, so that only the tree's callbacks run meanwhile.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto lfo = s.addModulator (track, ModulatorType::lfo);
    s.modulate (lfo, s.getPanParameter (track), 0.5f);
    const auto reverb = s.addEffect (track, "phaziteReverb");
    phazite::startMidiLearn (phazite::getParameter (s.context, { reverb, "mix" }));
    REQUIRE (phazite::isLearningMidi (s.context));

    auto element = phazite::findDeviceState (song, reverb);
    REQUIRE (element.isValid());
    const auto looked = phazite::countModulationLooks();
    const auto read = phazite::countModulatorsRead();

    {
        const phazite::Song::OutsideHistory outside (song);
        element.getParent().removeChild (element, nullptr);
    }

    CHECK (phazite::countModulationLooks() == looked);
    CHECK (phazite::countModulatorsRead() == read);
    CHECK_FALSE (phazite::isLearningMidi (s.context)); // its parameter gone
}

TEST_CASE ("Song values' modulation: a container sent to another track by a line brought back keeps its macros, on that track")
{
    // The keeper's case (b) on another channel: a version whose container is on another track than in the song, the song's
    // track's line brought back (ApplySong.cpp, makeTrackLike(): sendItemsHome() moves the container's element there), its
    // macros taken out with it and given back on the channel it comes to, their places naming it.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    const auto container = phazite::addContainer (context, track);
    REQUIRE (container.isValid());
    const auto macros = phazite::getContainerMacros (song, container);
    REQUIRE (macros.size() == 8);
    s.setMacroValue (macros[1], 0.5f);

    // The version: the same song, its container (with its macros, as whole state writes them) on the other track.
    auto version = phazite::test::copyWholeSong (song);
    const std::function<juce::ValueTree (const juce::ValueTree&, phazite::TrackID)> findIn =
        [&findIn] (const juce::ValueTree& parent, phazite::TrackID id)
    {
        for (const auto& child : parent)
        {
            if (child.hasType (phazite::schema::TRACK) && (juce::int64) child[phazite::schema::trackID] == (juce::int64) id.getRaw())
                return child;

            if (const auto inside = findIn (child, id); inside.isValid())
                return inside;
        }

        return juce::ValueTree();
    };

    const auto tracks = version.getChildWithName (phazite::schema::TRACKS);
    auto from = findIn (tracks, track), to = findIn (tracks, other);
    auto moved = from.getChildWithProperty (phazite::schema::deviceID, (juce::int64) container.getRaw());
    REQUIRE (moved.hasType (phazite::schema::CONTAINER));
    REQUIRE (moved.getChildWithName (phazite::schema::MACRO).isValid());
    from.removeChild (moved, nullptr);
    to.addChild (moved, 0, nullptr);

    phazite::test::UndoCheck undo (context);
    undo.checkSeveral ("Restore Track",
                       [&]
                       {
                           const phazite::UndoStep step (song, "Restore Track");
                           REQUIRE (phazite::makeTrackLike (song, track, version));
                           phazite::removeBrokenReferences (song);
                       });

    REQUIRE (phazite::findDeviceState (song, container).getParent() == phazite::findTrackState (song, other));
    REQUIRE (phazite::getContainerMacros (song, container) == macros);

    for (const auto id : macros)
    {
        const auto value = phazite::getModulatorValue (song, id);
        REQUIRE (value.has_value());
        CHECK (value->place.track == other);
        CHECK (value->place.container == container);
    }

    CHECK (phazite::getModulatorValue (song, macros[1])->value == doctest::Approx (0.5f));
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' modulation: a track deleted and brought back by Undo, whose modulator is then removed, is moved into a "
           "folder without it")
{
    // The keeper remembers a deleted track's modulators with its element, and the Undo (which it leaves alone) brings them back
    // itself: what it remembered must not come back with the next move of the same element, once the modulator is gone.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    const auto lfo = s.addModulator (track, ModulatorType::lfo);
    s.modulate (lfo, s.getPanParameter (track), 0.5f);
    phazite::test::UndoCheck undo (context);

    undo.checkSeveral ("Delete Track", [&] { phazite::deleteTracks (song, { track }, nullptr); });
    undo.stepBackOrForth ("Undo Delete Track", [&] { phazite::undo (song); });
    REQUIRE (phazite::getModulatorValue (song, lfo).has_value());

    undo.checkSeveral ("Remove Modulator", [&] { phazite::removeModulator (song, lfo); });
    REQUIRE_FALSE (phazite::getModulatorValue (song, lfo).has_value());

    phazite::TrackID folder;
    undo.checkSeveral ("a folder", [&] { folder = phazite::insertFolder (song, other, nullptr); });
    undo.checkSeveral ("Into the Folder", [&] { phazite::moveTrackIntoFolder (song, track, folder); });
    REQUIRE (phazite::getParentFolder (song, track) == folder);
    CHECK_FALSE (phazite::getModulatorValue (song, lfo).has_value());
    CHECK (phazite::getModulators (song, track).empty());
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
    CHECK_FALSE (phazite::getModulatorValue (song, lfo).has_value());
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' modulation: a lone modulator added to a track in a step is refused")
{
    // As a lone macro is (the keeper's case (d)): the element can't be taken out in the step that adds it (JUCE workaround
    // W62, docs/juce-workarounds.md), so it is refused, the song's modulation as it was; its step's Undo takes it out again.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    s.addModulator (track, ModulatorType::lfo);
    const auto held = phazite::getModulationValue (song);
    auto trackState = phazite::findTrackState (song, track);
    juce::ValueTree lone (phazite::schema::MODULATOR);
    lone.setProperty (phazite::schema::modulatorID, (juce::int64) song.makeID(), nullptr);
    lone.setProperty (phazite::schema::modulatorType, "lfo", nullptr);
    REQUIRE (phazite::readModulatorKind (lone).has_value());

    {
        const phazite::test::ExpectedAssertion refused ("ModulationArea.cpp");
        const phazite::UndoStep step (song, "A Lone Modulator");
        trackState.appendChild (lone, &song.getUndoManager());
#if JUCE_DEBUG
        CHECK (refused.getCount() == 1);
#endif
    }

    CHECK (phazite::getModulationValue (song) == held);
    phazite::undo (song);
    CHECK_FALSE (trackState.getChildWithName (phazite::schema::MODULATOR).isValid());
    CHECK (phazite::getModulationValue (song) == held);
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' modulation: a track with a modulator and a container with macros deleted, undone and redone")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto track = s.firstTrack;
    s.addTrack ("Other");
    const auto lfo = s.addModulator (track, ModulatorType::lfo);
    s.modulate (lfo, s.getPanParameter (track), 0.5f);
    const auto container = phazite::addContainer (context, track);
    REQUIRE (container.isValid());
    const auto macros = phazite::getContainerMacros (song, container);
    REQUIRE (macros.size() == 8);
    s.setMacroValue (macros[0], 0.25f);
    const auto before = phazite::getModulationValue (song);
    phazite::test::UndoCheck undo (context);

    undo.checkSeveral ("Delete Track", [&] { phazite::deleteTracks (song, { track }, nullptr); });
    CHECK (phazite::getModulationValue (song).modulators.empty());
    phazite::test::checkSong (song);
    undo.stepBackOrForth ("Undo", [&] { phazite::undo (song); });
    CHECK (phazite::getModulationValue (song) == before);
    undo.stepBackOrForth ("Redo", [&] { phazite::redo (song); });
    CHECK (phazite::getModulationValue (song).modulators.empty());
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' modulation: a container removed whose device a track's modulator modulates takes that modulation along, "
           "in one step, and its Undo brings it back")
{
    // The container's element goes, its macros with it (the keeper's case (b)), and what names its device goes too (Mixer.h:
    // forgetWhatNamesTheGone(), the LFO's modulation of the reverb in it): the area set twice in the one step, what the step,
    // its Undo and its Redo told in all what differs.
    Watched w;
    auto& song = w.song;
    auto& context = w.s.context;
    const auto track = w.s.firstTrack;
    const auto lfo = w.s.addModulator (track, ModulatorType::lfo);
    w.s.modulate (lfo, w.s.getPanParameter (track), 0.5f);
    const auto container = phazite::addContainer (context, track);
    REQUIRE (container.isValid());
    phazite::addEffectToContainer (context, container, w.s.findDeviceType ("phaziteReverb"));
    const auto inside = phazite::getContainerDevices (context, container);
    REQUIRE (inside.size() == 1);
    w.s.modulate (lfo, { inside[0].id, "mix" }, 0.25f);
    REQUIRE (phazite::getModulations (song, lfo).size() == 2);
    const auto before = phazite::getModulationValue (song);

    const auto removed = w.hearEach ("Remove Container", [&] { phazite::removeDevice (song, container); });
    checkHeard ("Remove Container", removed, true, true);
    CHECK (toldAny (removed[0], lfo, Change::modulations));
    REQUIRE (phazite::getModulations (song, lfo).size() == 1); // after the Redo
    CHECK (phazite::getModulations (song, lfo)[0].parameter == w.s.getPanParameter (track));
    CHECK (phazite::getContainerMacros (song, container).empty());

    phazite::undo (song);
    CHECK (phazite::getModulationValue (song) == before);

    // One exact undo step, as the devices' view removes it.
    phazite::test::UndoCheck undo (context);
    undo.check ("Remove Container", [&] { phazite::removeDevice (context, container); });
    CHECK (undo.getNumSteps() == 1);
    CHECK (phazite::getModulations (song, lfo).size() == 1);
    phazite::test::checkSong (song);
}
