// SPDX-License-Identifier: AGPL-3.0-or-later
// The multiband's editor (devices/dynamics/Multiband.h), with Maximus's parts and interactions (the owner, 2026-11-01: "the
// same layout, the same abilities ... all of the drag points should be the same (same interactions)"; its colours and
// drawing are Phazite's), on one page in the default dock, with no pages to turn (the owner, 2026-11-10: "the multiband
// compressor can't be separating the knobs for the frequency bands/graph from the graph by putting it on a separate tab.
// you must figure out how to put everything on one page"). Two parts, side by side:
//
// - Bands: the three bands across the spectrum, each as high as its level (its Post), in its colour, the spectrum of what
//   the multiband plays over them, the crossovers' slopes (12 or 24 dB) in its top corners: drag a crossover or the low
//   cut's edge left or right, a band up or down for its level; click a band to show its curve and controls. Under the graph,
//   always with it, its knobs: the low cut, the crossovers and LMH Delay; the Low, Mid and High levels and LMH Mix. A level
//   dragged in the graph and its knob are the same parameter.
// - Band: the band tabs (Low, Mid, High, Master), one band at a time; the band's curve, as Maximus's envelope editor draws
//   and edits it (DynamicsCurve.h, the compressor's too), lit up to the level its compressor hears, its menu over it; its
//   mode; its Solo and Key (the master's Key and Mid Drives); and its knobs: Pre, Stereo, Sat and Ceiling; Attack, Release,
//   Sustain and Release 2; the curve boxes, Peak or RMS, and the master's Post. Beside them where the part is wide enough,
//   Monitor (the band's last seconds, what it hears among them: the key, in its track's colour and named, while its Key is
//   on), else in their place while its switch is on; and the Sidechain part (the key's picker, its cuts and Listen), else
//   in the band's place by the Sidechain tab.
//
// Everything under the pointer that a drag moves says so: the pointer shows which way it moves, it lights up, and its
// tooltip, which the hint bar shows, says what it does (the owner, 2026-11-10). Which band shows, Monitor's switch and
// traces, the Sidechain tab, Freeze, Snap and Listen are the editor's for the session, not the project's.
#pragma once

#include "DeviceEditorBase.h"
#include "DynamicsCurve.h"
#include "model/base/IDs.h"
#include "model/mixer/SidechainChoice.h"

#include <array>
#include <vector>

namespace phazite
{
class ProjectContext;
}

namespace phazite::ui
{
class SidechainButton;

class MultibandEditor : public DeviceEditorBase,
                        private DisplayTicker::Display
{
public:
    // With the open project's context and the device's ID, its Sidechain part has the key's picker (SidechainMenus.h);
    // without them, it hasn't.
    explicit MultibandEditor (const DeviceHandle&, ProjectContext* = nullptr, DeviceID = {});
    ~MultibandEditor() override;

    // The band whose curve and controls show (Multiband::Band), as its tab picks it, or a click on it in the bands' graph.
    int getSelectedBand() const noexcept { return selectedBand; }
    void selectBand (int band);
    juce::Button& getBandButton (int band);

    // The selected band's curve.
    DynamicsCurve& getCurve() const noexcept;

    // The bands' graph; and where it draws a frequency and a band's level, in its own coordinates.
    juce::Component& getBandsGraph() const noexcept;
    float getBandsX (double frequency) const;
    float getBandsY (float decibels) const;

    // Monitor, and its switch, which shows it in place of the band's controls where it can't show beside them (then
    // hidden); whether it shows. And the track it names while the selected band hears the key through it.
    juce::Component& getMonitor() const noexcept;
    juce::Button& getMonitorButton();
    bool isMonitorShowing() const;
    juce::String getMonitorKeyName() const;

    // The Sidechain part, and its tab, which shows it in place of the band where it can't show beside it (then hidden);
    // whether it shows. Its key's picker (nullptr without a context) and Listen.
    juce::Component& getSidechainPart() const noexcept;
    juce::Button& getSidechainTab();
    bool isSidechainShowing() const;
    SidechainButton* getSidechainButton() const noexcept;
    juce::Button& getListenButton();

    // Takes the meters into Monitor and the spectrum into the bands' graph, shows the key's track, keeps the key playing while
    // Listen is on and shows, and repaints what moved; the ticker does it.
    void refresh();

    // The menu of the traces Monitor shows, each ticked while it shows; an item switches it.
    juce::PopupMenu getTracesMenu();

private:
    class Analysis;
    class MonitorPart;
    class BandsGraph;
    class BandsPart;
    class SidechainPart;
    class BandPanel;

    int selectedBand = 3;                    // the master at first, as Maximus opens on it
    std::vector<ParameterHandle> parameters; // every parameter, in the multiband's order, for the curves and the bands
    std::vector<float> values;
    std::array<float, 4> shownHeard { -120.0f, -120.0f, -120.0f, -120.0f }; // what each band's compressor hears, in decibels
    DrawnNumbers drawnValues;
    SidechainChoice shownSource; // the track the key comes from, as shown

    DynamicsCurve* curve = nullptr;
    Analysis* analysis = nullptr;
    BandsGraph* bandsGraph = nullptr;
    SidechainPart* sidechain = nullptr;
    BandPanel* bandPanel = nullptr;

    juce::String describeListeners() const; // which bands hear the key
    void tick() override { refresh(); }
};
} // namespace phazite::ui
