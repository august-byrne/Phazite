// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Mirror.h"
#include "model/transport/internal/Tempo.h"

namespace phazite::mirror
{
void reconcileTempoMap (te::Edit& edit)
{
    auto& sequence = edit.tempoSequence;
    const auto tempos = getTempoChanges (edit);

    while (sequence.getNumTempos() > (int) tempos.size())
        sequence.removeTempo (sequence.getNumTempos() - 1, false);

    for (int i = 0; i < (int) tempos.size(); ++i)
    {
        const auto& change = tempos[(size_t) i];
        const auto beat = toTracktion (change.beat);

        if (i >= sequence.getNumTempos())
        {
            sequence.insertTempo (beat, change.bpm, 0.0f);
            continue;
        }

        auto* tempo = sequence.getTempo (i);

        if (tempo->getStartBeat() != beat)
            tempo->setStartBeat (beat);

        // Through setBpm(), which keeps Tracktion's clips and automation on their beats.
        if (! juce::exactlyEqual (tempo->getBpm(), change.bpm))
            tempo->setBpm (change.bpm);
    }

    const auto signatures = getTimeSignatureChanges (edit);

    while (sequence.getNumTimeSigs() > (int) signatures.size())
        sequence.removeTimeSig (sequence.getNumTimeSigs() - 1);

    for (int i = 0; i < (int) signatures.size(); ++i)
    {
        const auto& change = signatures[(size_t) i];
        auto* timeSig = i < sequence.getNumTimeSigs() ? sequence.getTimeSig (i) : sequence.insertTimeSig (toTracktion (change.beat)).get();

        if (timeSig == nullptr)
            continue;

        if (timeSig->getStartBeat() != toTracktion (change.beat))
            timeSig->startBeatNumber = toTracktion (change.beat);

        if (timeSig->numerator.get() != change.signature.numerator)
            timeSig->numerator = change.signature.numerator;

        if (timeSig->denominator.get() != change.signature.denominator)
            timeSig->denominator = change.signature.denominator;
    }

    // Where the clips and the automation are in seconds follows the tempo.
    reconcileClips (edit);
    reconcileAutomation (edit);
}
} // namespace phazite::mirror
