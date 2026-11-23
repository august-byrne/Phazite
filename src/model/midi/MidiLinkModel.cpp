// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MidiLinkModel.h"
#include "internal/MidiLinks.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/ParameterHandle.h"
#include "model/devices/internal/ParameterHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace phazite
{
namespace
{
    MidiLinker* findLinker (const ProjectContext& context)
    {
        auto* song = findSong (context);
        return song != nullptr ? findMidiLinker (*song) : nullptr;
    }

    MidiLinker* findLinker (const ParameterHandle& handle)
    {
        auto* song = findSong (handle);
        return song != nullptr ? findMidiLinker (*song) : nullptr;
    }
} // namespace

std::vector<MidiLinkInfo> getMidiLinks (const ProjectContext& context)
{
    std::vector<MidiLinkInfo> infos;
    const auto* song = findSong (context);

    if (song == nullptr)
        return infos;

    for (const auto& link : getMidiLinks (*song))
    {
        MidiLinkInfo info { link.controller, {}, {}, {} };
        juce::StringArray names;

        if (const auto found = locateParameter (*song, link.parameter))
        {
            // A modulator's or a macro's track by its value, and a send's by its ID in the sends' area (the FoundParameter of
            // either holds no element of it).
            const auto isModulation = found->source == ParameterSource::modulator || found->source == ParameterSource::macro;
            info.track = isModulation ? getModulatorTrack (*song, ModulatorID::fromRaw (link.parameter.device.getRaw()))
                         : found->source == ParameterSource::send ? getSendTrack (*song, link.parameter.device)
                                                                  : getParameterTrack (*song, link.parameter);

            if (holdsDevice (*song, link.parameter.device))
                info.device = link.parameter.device;
            else if (const auto container = getChainContainer (*song, link.parameter.device); container.isValid())
                info.device = container;

            if (info.track.isValid())
                names.add (getTrackName (*song, info.track));

            // A macro by its own name alone; a modulator's parameter after the modulator's.
            if (found->source == ParameterSource::modulator)
                names.add (getModulatorName (*song, ModulatorID::fromRaw (link.parameter.device.getRaw())));
            else if (found->source != ParameterSource::macro)
                names.add (getParameterDeviceName (*song, link.parameter));

            names.add (getParameterName (*song, link.parameter));
        }
        else
        {
            names.add (link.parameter.parameter); // a plug-in's while it doesn't load
        }

        names.removeEmptyStrings();
        info.turns = names.joinIntoString (": ");
        infos.push_back (std::move (info));
    }

    return infos;
}

std::optional<MidiController> getMidiLink (const ParameterHandle& handle)
{
    const auto* song = findSong (handle);
    const auto parameter = findParameterRef (handle);
    return song != nullptr && parameter.isValid() ? findMidiLink (*song, parameter) : std::nullopt;
}

bool canLearnMidi (const ParameterHandle& handle)
{
    return findLinker (handle) != nullptr && findParameterRef (handle).isValid();
}

void startMidiLearn (const ParameterHandle& handle)
{
    if (auto* linker = findLinker (handle); linker != nullptr && canLearnMidi (handle))
        linker->startLearning (findParameterRef (handle));
}

void cancelMidiLearn (ProjectContext& context)
{
    if (auto* linker = findLinker (context))
        linker->cancelLearning();
}

void cancelMidiLearn (const ParameterHandle& handle)
{
    if (auto* linker = findLinker (handle); linker != nullptr && isLearningMidi (handle))
        linker->cancelLearning();
}

bool isLearningMidi (const ParameterHandle& handle)
{
    const auto* linker = findLinker (handle);
    const auto learning = linker != nullptr ? linker->getLearning() : std::nullopt;
    return learning.has_value() && *learning == findParameterRef (handle);
}

bool isLearningMidi (const ProjectContext& context)
{
    const auto* linker = findLinker (context);
    return linker != nullptr && linker->getLearning().has_value();
}

std::optional<juce::String> getMidiLearnName (const ProjectContext& context)
{
    const auto* linker = findLinker (context);
    const auto learning = linker != nullptr ? linker->getLearning() : std::nullopt;
    return learning.has_value() ? std::optional (getParameterName (linker->getSong(), *learning)) : std::nullopt;
}

std::optional<juce::String> takeMidiLinkMade (ProjectContext& context)
{
    auto* linker = findLinker (context);
    return linker != nullptr ? linker->takeLinkMade() : std::nullopt;
}

void removeMidiLink (const ParameterHandle& handle)
{
    if (const auto controller = getMidiLink (handle))
    {
        auto& song = *findSong (handle);
        const UndoStep step (song, "Remove MIDI Link");
        unlinkMidiController (song, *controller);
    }
}

void removeMidiLink (ProjectContext& context, MidiController controller)
{
    if (auto* song = findSong (context); song != nullptr && findLinkedParameter (*song, controller).has_value())
    {
        const UndoStep step (*song, "Remove MIDI Link");
        unlinkMidiController (*song, controller);
    }
}

//==============================================================================
bool canLearnLastTouched (const ProjectContext& context)
{
    return isLearningMidi (context) || (findLinker (context) != nullptr && getLastTouchedParameter (context).has_value());
}

void learnLastTouched (ProjectContext& context)
{
    auto* linker = findLinker (context);

    if (linker == nullptr)
        return;

    if (linker->getLearning().has_value())
        linker->cancelLearning();
    else if (const auto parameter = getLastTouchedParameter (context))
        linker->startLearning (*parameter);
}

juce::String describeLearnLastTouched (const ProjectContext& context)
{
    if (isLearningMidi (context))
        return "Cancel MIDI Learn";

    if (const auto parameter = getLastTouchedParameter (context))
        return "MIDI Learn " + getParameterName (*findSong (context), *parameter);

    return "MIDI Learn Last Touched Parameter";
}

void addMidiLinkCommands (CommandRegistry& registry, ProjectContext& context)
{
    // No undo target: learning changes nothing itself. The link is made, as its own step, when a controller comes.
    registry.add ({ .id = "midi.learnLastTouched",
                    .name = "MIDI Learn Last Touched Parameter",
                    .category = "MIDI",
                    .description = "Links the next knob or fader moved on a MIDI controller to the parameter changed last.",
                    .synonyms = { "map", "assign", "controller", "cc", "knob", "remote", "midi learn" },
                    .perform = [&context] { learnLastTouched (context); },
                    .isEnabled = [&context] { return canLearnLastTouched (context); },
                    .currentName = [&context] { return describeLearnLastTouched (context); },
                    .overPlugins = true });
}
} // namespace phazite
