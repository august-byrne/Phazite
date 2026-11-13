// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SongPlaceText.h"
#include "model/arrangement/TimelineModel.h"

#include <algorithm>
#include <cmath>

namespace phazite::ui
{
namespace
{
    constexpr double edge = 1.0e-9; // what rounding leaves short of a whole beat or sixteenth still counts as it

    // A count of beats to a hundredth, without zeros after its point: "1", "1.5", "0.25".
    juce::String formatBeats (double beats)
    {
        auto text = juce::String (std::round (beats * 100.0) / 100.0, 2);

        if (text.containsChar ('.'))
            text = text.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");

        return text;
    }
} // namespace

juce::String formatBarPosition (const ProjectContext& context, BeatPosition place)
{
    const auto bar = getBarAt (context, place);
    const auto barStart = getBarStart (context, bar);
    const auto beatLength = getBeatLength (context, barStart).inBeats();
    const auto intoBar = std::max (0.0, (place - barStart).inBeats());
    const auto beat = (int) std::floor (intoBar / beatLength + edge);
    const auto intoBeat = std::max (0.0, intoBar - beat * beatLength);
    const auto sixteenth = (int) std::floor (intoBeat / sixteenthBeats + edge);
    return juce::String (bar + 1) + "." + juce::String (beat + 1) + "." + juce::String (sixteenth + 1);
}

std::optional<BeatPosition> parseBarPosition (const ProjectContext& context, const juce::String& text)
{
    auto parts = juce::StringArray::fromTokens (text.retainCharacters ("0123456789. ").replaceCharacter (' ', '.'), ".", {});
    parts.removeEmptyStrings();

    if (parts.isEmpty() || parts[0].getIntValue() < 1)
        return {};

    const auto barStart = getBarStart (context, parts[0].getIntValue() - 1);
    const auto beat = parts.size() > 1 ? std::max (1, parts[1].getIntValue()) : 1;
    const auto sixteenths = parts.size() > 2 ? std::max (1, parts[2].getIntValue()) : 1;
    return barStart + getBeatLength (context, barStart) * (double) (beat - 1) + BeatDuration::fromBeats (sixteenthBeats * (sixteenths - 1));
}

juce::String formatSongLength (const ProjectContext& context, BeatPosition start, BeatDuration length)
{
    const auto barLength = getBarLength (context, start).inBeats();
    const auto beatLength = getBeatLength (context, start).inBeats();
    const auto beats = std::max (0.0, length.inBeats());
    const auto bars = barLength > 0.0 ? (int) std::floor (beats / barLength + edge) : 0;
    const auto rest = beatLength > 0.0 ? std::max (0.0, beats - bars * barLength) / beatLength : 0.0;
    const auto restText = formatBeats (rest);

    juce::StringArray parts;

    if (bars > 0)
        parts.add (juce::String (bars) + (bars == 1 ? " bar" : " bars"));

    if (restText != "0" || bars == 0)
        parts.add (restText + (restText == "1" ? " beat" : " beats"));

    return parts.joinIntoString (" ");
}
} // namespace phazite::ui
