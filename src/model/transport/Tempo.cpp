// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Tempo.h"
#include "model/commands/Undo.h"

#include <cmath>

namespace phazite
{
double getTempo (const te::Edit& edit)
{
    return edit.tempoSequence.getTempo (0)->getBpm();
}

void setTempo (te::Edit& edit, double bpm)
{
    const auto rounded = std::round (juce::jlimit (minTempo, maxTempo, bpm) * 100.0) / 100.0;
    auto* tempo = edit.tempoSequence.getTempo (0);

    if (juce::approximatelyEqual (tempo->getBpm(), rounded))
        return;

    const UndoStep step (edit, "Change Tempo");
    tempo->setBpm (rounded); // Tracktion moves the clips with their beats
}

TimeSignature getTimeSignature (const te::Edit& edit)
{
    const auto* timeSig = edit.tempoSequence.getTimeSig (0);
    return { timeSig->numerator.get(), timeSig->denominator.get() };
}

bool isValid (TimeSignature timeSignature)
{
    const auto denominator = timeSignature.denominator;
    return timeSignature.numerator >= 1 && timeSignature.numerator <= 32
           && (denominator == 2 || denominator == 4 || denominator == 8 || denominator == 16);
}

void setTimeSignature (te::Edit& edit, TimeSignature timeSignature)
{
    if (! isValid (timeSignature) || timeSignature == getTimeSignature (edit))
        return;

    const UndoStep step (edit, "Change Time Signature");
    auto* timeSig = edit.tempoSequence.getTimeSig (0);
    timeSig->numerator = timeSignature.numerator;
    timeSig->denominator = timeSignature.denominator;
}
} // namespace phazite
