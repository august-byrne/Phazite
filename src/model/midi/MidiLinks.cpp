// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/MidiLinks.h"
#include "devices/core/Clock.h"
#include "model/automation/AutomationValues.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Parameters.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/versions/internal/AreaClashes.h"

#include <algorithm>
#include <cmath>

namespace phazite
{
namespace
{
    juce::ValueTree findLinksState (const Song& song)
    {
        return song.getState().getChildWithName (schema::MIDILINKS);
    }

    // The song's tree written as the song is written now: in the step in progress, or outside the history.
    juce::UndoManager* getTreeUndo (Song& song)
    {
        return song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();
    }

    // The song's links made the next value: their area's, in the step in progress, or outside the history while the song is
    // written outside it (SongValues.h: setMidiLinks(), which writes nothing for the links the song holds already).
    void setLinks (Song& song, MidiLinksValue next)
    {
        if (auto* values = song.getValues())
            values->setMidiLinks (song, std::move (next));
        else
            jassertfalse; // the MIDI links' edits are a song's that holds its values (internal/Project.h: makeSong())
    }

    // The links without those that one asks to take away, in their order: the same value where it takes none.
    template <typename TakesAway>
    MidiLinksValue without (const MidiLinksValue& value, TakesAway&& takesAway)
    {
        auto kept = immer::flex_vector<MidiLinkEntry>().transient();
        auto took = false;

        for (const auto& link : value.links)
        {
            if (takesAway (link))
                took = true;
            else
                kept.push_back (link);
        }

        return took ? MidiLinksValue { kept.persistent() } : value;
    }

    // Whether an element of another song's, or anything in it, stands in for what that song's version couldn't read
    // (Schema.h: schema::versions::missing).
    bool holdsStandIn (const juce::ValueTree& element)
    {
        if (element.hasProperty (schema::versions::missing))
            return true;

        for (const auto& child : element)
            if (holdsStandIn (child))
                return true;

        return false;
    }

    // How a merge names a link in its clash's path (SongMerge.cpp: keyOf()): its type and its key, its controller.
    juce::String nameInPath (const MidiLinkEntry& link)
    {
        return schema::MIDILINK.toString() + ":" + schema::getElementKey (writeMidiLinkElement (link));
    }

    // How a moved controller's link is told: "CC 21 on channel 1 now turns Cutoff, instead of Volume."
    juce::String describeLinkMade (MidiController controller, const juce::String& turns, const juce::String& before)
    {
        return describeMidiControllerInText (controller) + " now turns " + turns + (before.isNotEmpty() ? ", instead of " + before : "")
               + ".";
    }

