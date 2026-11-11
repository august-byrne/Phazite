// SPDX-License-Identifier: AGPL-3.0-or-later
// The last two bars of a dynamics device's sound, at the tempo where the song's playhead is, scrolling from right to
// left (docs/design.md, section 9), as FabFilter's compressor and limiter scroll theirs; with what the device listens
// to on the same time line as what it does to the sound, as Kickstart and ShaperBox draw the kick under what it ducks,
// so that each hit shows the dip it causes and whether the sound comes back before the next one (the owner,
// 2026-11-10): how loud the sound comes in (pale) and goes out (in the device's colour, over it), so that where it is
// turned down what goes out dips under what came in; and how far it is turned down (red, hanging from the top, its
// deepest dips numbered), or, while the device listens to another track through its sidechain, its key, that track as a
// line in its colour, named; and faint lines at the time signature's beats, stronger ones at the bars, where the song
// was as it played, which scroll with the sound, so that a kick on the beat lands on a line. It always shows two bars:
// it has no speed to set. A line across at a level the device sets (a limiter's ceiling, a ducking compressor's
// threshold) moves with a drag up or down near it, as one undo step: there the pointer becomes an up-and-down arrow,
// the line lights up, and the hint bar says what a drag does. Its editor pushes a reading each tick.
#pragma once

#include "DeviceGraph.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/ParameterHandle.h"
#include "ui/components/DisplayTicker.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <deque>
#include <functional>
#include <vector>

namespace phazite::ui
{
class LevelHistoryView : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    // Levels from lowestLevel to 0 dB; reductions down to reductionRange dB, from the top.
    LevelHistoryView (const juce::String& title, float lowestLevel, float reductionRange);

    using SongTime = DeviceHandle::SongTime;
    static constexpr double barsShown = 2.0;

    // A tick's reading, taken at a moment (milliseconds, by juce::Time::getMillisecondCounterHiRes()) with where the song was
    // then (DeviceHandle::getSongTime()), in decibels: the loudest in, the loudest out, how far it was turned down (0 or
    // more), and, while it listens to another track (setKey()), that track's loudest, as its detector hears it.
    void push (double milliseconds, const SongTime&, float input, float output, float reduction, float key = -200.0f);

    // How long it shows across its width: two bars at the tempo where the playhead was at the newest reading.
    double getSpanMilliseconds() const noexcept { return span; }

    // Where it draws a moment, in its own coordinates: the newest reading's at its right edge.
    float getXForTime (double milliseconds) const;

    // The beat and bar lines it shows now, oldest first, each at the moment the song passed it.
    struct Line
    {
        double milliseconds = 0.0;
        bool bar = false;
    };

    std::vector<Line> getLines() const;

    // The line across: its parameter (the caller's handle, which must outlive this or be taken away first), named
    // ("Ceiling"), or none.
    void setLine (ParameterHandle*, const juce::String& name);
    bool hasLine() const noexcept { return line != nullptr; }
    bool isLineLit() const noexcept { return lineLit || dragging; } // under the pointer, or dragged

    // How far it turns down shows from the top: down to this many decibels over this share of its height (at first its
    // reduction range over half of it). Over all of it, as far as its lowest level, it reads on the levels' scale.
    void setReductionScale (float decibels, float shareOfHeight);

    // The track the device listens to through its sidechain (its key), named and in its colour; an empty name for none. With
    // one, it shows the sound it ducks, what came in, pale, and what went out over it in the device's colour, each duck a dip
    // under what came in, and the key as a line in its colour beside the threshold's, as Kickstart and ShaperBox draw the
    // kick over what it ducks (the owner, 2026-11-11: the key with how far it turned down, red from the top, "just shows
    // the duck envelope mirrored"). Without one, how far it turned down hangs from the top in red, as Pro-C's does.
    void setKey (const juce::String& name, juce::Colour);
    juce::String getKeyName() const { return keyName; }
    bool showsReduction() const noexcept { return keyName.isEmpty(); }

    // The newest reading's reduction, as shown.
    float getNewestReduction() const;

    float getYForLevel (float decibels) const; // in its own coordinates

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    struct Reading
    {
        double milliseconds = 0.0;
        float input = 0.0f, output = 0.0f, reduction = 0.0f, key = -200.0f;
    };

    const float lowest;
    float reductionRange, reductionShare = 0.5f;
    std::deque<Reading> readings; // oldest first: those in view, and the one before them that their traces start from
    std::deque<Line> lines;
    SongTime lastSong;
    double span = 4000.0;
    int pushes = 0;
    DrawnNumbers drawn;
    ParameterHandle* line = nullptr;
    juce::String lineName, keyName;
    juce::Colour keyColour;
    bool lineLit = false, dragging = false;
    float startValue = 0.0f;

    void describe(); // for screen readers: what it shows, and how its line moves
    graph::LevelAxis getLevels() const;
    void addLines (double fromMilliseconds, double toMilliseconds, const SongTime&);
    bool isFlat() const; // nothing in view but silence, without lines: a picture that doesn't move
    bool isNearLine (float y) const;
    void setLineLit (bool);
    juce::Path makeTrace (float Reading::* value, const std::function<float (float)>& yFor) const; // through its readings
};
} // namespace phazite::ui
