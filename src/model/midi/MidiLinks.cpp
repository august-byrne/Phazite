// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/MidiLinks.h"
#include "devices/core/Clock.h"
#include "model/automation/AutomationValues.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Parameters.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"

#include <algorithm>
#include <cmath>

namespace phazite
{
namespace
{
    MidiController readController (const juce::ValueTree& link)
    {
        return { (int) link[schema::midiLinkChannel], (int) link[schema::midiLinkNumber] };
    }

    std::vector<juce::ValueTree> getLinkStates (const Song& song)
    {
        std::vector<juce::ValueTree> links;

        for (const auto& link : song.getState().getChildWithName (schema::MIDILINKS))
            if (link.hasType (schema::MIDILINK))
                links.push_back (link);

        return links;
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
    return readMidiLinksValue (song.getState().getChildWithName (schema::MIDILINKS));
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

    auto* undo = &song.getUndoManager();
    const auto before = findLinkedParameter (song, controller);

    // The controller's link moves here, and the parameter's own goes: one link of each.
    for (auto link : getLinkStates (song))
        if (readController (link) == controller || readMidiLinkParameter (song, link) == parameter)
            link.getParent().removeChild (link, undo);

    const auto found = locateParameter (song, parameter);
    const auto isMacro = found.has_value() && found->source == ParameterSource::macro;

    juce::ValueTree link (schema::MIDILINK);
    link.setProperty (schema::midiLinkChannel, controller.channel, nullptr);
    link.setProperty (schema::midiLinkNumber, controller.number, nullptr);
    link.setProperty (schema::midiLinkDevice, juce::String ((juce::int64) parameter.device.getRaw()), nullptr);
    link.setProperty (schema::midiLinkParameter, isMacro ? juce::String (schema::midiLinkMacroValue) : parameter.parameter, nullptr);
    song.getState().getOrCreateChildWithName (schema::MIDILINKS, undo).appendChild (link, undo);

    return before.has_value() && *before != parameter ? before : std::nullopt;
}

void unlinkMidiController (Song& song, MidiController controller)
{
    for (auto link : getLinkStates (song))
        if (readController (link) == controller)
            link.getParent().removeChild (link, &song.getUndoManager());
}

void forgetMidiLinksOfTheGone (Song& song, const std::set<std::uint64_t>& items)
{
    for (auto link : getLinkStates (song))
        if (! items.contains (readID (link[schema::midiLinkDevice])))
            link.getParent().removeChild (link, &song.getUndoManager());

    keepOneMidiLinkEach (song, &song.getUndoManager());
}

void keepOneMidiLinkEach (Song& song, juce::UndoManager* undo)
{
    // By what the elements hold, as the loader reads them before anything else does.
    std::set<std::pair<int, int>> controllers;
    std::set<std::pair<std::uint64_t, juce::String>> parameters;

    for (auto link : getLinkStates (song))
    {
        const auto controller = readController (link);
        const std::pair<int, int> byController { controller.channel, controller.number };
        const std::pair<std::uint64_t, juce::String> byParameter { readID (link[schema::midiLinkDevice]),
                                                                   link[schema::midiLinkParameter].toString() };

        if (controllers.contains (byController) || parameters.contains (byParameter))
        {
            link.getParent().removeChild (link, undo);
            continue;
        }

        controllers.insert (byController);
        parameters.insert (byParameter);
    }
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
    // hears the song change), so that what the knob sends meanwhile turns the parameter, never the instrument.
    {
        const UndoStep step (*song, "Link MIDI Controller");
        linkMidiController (*song, controller, parameter);
    }

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
    // Learning ends as its parameter goes.
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
