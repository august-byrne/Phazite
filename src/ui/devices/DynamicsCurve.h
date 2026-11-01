// SPDX-License-Identifier: AGPL-3.0-or-later
// A dynamics device's curve (devices/dynamics/DynamicsCurve.h), as Maximus's envelope editor shows and edits it, for the
// multiband's bands and the compressor (the owner, 2026-11-01: "i want this graph to also be used the same in the
// compressor plugin"): level in across, level out up, over a grid at Maximus's levels with the 0 dB lines through the
// middle and the line where level in equals level out. The area under the curve is shaded, and lit, as Maximus lights it,
// up to the level its compressor hears now, so that it moves with the sound. Its points are dots and the handles in the
// middle of its lines hollow:
//
// - Drag a point; Shift keeps it level, Ctrl keeps it where it is across, and with Snap on it goes onto a grid line near it.
//   The first and last points stay at the curve's ends.
// - Drag a handle up or down to bend its line (Ctrl in fine steps); right-click it to straighten the line.
// - Right-click a point for its line's shape (single or double curve, hold, stairs, smooth stairs) or to delete it; Alt and
//   a click deletes it too. Right-click elsewhere to add a point there.
//
// Each change is one undo step. Its tools go under it (DynamicsCurveTools): its menu (shapes to start from, copy and paste,
// from any device's curve to any other's, flip, smooth, reset), Freeze (no editing) and Snap, the editor's for the session.
#pragma once

#include "devices/dynamics/DynamicsCurve.h"
#include "model/devices/DeviceHandle.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <vector>

namespace phazite::ui
{
class DynamicsCurve : public juce::Component
{
public:
    // The device's curve whose parameters (its count of points, then its points: devices::curves) start at the index set,
    // read from its editor's values, every parameter's in the device's order, which its editor brings up to date: onEdit
    // asks it to, after each change the curve makes.
    DynamicsCurve (const DeviceHandle&, const std::vector<float>& values);

    // Which curve shows, and in what colour (by default the device's).
    void setCurve (int firstParameter, std::optional<juce::Colour> = {});

    // The level its compressor hears now, in decibels, as far as the area under the curve is lit; -90 dB or less, silence.
    void setHeardLevel (float decibels);
    float getHeardX() const;

    std::function<void()> onEdit;

    bool isFrozen() const noexcept { return frozen; }
    void setFrozen (bool);
    bool snaps = true;

    // Where it shows a place across and up its axes (from 0 to 1), a point, and the handle that bends the line before it
    // (none for the first point), in its own coordinates.
    juce::Point<float> getPosition (float across, float up) const;
    juce::Point<float> getPointPosition (int point) const;
    std::optional<juce::Point<float>> getTensionPosition (int point) const;

    int getCount() const;
    std::vector<devices::curves::Point> getPoints() const;

    // Its menus' changes, each one undo step: a whole curve (its points in order), a line's shape before a point
    // (devices::curves::PointType), deleting a point, and a shape to start from by its name (devices::curves::getShapes()).
    void setPoints (std::vector<devices::curves::Point>, const juce::String& undoName);
    void setPointType (int point, int type);
    void deletePoint (int point);
    bool applyShape (const juce::String& name);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    // What a press takes: a point, or the tension handle of the line before it.
    struct Target
    {
        bool point = true;
        int index = 0;

        bool operator== (const Target&) const = default;
    };

    DeviceHandle device;
    const std::vector<float>& values;
    int first = 0;
    std::optional<juce::Colour> colour;
    float heard = -120.0f;
    bool frozen = false;
    std::optional<Target> hovered, dragged;
    ParameterHandle firstDragged, secondDragged; // what a drag moves
    float startAcross = 0.0f, startUp = 0.0f;

    juce::Rectangle<float> getArea() const;
    float acrossAt (float x) const;
    float upAt (float y) const;
    float getValue (int point, devices::curves::PointField) const;
    std::string_view getID (int point, devices::curves::PointField) const;
    std::optional<Target> findTarget (juce::Point<float>) const;
    void setValue (int point, devices::curves::PointField, float value, const juce::String& undoName);
    void addPoint (juce::Point<float>);
    void edited();
    void paintHandle (juce::Graphics&, juce::Colour background, juce::Colour, Target, juce::Point<float>) const;
    void showPointMenu (int point);
};

// The curve's tools, under it: its menu, Freeze and Snap.
class DynamicsCurveTools : public juce::Component
{
public:
    explicit DynamicsCurveTools (DynamicsCurve&);

    int getPreferredWidth() const;
    void resized() override;

    juce::Button& getFreezeButton() noexcept { return freezeButton; }
    juce::Button& getSnapButton() noexcept { return snapButton; }

private:
    DynamicsCurve& curve;
    IconButton menuButton { "Curve menu", Icon::menu }, freezeButton { "Freeze", Icon::frozen }, snapButton { "Snap", Icon::magnet };

    void showMenu();
};
} // namespace phazite::ui
