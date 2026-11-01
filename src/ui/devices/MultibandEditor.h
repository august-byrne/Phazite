// SPDX-License-Identifier: AGPL-3.0-or-later
// The multiband's editor (devices/dynamics/Multiband.h), in Maximus's layout (the owner, 2026-11-01: "the same layout, the
// same abilities ... all of the drag points should be the same (same interactions)"; its colours and drawing are Phazite's):
//
// - Top left, the selected band's compression curve, level in across against level out up, in the band's colour, its
//   points dots and the tension handles between them hollow, as Maximus's envelope editor draws them: drag a point (it snaps
//   to the grid while Snap is on; Shift keeps it level, Ctrl keeps it where it is across); drag a tension handle up or down
//   to bend the line before it (Ctrl in fine steps), right-click a handle to straighten it; right-click a point for its line's
//   shape (single or double curve, hold, stairs, smooth stairs) or to delete it, Alt+click deletes it too; right-click
//   elsewhere to add a point there. Under it, its menu (shapes to start from, copy and paste, flip, smooth, reset), Freeze
//   (no editing) and Snap.
// - Top right, the analysis display: Monitor, the last seconds of the selected band (what comes in, what goes out, what its
//   compressor hears, its gain and its pre and post levels, each shown by its switch below, every band's gain too), scrolling
//   at the speed set, paused by a click; or Bands, the three bands across the spectrum in their colours, as high as their pre
//   gains, the spectrum of what it plays over them: drag a band's edge for its crossover, a band up or down for its pre gain.
//   A crossover or pre gain changed while Monitor shows brings Bands up for a moment.
// - Below, the band tabs (Low, Mid, High, Master) with Solo; the selected band's mode (On, Comp Off, Muted, Off) and stereo
//   separation; its pre and post gains; its saturation and ceiling; its attack, release and sustain, its curve boxes, peak
//   or RMS, and its second release; then LMH Delay and LMH Mix; and the crossovers with their slopes, the low cut and the
//   mid band driving the master.
//
// Which band shows, the display's view and its switches, Freeze and Snap are the editor's for the session, not the project's.
#pragma once

#include "DeviceEditorBase.h"

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

    juce::Component& getCurve() const noexcept;
    juce::Component& getDisplay() const noexcept;

    // Where the curve shows a place across and up its axes (from 0 to 1), a point of the selected band's curve, and the handle
    // that bends the line before it (none for the first point), in the curve's own coordinates.
    juce::Point<float> getCurvePosition (float across, float up) const;
    juce::Point<float> getPointPosition (int point) const;
    std::optional<juce::Point<float>> getTensionPosition (int point) const;

    // What the curve's menus do, each one undo step: a line's shape before a point (Multiband::PointType), deleting a point,
    // and a shape to start from (by its name in the curve's menu).
    void setPointType (int point, int type);
    void deletePoint (int point);
    bool applyShape (const juce::String& name);
    static juce::StringArray getShapeNames();

    // The display's views: Bands (true) or Monitor; and where Bands draws a frequency and a band's pre gain, in its own
    // coordinates.
    bool isShowingBands() const;
    void showBands (bool);
    float getBandsX (double frequency) const;
    float getBandsY (float decibels) const;

    // Takes the meters into the display, shows the selected band's controls and repaints what moved; the ticker does it.
    void refresh();

private:
    class Curve;
    class Analysis;
    class Panel;

    int selectedBand = 3;                    // the master at first, as Maximus opens on it
    std::vector<ParameterHandle> parameters; // every parameter, in the multiband's order, for the curves and the bands
    std::vector<float> values;
    std::array<float, 4> shownHeard { -120.0f, -120.0f, -120.0f, -120.0f }; // what each band's compressor hears, in decibels
    DrawnNumbers drawnCurve;
    Curve* curve = nullptr;
    Analysis* analysis = nullptr;
    Panel* panel = nullptr;

    void tick() override { refresh(); }
};
} // namespace phazite::ui
