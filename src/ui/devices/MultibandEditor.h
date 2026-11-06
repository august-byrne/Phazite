// SPDX-License-Identifier: AGPL-3.0-or-later
// The multiband's editor (devices/dynamics/Multiband.h), with Maximus's parts and interactions (the owner, 2026-11-01: "the
// same layout, the same abilities ... all of the drag points should be the same (same interactions)"; its colours and
// drawing are Phazite's), laid out to fit the dock without scrolling (the owner, 2026-11-06):
//
// - The band tabs (Low, Mid, High, Master) with the selected band's Solo, which lead every page in the dock.
// - The selected band's compression curve, in the band's colour, as Maximus's envelope editor draws and edits it
//   (DynamicsCurve.h, which the compressor's editor shares): level in across against level out up, lit up to the level the
//   band's compressor hears; under it, its menu, Freeze and Snap.
// - The band's mode (On, Comp Off, Muted, Off) and stereo separation; its pre and post gains, its saturation and ceiling.
// - The analysis display: Monitor, the last seconds of the selected band (what comes in, what goes out, what its compressor
//   hears, its gain and its pre and post levels, each shown as its Traces menu says, every band's gain too), scrolling at the
//   speed set, paused by a click; or Bands, the three bands across the spectrum in their colours, as high as their pre gains,
//   the spectrum of what it plays over them: drag a band's edge for its crossover, a band up or down for its pre gain. A
//   crossover or pre gain changed while Monitor shows brings Bands up for a moment.
// - The band's attack, release, sustain and second release, its curve boxes and peak or RMS.
// - The crossovers with their slopes; LMH Delay and LMH Mix, the low cut and the mid band driving the master.
//
// In a window of its own, or a tall dock, the curve, the band's controls and the display across, the rest under them, as
// Maximus lays them out; in the dock, as many parts on each page as fit beside the band tabs. Which band shows, the
// display's view and its traces, Freeze and Snap are the editor's for the session, not the project's.
#pragma once

#include "DeviceEditorBase.h"
#include "DynamicsCurve.h"

#include <array>
#include <optional>
#include <vector>

namespace phazite::ui
{
class MultibandEditor : public DeviceEditorBase,
                        private DisplayTicker::Display
{
public:
    explicit MultibandEditor (const DeviceHandle&);
    ~MultibandEditor() override;

    // The band whose curve and controls show (Multiband::Band), as its tab picks it.
    int getSelectedBand() const noexcept { return selectedBand; }
    void selectBand (int band);
    juce::Button& getBandButton (int band);

    // The selected band's curve, and the analysis display.
    DynamicsCurve& getCurve() const noexcept;
    juce::Component& getDisplay() const noexcept;

    // The display's views: Bands (true) or Monitor; and where Bands draws a frequency and a band's pre gain, in its own
    // coordinates.
    bool isShowingBands() const;
    void showBands (bool);
    float getBandsX (double frequency) const;
    float getBandsY (float decibels) const;

    // Takes the meters into the display, shows the selected band's controls and repaints what moved; the ticker does it.
    void refresh();

    // The menu of the traces Monitor shows, each ticked while it shows; an item switches it.
    juce::PopupMenu getTracesMenu();

private:
    class Analysis;
    class BandPart;
    class BandsPart;
    class CurvePart;
    class DisplayPart;

    int selectedBand = 3;                    // the master at first, as Maximus opens on it
    std::vector<ParameterHandle> parameters; // every parameter, in the multiband's order, for the curves and the bands
    std::vector<float> values;
    std::array<float, 4> shownHeard { -120.0f, -120.0f, -120.0f, -120.0f }; // what each band's compressor hears, in decibels
    DrawnNumbers drawnCurve;
    DynamicsCurve* curve = nullptr;
    Analysis* analysis = nullptr;
    BandsPart* bandTabs = nullptr;
    DisplayPart* display = nullptr;
    std::vector<BandPart*> bandParts; // those that show the selected band's controls

    void showBandControls();

    void tick() override { refresh(); }
};
} // namespace phazite::ui
