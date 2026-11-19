// SPDX-License-Identifier: AGPL-3.0-or-later
// The compressor's editor (roadmap steps 1.6 and S6.3, devices/dynamics/Compressor.h): three tabs along its top, Compress,
// Duck and Transient, which are its modes, as Fruity Limiter's LIMIT and COMP tabs are its own (the owner, 2026-11-11:
// "should the line graph even co-exist in the same effect as sidechaining"; the research found no transfer curve helps
// while ducking, so the curve and the ducking don't share a page: Compress shows the curve, Duck shows time). A click on a
// tab sets the mode, as one undo step, and shows its page; each fits the default dock on one page. Each shows its history
// (LevelHistoryView.h), the last two bars at the song's tempo with its beats and bars, a reading every half millisecond from
// the compressor's trail (devices/core/LevelTrail.h), and how far it turns the sound down in red from the top (Transient's,
// up and down about the middle).
//
// - Compress, kept plain for a quick compressor (the owner, 2026-11-01): its characters as a row of buttons; its curve, drawn
//   and edited as the multiband's are, as Maximus's (DynamicsCurve.h), lit up to the level it hears, with its menu's shapes
//   to start from, and a meter beside it of how far it turns the sound down (from its middle down) or up (from its middle up);
//   beside them its history, what comes in pale and what goes out in its colour, the pairing of Maximus's curve and Fruity
//   Limiter's display the owner finds useful (with a key, the key as Duck's page shows it); under the history its attack,
//   release, hold and look-ahead, its input gain, drive and makeup, Auto Makeup's switch by the curve's tools, and the key
//   it hears with its cuts and Listen, Duck's Sidechain part; where the dock is too short for the three one over another,
//   they show under tabs, the knobs first.
// - Duck, for sidechain pumping: its key, the track it listens to, picked there (SidechainMenus.h; given the project), over
//   the key's cuts and Listen, which plays the key instead of the sound while the editor shows; beside it the history as its
//   graph, with no curve: the key filled in its track's colour and named, its threshold a line across it to drag, and how far
//   it ducks the sound on the levels' own scale; under it its threshold, depth, attack, hold, release and release curve.
// - Transient: its history, with how far it turns the sound up and down as a line about the middle, beside its punch and
//   sustain, large, over its speed, makeup and low cut.
//
// Its mix, for parallel compression, is its row's in the Devices view.
#pragma once

#include "DeviceEditorBase.h"
#include "DynamicsCurve.h"
#include "LevelHistoryView.h"
#include "devices/core/LevelTrail.h"

#include <memory>
#include <vector>

namespace phazite
{
class ProjectContext;
}

namespace phazite::ui
{
class SidechainButton;

class CompressorEditor : public DeviceEditorBase,
                         private DisplayTicker::Display
{
public:
    // Given the project and the device's ID, it has its key's picker too.
    explicit CompressorEditor (const DeviceHandle&, ProjectContext* = nullptr, DeviceID = {});
    ~CompressorEditor() override;

    // Its modes' tabs, by the mode (Compressor::Mode: Compress, Duck, then Transient), and whether Duck's page shows, or
    // Transient's.
    juce::Button& getModeTab (int mode);
    bool isDucking() const noexcept;
    bool isShapingTransients() const noexcept;

    DynamicsCurve& getCurve();
    LevelHistoryView& getHistory();
    juce::Button& getListenButton(); // plays the key instead of the sound, while on and showing
    juce::Button& getCharacterButton (int character);
    SidechainButton* getKeyPicker() const noexcept { return keyPicker; } // nullptr without the project

    // What the meters show: the level the detector hears, and how far the sound goes down and up (0 or more), in decibels.
    float getShownLevel() const noexcept { return shownLevel; }
    float getShownReduction() const noexcept { return shownReduction; }
    float getShownBoost() const noexcept { return shownBoost; }

    // Brings the curve, the meter, the history, the key and the character's buttons up to date, and keeps the key playing
    // while Listen is on and Duck's page shows; the ticker does it.
    void refresh();

private:
    class CurvePart;
    class KeyPart;

    ProjectContext* const context;
    std::vector<ParameterHandle> parameters; // every parameter, in the compressor's order, for the curve
    std::vector<float> values;
    ParameterHandle threshold; // Duck's, which the history's line drags
    float shownLevel, shownReduction = 0.0f, shownBoost = 0.0f;
    std::vector<devices::LevelReading> pulled;           // from the compressor's trail, at each refresh,
    std::vector<LevelHistoryView::Levels> historyLevels; // and as its history takes them
    int emptyRefreshes = 0;                              // refreshes in a row that brought no reading
    static constexpr int refreshesToSilence = 4;         // after which the device is taken to sleep
    DrawnNumbers drawnCurve;                             // what its curve and meter show
    int shownSongVersion = -1;
    bool knobsChosen = false; // Compress's knobs shown first, where its column shows its parts under tabs

    CurvePart* curvePart = nullptr;
    ChoiceButtons* characters = nullptr;
    KeyPart* keyPart = nullptr;
    SidechainButton* keyPicker = nullptr;
    LevelHistoryView* history = nullptr;

    void modeShown (int) override;
    void showKey(); // the key's track in the picker, and in the history while it ducks
    void tick() override { refresh(); }
    void shownAgain() override; // its meters drained, its history empty
};
} // namespace phazite::ui