    // Every linker, for findMidiLinker(). Message thread only.
    std::vector<MidiLinker*>& getLinkers()
    {
        static std::vector<MidiLinker*> linkers;
        return linkers;
    }
} // namespace

//==============================================================================
MidiLinksValue getMidiLinksValue (const Song& song)
{
    if (const auto* values = song.getValues())
        return values->midiLinks.get();

    return readMidiLinksValue (findLinksState (song));
}

ParameterRef readMidiLinkParameter (const Song& song, const MidiLinkEntry& link)
{
    const auto device = DeviceID::fromRaw (link.device);

    // A macro's one parameter has the macro's ID (Parameters.h), which its link doesn't repeat.
    if (link.parameter == schema::midiLinkMacroValue
        && findModulatorState (song, ModulatorID::fromRaw (device.getRaw())).hasType (schema::MACRO))
        return { device, juce::String (device.getRaw()) };

    return { device, link.parameter };
}

ParameterRef readMidiLinkParameter (const Song& song, const juce::ValueTree& link)
{
    return readMidiLinkParameter (song, readMidiLinkEntry (link));
}

std::vector<MidiLink> getMidiLinks (const Song& song)
{
    const auto value = getMidiLinksValue (song);
    std::vector<MidiLink> links;
    links.reserve (value.links.size());

    for (const auto& link : value.links)
        links.push_back ({ { link.channel, link.number }, readMidiLinkParameter (song, link) });

    std::stable_sort (links.begin(), links.end(), [] (const MidiLink& a, const MidiLink& b) { return a.controller < b.controller; });
    return links;
}

std::optional<MidiController> findMidiLink (const Song& song, const ParameterRef& parameter)
{
    const auto value = getMidiLinksValue (song);

    for (const auto& link : value.links)
        if (readMidiLinkParameter (song, link) == parameter)
            return MidiController { link.channel, link.number };

    return {};
}

std::optional<ParameterRef> findLinkedParameter (const Song& song, MidiController controller)
{
    const auto value = getMidiLinksValue (song);

    for (const auto& link : value.links)
        if (MidiController { link.channel, link.number } == controller)
            return readMidiLinkParameter (song, link);

    return {};
}

std::optional<ParameterRef> linkMidiController (Song& song, MidiController controller, const ParameterRef& parameter)
{
    if (! controller.isValid() || ! parameter.isValid())
        return {};

    if (song.getValues() == nullptr)
    {
        jassertfalse; // the MIDI links' edits are a song's that holds its values (model/project/internal/Project.h: makeSong())
        return {};
    }

    const auto before = findLinkedParameter (song, controller);

    // The controller's link moves here, and the parameter's own goes: one link of each, the new one after the others.
    const auto found = locateParameter (song, parameter);
    const auto isMacro = found.has_value() && found->source == ParameterSource::macro;
    const auto next =
        without (getMidiLinksValue (song), [&] (const MidiLinkEntry& link)
                 { return MidiController { link.channel, link.number } == controller || readMidiLinkParameter (song, link) == parameter; });

    // The song's MIDILINKS first, where it has none: the place of its links, added with the first.
    if (! findLinksState (song).isValid())
        song.getState().appendChild (juce::ValueTree (schema::MIDILINKS), getTreeUndo (song));

    setLinks (song, { next.links.push_back ({ controller.channel, controller.number, parameter.device.getRaw(),
                                              isMacro ? juce::String (schema::midiLinkMacroValue) : parameter.parameter }) });

    return before.has_value() && *before != parameter ? before : std::nullopt;
}

void unlinkMidiController (Song& song, MidiController controller)
{
    setLinks (song, without (getMidiLinksValue (song), [controller] (const MidiLinkEntry& link)
                             { return MidiController { link.channel, link.number } == controller; }));
}

void forgetMidiLinksOfTheGone (Song& song, const std::set<std::uint64_t>& items)
{
    setLinks (song, keepOneLinkEach (without (getMidiLinksValue (song),
                                              [&items] (const MidiLinkEntry& link) { return ! items.contains (link.device); })));
}

void keepOneMidiLinkEach (Song& song)
{
    // The rule the song's value keeps (MidiLinksValue.h: keepOneLinkEach()), on the elements as the loader reads them before
    // anything else does: those the rule keeps are those it reads, in their order, less those it lets go (a link equal to one
    // kept before it went: so each kept is the first element alike that is left). A song that holds its values keeps the rule
    // in its value, its MIDILINKS empty: nothing.
    auto links = findLinksState (song);
    const auto kept = keepOneLinkEach (readMidiLinksValue (links));
    size_t next = 0;

    for (int i = 0; i < links.getNumChildren();)
    {
        if (const auto link = links.getChild (i); link.hasType (schema::MIDILINK))
        {
            if (next < kept.links.size() && readMidiLinkEntry (link) == kept.links[next])
            {
                ++next;
            }
            else
            {
                links.removeChild (i, nullptr);
                continue;
            }
        }

        ++i;
    }
}

bool makeMidiLinksAs (Song& song, const juce::ValueTree& links)
{
    auto* values = song.getValues();

    if (values == nullptr || (links.isValid() && ! links.hasType (schema::MIDILINKS)))
        return false;

    // Its element first where it comes, its value, then its element away where it goes: the value never says more than the
    // element does (a song without a MIDILINKS holds no links).
    auto state = song.getState();
    const auto live = findLinksState (song);
    const auto before = values->midiLinks.get();
    auto wrote = false;

    if (links.isValid() && ! live.isValid())
    {
        state.appendChild (juce::ValueTree (schema::MIDILINKS), getTreeUndo (song));
        wrote = true;
    }

    values->setMidiLinks (song, links.isValid() ? readHeldMidiLinks (links) : MidiLinksValue {});
    wrote = wrote || ! values->midiLinks.get().isSameAs (before);

    if (! links.isValid() && live.isValid())
    {
        state.removeChild (live, getTreeUndo (song));
        wrote = true;
    }

    return wrote;
}

AreaChoice chooseMidiLinkClash (Song& song, const MergeClash& clash, int value, const MergeSides&)
{
    // A path into the song's MIDILINKS, whose links the song holds as a value beside its tree (model/base/internal/
    // SongValues.h), its tree's element an empty placeholder: a link merges whole (SongMerge.cpp: mergesWhole()), so its clash
    // is the whole link's, by its controller, the path's second step. The side chosen's link put in the place of the song's
    // link of that controller, or after the others where it has none; or the song's taken away where that side had none: as
    // the song's tree had it chosen, in the choice's step (VersionsModel.cpp: chooseClash(), which then keeps one link of
    // each parameter: forgetWhatNamesTheGone()). Refused (nothing written) for any other clash on its path, which a merge of
    // two songs Phazite holds never makes, but a commit's song may come from anyone (tests/FuzzTests.cpp chooses every clash
    // of a damaged merge): a property, the MIDILINKS itself, an element of another type, a link the song can't hold, one that
    // stands in for what its version couldn't read, and a song without a MIDILINKS (where the tree had no place to choose in).
    if (clash.path.isEmpty() || ! clash.path[0].startsWith (schema::MIDILINKS.toString() + ":"))
        return AreaChoice::notTheArea;

    const juce::ValueTree* elements[] = { &clash.baseElement, &clash.oursElement, &clash.theirsElement };

    if (clash.property.isValid() || clash.path.size() != 2 || ! clash.path[1].startsWith (schema::MIDILINK.toString() + ":")
        || ! juce::isPositiveAndBelow (value, 3) || song.getValues() == nullptr || ! findLinksState (song).isValid())
        return AreaChoice::refused;

    const auto& chosen = *elements[value];
    const auto link = chosen.isValid() ? readHeldMidiLink (chosen) : std::nullopt;

    if (chosen.isValid() && (! link.has_value() || holdsStandIn (chosen)))
        return AreaChoice::refused;

    const auto links = getMidiLinksValue (song);
    auto place = links.links.size();

    for (size_t i = 0; i < links.links.size(); ++i)
    {
        if (nameInPath (links.links[i]) == clash.path[1])
        {
            place = i;
            break;
        }
    }

    auto next = place < links.links.size() ? links.links.erase (place) : links.links;

    if (link.has_value())
        next = next.insert (juce::jmin (place, next.size()), *link);

    setLinks (song, { next });
    return AreaChoice::chosen;
}

std::optional<float> getValueAtControllerPosition (const Song& song, const ParameterRef& parameter, int position)
{
    const auto travel = getControlTravel (song, parameter);

    if (! travel.has_value())
        return {};

    const auto along = (float) juce::jlimit (0, 127, position) / 127.0f;

    // A parameter that steps: its steps share the travel evenly (a switch on from 64, as halfway is 63.5).
    if (travel->step > 0.0f)
    {
        const auto steps = juce::roundToInt ((travel->maximum - travel->minimum) / travel->step) + 1;

        if (steps >= 2)
        {
            const auto index = juce::jmin (steps - 1, (int) std::floor (along * (float) steps));
            return juce::jlimit (travel->minimum, travel->maximum, travel->minimum + (float) index * travel->step);
        }
    }

    return fromControlPosition (*travel, along);
}

//==============================================================================
MidiLinker::MidiLinker (Song& s)
    : song (&s),
      state (s.getState())
{
    state.addListener (this);
    getLinkers().push_back (this);
}

MidiLinker::~MidiLinker()
{
    std::erase (getLinkers(), this);
    onChange = nullptr; // what it would tell may be going too
    state.removeListener (this);
    stopTimer();
    endBurst();

    if (learning.has_value())
        setLearning (std::nullopt);
}

void MidiLinker::startLearning (const ParameterRef& parameter)
{
    if (! locateParameter (*song, parameter).has_value())
        return;

    linkMade.reset();
    setLearning (parameter);
}

void MidiLinker::cancelLearning()
{
    setLearning (std::nullopt);
}

void MidiLinker::setLearning (std::optional<ParameterRef> parameter)
{
    const auto changed = learning != parameter;
    learning = std::move (parameter);

    if (auto* session = findSession (*song))
        session->setMidiLearning (learning.has_value());

    if (changed && onChange != nullptr)
        onChange();
}

void MidiLinker::take (MidiController controller, int value)
{
    if (! controller.isValid())
        return;

    if (learning.has_value())
    {
        learn (controller);
        return;
    }

    if (const auto parameter = findLinkedParameter (*song, controller))
        if (const auto set = getValueAtControllerPosition (*song, *parameter, value))
            turn (*parameter, *set);
}

void MidiLinker::learn (MidiController controller)
{
    const auto parameter = *learning;

    if (! locateParameter (*song, parameter).has_value())
    {
        setLearning (std::nullopt);
        return;
    }

    // Named before the link moves: what the controller turned is still there.
    const auto turns = getParameterName (*song, parameter);
    const auto before = findLinkedParameter (*song, controller);
    const auto beforeName = before.has_value() && *before != parameter ? getParameterName (*song, *before) : juce::String();

    // Linked before learning ends, which the window hears: the controller is diverted from the link's step on (the session
    // follows the song's links at once, not at the next turn of the message loop, as it does for any other change of them:
    // PlaybackSession.h, followMidiLinks()), so that what the knob sends meanwhile turns the parameter, never the instrument.
    {
        const UndoStep step (*song, "Link MIDI Controller");
        linkMidiController (*song, controller, parameter);
    }

    if (auto* session = findSession (*song))
        session->followMidiLinks();

    linkMade = describeLinkMade (controller, turns, beforeName);
    setLearning (std::nullopt);
}

void MidiLinker::turn (const ParameterRef& parameter, float value)
{
    // During a control's drag, in the drag's step, as another control moved by it would be.
    if (Gesture::isInProgress (*song))
    {
        setValueSetByHand (*song, parameter, value);
        return;
    }

    // A burst kept meanwhile (something else began a step) has ended; another begins.
    if (burst != nullptr && ! burst->isOpen())
    {
        burst = nullptr;
        inBurst.clear();
    }

    if (burst == nullptr)
    {
        burstName = getChangeName (*song, parameter); // as its control's change is named
        burst = std::make_unique<Trial> (*song, burstName, [this] { finishWriting(); });
    }

    const auto first = std::find (inBurst.begin(), inBurst.end(), parameter) == inBurst.end();

    if (first)
        inBurst.push_back (parameter);

    burst->apply (
        [this, &parameter, value, first]
        {
            // Turned as a control's drag turns it: punched in as it is grabbed, where its track writes automation.
            if (auto* session = findPlayingSession (*song); session != nullptr && first)
                session->getWriter().gestureBegan (parameter);

            setValueSetByHand (*song, parameter, value);
        });

    lastMove = devices::Clock::getMilliseconds();

    if (! isTimerRunning())
        startTimer (restCheckMilliseconds);
}

bool MidiLinker::isInBurst() const
{
    return burst != nullptr && burst->isOpen() && burst->hasChanges();
}

bool MidiLinker::isBurstOpen() const
{
    return burst != nullptr && burst->isOpen();
}

void MidiLinker::finishWriting()
{
    // What its passes wrote, let go of as a control is: in its step, where it changed something. A burst that changed
    // nothing begins no step for it: during a control's drag (which kept it, empty, as it began), what they wrote joins the
    // drag's step, as a control moved by the drag; otherwise nothing was written, and the writer's holds are let go of.
    const auto parameters = std::exchange (inBurst, {});
    stopTimer();

    auto* session = findPlayingSession (*song);

    if (session == nullptr || parameters.empty())
        return;

    auto& writer = session->getWriter();

    if (burst != nullptr && burst->hasChanges())
    {
        burst->apply (
            [&writer, &parameters]
            {
                for (const auto& parameter : parameters)
                    writer.gestureEnded (parameter);
            });
    }
    else if (Gesture::isInProgress (*song))
    {
        for (const auto& parameter : parameters)
            writer.gestureEnded (parameter);
    }
    else
    {
        for (const auto& parameter : parameters)
            writer.gestureLetGo (parameter);
    }
}

void MidiLinker::endBurst()
{
    if (burst == nullptr)
        return;

    finishWriting();

    if (burst->isOpen())
        burst->keep (burstName);

    burst = nullptr;
}

void MidiLinker::timerCallback()
{
    if (burst == nullptr || ! burst->isOpen())
    {
        burst = nullptr;
        inBurst.clear();
        stopTimer();
        return;
    }

    if (devices::Clock::getMilliseconds() - lastMove >= restMilliseconds)
        endBurst();
}

void MidiLinker::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int)
{
    // Learning ends as its parameter goes: a device, a chain, a send, a fader, a modulator or a macro, each still an element of
    // the song's tree, so the linker stays a listener of the tree for them (the links themselves are a value: MidiLinks.h).
    // It reads no area of the song's values here (locateParameter() reads the tree's elements).
    if (learning.has_value() && ! locateParameter (*song, *learning).has_value())
        setLearning (std::nullopt);
}

MidiLinker* findMidiLinker (const Song& song)
{
    for (auto* linker : getLinkers())
        if (&linker->getSong() == &song)
            return linker;

    return nullptr;
}
} // namespace phazite
