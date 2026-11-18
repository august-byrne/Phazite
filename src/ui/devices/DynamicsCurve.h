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
// Under the pointer, what a drag takes lights up, ringed in the text colour as the other graphs' handles are, the pointer
// shows which way it moves (a hand for a point, up and down for an end point or a handle), and its tooltip, which the hint
// bar shows, says what it does (the owner, 2026-11-10: "it should be obvious that you are able to do this gesture"). Each
// change is one undo step; a drag moves its handles from their values set by hand and Escape takes it back
// (graph::HandleDrag).
//
// Its points are exact by numbers too, as a compressor's threshold and ratio are (roadmap step S6.3):
// - While a point is under the pointer, dragged or chosen from the keyboard, its corner reads it, "In -18.0 dB, Out -12.0
//   dB", and the ratio of the line that leaves it, where that is a single curve: "4.0:1", "1:2.0" for one that expands,
//   "Limit" from 20:1, "Gate" past 1:10.
// - A point's menu has Type Values...: a field over it, as an automation point's Type a Value..., takes "in, out" in
//   decibels ("-18, -12"), the curve's ends their level out alone, and moves it there, one undo step named as a drag's.
// - Tab reaches it; Left and Right choose a point (lit as under the pointer); with Ctrl the arrows move it a tenth of a
//   decibel in (Left, Right) or out (Up, Down), with Shift a decibel, each run of presses one undo step, as a turn of the
//   EQ's wheel is (graph::WheelGesture). Screen readers read the chosen point's numbers as its value.
//
// Its tools go under it
// (DynamicsCurveTools): its menu (shapes to start from, copy and paste, from any device's curve to any other's, flip,
// smooth, reset), Freeze (no editing) and Snap, the editor's for the session.
#pragma once

#include "DeviceGraph.h"
#include "devices/dynamics/DynamicsCurve.h"
#include "model/devices/DeviceHandle.h"
#include "ui/theme/DesignTokens.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <vector>

namespace phazite::ui
{
class TypedField;

class DynamicsCurve : public juce::Component,
                      public juce::TooltipClient,
                      private juce::Timer
{
public:
    // The device's curve whose parameters (its count of points, then its points: devices::curves) start at the index set,
    // read from its editor's values, every parameter's in the device's order, which its editor brings up to date: onEdit
    // asks it to, after each change the curve makes.
    DynamicsCurve (const DeviceHandle&, const std::vector<float>& values);
    ~DynamicsCurve() override;

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

    // A point as numbers, "In -18.0 dB, Out -12.0 dB", and the ratio of the line that leaves it ("4.0:1", "1:2.0", "Limit",
    // "Gate"), none where that line is no single curve, or there is none; and what the corner reads now (a point under the
    // pointer, dragged or chosen), empty with none.
    juce::String describePoint (int point) const;
    juce::String describeRatio (int point) const;
    juce::String getReadout() const;

    // Type Values...: the field over a point, and what it does with its text: "in, out" in decibels ("silence" for none), the
    // curve's ends their level out alone, the point moved there as one undo step ("Move Point"). False for text it can't read.
    void typeValues (int point);
    // Text it can't read, it refuses, and says why in its corner (and to screen readers) until the next edit or move of the
    // pointer: getRefusal(), empty when nothing was refused. A comma between digits is a decimal comma ("-18,5"); the levels
    // are separated by a semicolon, white space, or a comma followed by a space or a sign.
    juce::TextEditor* getValuesField() const;
    bool setPointLevels (int point, const juce::String& text);
    juce::String getRefusal() const { return refusal; }

    // The point chosen from the keyboard, -1 for none.
    int getChosenPoint() const noexcept { return chosen; }

    // What the point or handle under the pointer does, and how to work it; nothing elsewhere (the hint bar then shows the
    // curve's description).
    juce::String getTooltip() override;

    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override;
    void focusLost (FocusChangeType) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

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
    ParameterHandle firstDragged, secondDragged; // what a drag moves,
    graph::HandleDrag drag { *this };            // each from its value set by hand
    int chosen = -1;                             // from the keyboard
    juce::String refusal;                        // why the levels typed last were refused, until the next edit
    ParameterHandle keyedAcross, keyedUp;        // a run of arrow presses: the point's, whose gesture holds the step,
    int keyedPoint = -1;                         // and which point it moves
    std::unique_ptr<TypedField> valuesField;     // Type Values...'s, while it is open and until a message after it closed

    std::optional<int> getShownPoint() const; // the point the corner reads: dragged, under the pointer, or chosen
    void movePointBy (int point, float inDecibels, float outDecibels);
    void endKeyRun();
    void timerCallback() override { endKeyRun(); }
    void announce(); // the chosen point's numbers, to screen readers

    juce::Rectangle<float> getArea() const;
    float acrossAt (float x) const;
    float upAt (float y) const;
    float getValue (int point, devices::curves::PointField) const;
    std::string_view getID (int point, devices::curves::PointField) const;
    std::optional<Target> findTarget (juce::Point<float>) const;
    void setValue (int point, devices::curves::PointField, float value, const juce::String& undoName);
    void addPoint (juce::Point<float>);
    void edited();
    void paintHandle (juce::Graphics&, const Palette&, juce::Colour tint, Target, juce::Point<float>) const;
    void showPointMenu (int point);
    void showCursorFor (const std::optional<Target>&);
};

// The curve's tools, under it: its menu, Freeze and Snap; or, compact, its menu alone, which then has Freeze and Snap in it,
// ticked while they are on (the multiband's, beside its Monitor's switch where its column is narrow).
class DynamicsCurveTools : public juce::Component
{
public:
    explicit DynamicsCurveTools (DynamicsCurve&);

    int getPreferredWidth() const;
    void resized() override;

    void setCompact (bool);
    bool isCompact() const noexcept { return compact; }

    juce::Button& getMenuButton() noexcept { return menuButton; }
    juce::Button& getFreezeButton() noexcept { return freezeButton; }
    juce::Button& getSnapButton() noexcept { return snapButton; }

    // The curve's menu, as its button shows it.
    juce::PopupMenu getMenu();

private:
    DynamicsCurve& curve;
    IconButton menuButton { "Curve menu", Icon::menu }, freezeButton { "Freeze", Icon::frozen }, snapButton { "Snap", Icon::magnet };
    bool compact = false;

    void showMenu();
};
} // namespace phazite::ui
