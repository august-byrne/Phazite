// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/NoteElements.h"
#include "model/base/internal/SongIndex.h"

namespace phazite
{
int readChannel (const juce::ValueTree& element, const juce::Identifier& property)
{
    return juce::jlimit (1, 16, (int) element.getProperty (property, 1));
}

void writeChannel (juce::ValueTree& element, const juce::Identifier& property, int channel, juce::UndoManager* undo)
{
    if (channel = juce::jlimit (1, 16, channel); channel == 1)
        element.removeProperty (property, undo);
    else
        element.setProperty (property, channel, undo);
}

//==============================================================================
NoteElement findNote (const Song& song, const juce::ValueTree& clipState, NoteID id)
{
    return { InPlace(), [&] { return song.getIndex().findNote (clipState, id.getRaw()); } };
}

NoteEdit editNote (Song& song, const juce::ValueTree& clipState, NoteID id)
{
    return { InPlace(), [&] { return song.getIndex().findNote (clipState, id.getRaw()); }, &song.getUndoManager() };
}

void appendNote (juce::ValueTree& clipState, const NoteValues& values, juce::UndoManager* undo)
{
    juce::ValueTree note (schema::NOTE);
    note.setProperty (schema::noteID, (juce::int64) values.id.getRaw(), nullptr);
    note.setProperty (schema::notePitch, values.pitch, nullptr);
    note.setProperty (schema::noteStart, values.start.inBeats(), nullptr);
    note.setProperty (schema::noteLength, values.length.inBeats(), nullptr);
    note.setProperty (schema::noteVelocity, values.velocity, nullptr);

    for (const auto& [property, on] : { std::pair { &schema::noteMute, values.muted }, std::pair { &schema::noteSlide, values.slide },
                                        std::pair { &schema::notePortamento, values.portamento } })
        if (on)
            note.setProperty (*property, true, nullptr);

    writeChannel (note, schema::noteChannel, values.channel, nullptr);
    clipState.appendChild (note, undo);
}

void removeNote (juce::ValueTree& clipState, const NoteElement& note, juce::UndoManager* undo)
{
    clipState.removeChild (note.state, undo);
}

void removeAllNotes (juce::ValueTree& clipState, juce::UndoManager* undo)
{
    for (int i = clipState.getNumChildren(); --i >= 0;)
        if (clipState.getChild (i).hasType (schema::NOTE))
            clipState.removeChild (i, undo);
}

//==============================================================================
const char* getSchemaKind (ControlKind kind) noexcept
{
    switch (kind)
    {
        case ControlKind::controller:
            return schema::controlIsController;
        case ControlKind::pitchWheel:
            return schema::controlIsPitchWheel;
        case ControlKind::channelPressure:
            return schema::controlIsChannelPressure;
        case ControlKind::aftertouch:
            return schema::controlIsAftertouch;
        case ControlKind::programChange:
            return schema::controlIsProgramChange;
    }

    return schema::controlIsController;
}

std::optional<ControlKind> ControlElement::getKind() const
{
    const auto kind = state[schema::controlKind].toString();

    for (const auto each : { ControlKind::controller, ControlKind::pitchWheel, ControlKind::channelPressure, ControlKind::aftertouch,
                             ControlKind::programChange })
        if (kind == getSchemaKind (each))
            return each;

    return std::nullopt;
}

ControlElement makeControl (const ControlValues& values)
{
    juce::ValueTree control (schema::CONTROL);
    control.setProperty (schema::controlKind, getSchemaKind (values.kind), nullptr);

    if (hasNumber (values.kind))
        control.setProperty (schema::controlNumber, juce::jlimit (0, 127, values.number), nullptr);

    control.setProperty (schema::controlBeat, std::max (0.0, values.beat), nullptr);
    control.setProperty (schema::controlValue, juce::jlimit (0, getHighestValue (values.kind), values.value), nullptr);
    writeChannel (control, schema::controlChannel, values.channel, nullptr);
    return ControlElement (control);
}

void appendControl (juce::ValueTree& clipState, const ControlElement& control, juce::UndoManager* undo)
{
    clipState.appendChild (control.state, undo);
}

ControlElement getControlAt (const juce::ValueTree& clipState, int place)
{
    if (ControlElement control (InPlace(), [&] { return clipState.getChild (place); }); control.isControl())
        return control;

    return {};
}
} // namespace phazite
