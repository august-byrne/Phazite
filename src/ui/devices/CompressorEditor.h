// SPDX-License-Identifier: AGPL-3.0-or-later
// The compressor's editor (roadmap step 1.6, devices/dynamics/Compressor.h): two tabs along its top, Compress and Duck, which
// are its modes, as Fruity Limiter's LIMIT and COMP tabs are its own (the owner, 2026-11-11: "should the line graph even
// co-exist in the same effect as sidechaining"; the research found no transfer curve helps while ducking, so the curve and
// the ducking don't share a page: Compress shows the curve, Duck shows time). A click on a tab sets the mode, as one undo
// step, and shows its page; each fits the default dock on one page. Both show its history (LevelHistoryView.h), the last
// two bars at the song's tempo with its beats and bars, and how far it turns the sound down in red from the top.
//
// - Compress, kept plain for a quick compressor (the owner, 2026-11-01): its characters as a row of buttons; its curve, drawn
//   and edited as the multiband's are, as Maximus's (DynamicsCurve.h), lit up to the level it hears, with its menu's shapes
//   to start from, and a meter beside it of how far it turns the sound down (from its middle down) or up (from its middle up);
//   beside them its history, what comes in pale and what goes out in its colour, the pairing of Maximus's curve and Fruity
//   Limiter's display the owner finds useful; under the history its attack and release, its input gain, drive and makeup,
//   and the cuts of what it hears.
// - Duck, for sidechain pumping: its key, the track it listens to, picked there (SidechainMenus.h; given the project), over
//   the key's cuts and Listen, which plays the key instead of the sound while the editor shows; beside it the history as its
//   graph, with no curve: the key filled in its track's colour and named, its threshold a line across it to drag, and how far
//   it ducks the sound on the levels' own scale; under it its threshold, depth, attack, hold, release and release curve.
//
// Its mix, for parallel compression, is its row's in the Devices view.
#pragma once

#include "DeviceEditorBase.h"
#include "DynamicsCurve.h"
#include "LevelHistoryView.h"

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

    // Its modes' tabs, by the mode (Compressor::Mode: Compress, then Duck), and whether Duck's page shows.
    juce::Button& getModeTab (int mode);
    bool isDucking() const noexcept;

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
    DrawnNumbers drawnCurve; // what its curve and meter show
    int shownSongVersion = -1;

    CurvePart* curvePart = nullptr;
    ChoiceButtons* characters = nullptr;
    KeyPart* keyPart = nullptr;
    SidechainButton* keyPicker = nullptr;
    LevelHistoryView* history = nullptr;

    void modeShown (int) override;
    void showKey(); // the key's track in the picker, and in the history while it ducks
    void tick() override { refresh(); }
};
} // namespace phazite::ui
