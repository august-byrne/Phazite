// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's Motion (roadmap step S6.10): the effects that sweep the sound with an LFO, as one device in three modes, each
// played by an engine of its own, whose DSP is the device it was before (Chorus.h, Flanger.h, Phaser.h). Chorus: copies of
// the sound a few milliseconds behind, their delays swept, up to eight a side. Flanger: one copy, its delay swept down from
// the Delay, mixed with the sound, a comb of notches sliding through it. Phaser: the sound through all-pass filters whose
// notches the LFO sweeps around a frequency. They share the LFO (devices/dsp/SyncedLfo.h), Feedback (-100 to +100 %: as far
// as each mode's engine goes), Stereo and Safe Bass (SafeBass.h); a mode ignores the parameters it doesn't use. Its values
// map to the engine's own (getChorusValues(), getFlangerValues(), getPhaserValues()), which its editor's pictures use too,
// so that there is one mapping; the tests hold the mapping to each engine's values written out by hand.
// One meaning of Mix in every mode: what it puts out is the effect alone, which its runner blends with the sound, half and
// half at 50 % (its default), as the chorus's copies are. The flanger and the phaser put out the sound and their copy
// already mixed, y = (x + copy) / 2, so in those modes it puts out 2 y - x, x the dry copy its runner mixes back in (after
// Safe Bass's split, ProcessContext::dry): at 50 % Mix the runner plays 0.5 x + 0.5 (2 y - x) = y, what the flanger and
// the phaser played at 100 % of theirs.
// A change of mode fades the effect out over 10 ms, toward the sound as it came in (what it puts out and the dry copy alike,
// so that at the bottom of the fade the runner plays the sound untouched, at any mix), resets the new mode's engine and
// fades that in over 10 ms, as the reverb changes its mode: each engine fed the sound faded as it fades, so that no sound
// cut in at full level clicks in its lines, and what it plays taken down again only near the fade's bottom, so that the
// level holds through the fade (processStretch()). Counted in samples, so that it plays the same in blocks of any size. No
// latency.
#pragma once

#include "Chorus.h"
#include "Flanger.h"
#include "Phaser.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"

#include <array>
#include <atomic>
#include <span>
#include <vector>

namespace phazite::devices
{
class Motion final : public Processor
{
public:
    static constexpr const char* typeID = "phaziteMotion";

    enum Parameter
    {
        mode,      // Chorus, Flanger or Phaser
        delay,     // milliseconds: the middle of a chorus's sweep, the top of a flanger's
        frequency, // the phaser's
        depth,
        sync,
        division, // a note value (devices/dsp/NoteValues.h)
        rate,
        shape,    // sine or triangle
        feedback, // -1 to 1: as far as the mode's engine goes
        notches,  // the phaser's: a choice, 1 to 6
        spacing,  // the phaser's
        highCut,  // the chorus's
        voices,   // the chorus's: a choice, 1 to 8
        voicePhase,
        spread,
        shimmer,
        stereo,
        safeBass,
        mix // applied by its runner (DeviceInfo.h, mixParameterID)
    };

    enum class Mode
    {
        chorus,
        flanger,
        phaser
    };

    static const DeviceInfo info;

    // How far each mode's engine takes the Feedback at its most (100 %).
    static constexpr float chorusFeedback = 0.9f, flangerFeedback = 1.0f, phaserFeedback = 0.85f;

    // The mode its values pick.
    static Mode getMode (std::span<const float> parameters);

    // Its values as each engine's own (in the order and ranges of the engine's DeviceInfo), the mix among them: the chorus's
    // as Motion's, the flanger's and the phaser's twice it, as they mixed the sound in themselves (above). No engine reads its
    // mix as Motion plays it (Motion's runner applies Motion's), and the flanger's and the phaser's pictures draw Motion's
    // Mix (getFlangerMagnitude(), getPhaserMagnitude()); the engine's mix is for an engine run alone through a runner, as the
    // tests run it to compare with Motion, which then plays the Mix of the device it was. The chorus's picture reads the
    // chorus's mix, which is Motion's (Chorus::getMagnitude()).
    static std::array<float, Chorus::mix + 1> getChorusValues (std::span<const float> parameters);
    static std::array<float, Flanger::mix + 1> getFlangerValues (std::span<const float> parameters);
    static std::array<float, Phaser::mix + 1> getPhaserValues (std::span<const float> parameters);

    // What its runner plays of the sound at a frequency, as a gain, in Flanger or Phaser mode at its values, the Mix among
    // them: the engine's response H (Flanger::getResponse() with the copy lagging so many milliseconds, Phaser::getResponse()
    // with the notches swept to centre) and the dry copy D mixed back in as the one meaning of Mix has it, |(1 - 2 m) D +
    // 2 m H|, D the turn Safe Bass's split gives the dry copy (1 while it is off). At 50 % that is |H|; at 100 % the phaser
    // without feedback is flat, its all-pass sections alone. For its editor's pictures and tests.
    static double getFlangerMagnitude (std::span<const float> parameters, double milliseconds, double frequency, double sampleRate);
    static double getPhaserMagnitude (std::span<const float> parameters, double centre, double frequency, double sampleRate);

    // How deep the flanger's notches are as heard at the Mix, from 0 to 1: where the copy is opposite the sound its response
    // is 0, so what is heard there is the dry copy alone, |1 - 2 m| of it (at any feedback, Safe Bass aside). Whole at 50 %,
    // the sound and its copy cancelling; half at 25 and 75 %; none at 100 %, the copy alone, which has no notches.
    static float getFlangerNotchDepth (std::span<const float> parameters) noexcept;

    // Its engines, for its editor's pictures (where each one's sweep is) and tests.
    const Chorus& getChorus() const noexcept { return chorus; }
    const Flanger& getFlanger() const noexcept { return flanger; }
    const Phaser& getPhaser() const noexcept { return phaser; }

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() override;
    void process (const ProcessContext&) override;
    double getTailSeconds() const override; // the tail of the mode that plays

private:
    Chorus chorus;
    Flanger flanger;
    Phaser phaser;
    std::array<float, Chorus::mix + 1> chorusValues {};
    std::array<float, Flanger::mix + 1> flangerValues {};
    std::array<float, Phaser::mix + 1> phaserValues {};

    double sampleRate = 48000.0;
    int capacity = 0; // the longest stretch it plays at once: its scratch buffers' length
    // A stretch's sound and dry copy as they came in, while it fades; its dry copy where its runner hands none; and the
    // fade's gain at each of its samples.
    std::vector<float> asItCame, ownDry, gains;

    // The mode that plays (-1 until the first block, which takes the mode at once), and how far it has faded in, in samples
    // of fadeLength.
    int playing = -1;
    std::atomic<int> shownMode { 0 };
    int fadeLength = 480, faded = 480;

    Processor& getEngine (int) noexcept;
    const Processor& getEngine (int) const noexcept;

    // A stretch of the block, of at most capacity samples, in the mode that plays: its engine's, as the mix's one meaning
    // has it, faded as it fades in or out.
    void processStretch (const ProcessContext&, int start, int length, int numChannels, bool fadesOut);

    static_assert (std::atomic<int>::is_always_lock_free);
};
} // namespace phazite::devices
