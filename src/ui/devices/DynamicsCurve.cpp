// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DynamicsCurve.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "ui/components/TypedField.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <cmath>

namespace phazite::ui
{
namespace
{
    namespace curves = devices::curves;

    // The display, in decibels, the same across and up: from -60 dB at the left and the bottom, where silence also sits, to
    // the top of the curve's axis (+12 dB), a grid line every 6 dB, the 0 dB lines heavier, numbers every 12 dB. Its points
    // are places on the curve's own axis (devices::curves::toAxis()), as the device plays and saves them; only where they
    // show changes.
    constexpr float lowestShown = -60.0f;
    constexpr float gridStep = DynamicsCurve::gridDecibels, numberStep = DynamicsCurve::numberedDecibels;
    constexpr float snapDistance = 6.0f; // pixels from a grid line within which a dragged point snaps to it
    constexpr float tensionDrag = 2.0f;  // a drag the curve's height bends a line by this much of its range
    constexpr float fineTension = 0.2f;  // with Ctrl
    constexpr int deleteItem = 100;      // the point menu's Delete Point

    // Copy Curve's curve, for Paste Curve: any device's, for the session.
    std::optional<std::vector<curves::Point>>& getCopiedCurve()
    {
        static std::optional<std::vector<curves::Point>> copied;
        return copied;
    }

    // A place on the curve's axis as a level: "-12.0 dB", or silence at its start.
    juce::String describeLevel (float place)
    {
        const auto amplitude = curves::fromAxis (place);
        return amplitude <= 1.0e-6f ? juce::String ("silence") : graph::formatDecibels (20.0f * std::log10 (amplitude));
    }

    // The same to a tenth of a decibel always, as the corner reads it and Type Values... shows it: "-18.0", "+3.0", or silence.
    constexpr float silentAmplitude = 1.0e-6f;

    bool isSilent (float place)
    {
        return curves::fromAxis (place) <= silentAmplitude;
    }

    float toDecibels (float place)
    {
        return 20.0f * std::log10 (std::max (silentAmplitude, curves::fromAxis (place)));
    }

    // The top of the display: the curve's axis's top, in decibels (about +12).
    float getHighestShown()
    {
        return 20.0f * std::log10 (curves::fromAxis (1.0f));
    }

    // A place on the curve's axis as a share of the display, from 0 (-60 dB and below, silence) to 1 (its top); and back.
    float getShare (float place)
    {
        return juce::jlimit (0.0f, 1.0f, (toDecibels (place) - lowestShown) / (getHighestShown() - lowestShown));
    }

    float getPlace (float share)
    {
        if (share <= 0.0f)
            return 0.0f;

        if (share >= 1.0f)
            return 1.0f;

        return juce::jlimit (0.0f, 1.0f, curves::axisForDecibels (lowestShown + share * (getHighestShown() - lowestShown)));
    }

    // The grid's lines, as shares of the display: every 6 dB from the bottom up, and the top.
    std::vector<float> getGridShares()
    {
        std::vector<float> shares;

        for (auto decibels = lowestShown; decibels < getHighestShown(); decibels += gridStep)
            shares.push_back ((decibels - lowestShown) / (getHighestShown() - lowestShown));

        shares.push_back (1.0f);
        return shares;
    }

    juce::String formatLevel (float place)
    {
        if (isSilent (place))
            return "silence";

        const auto tenths = std::round (toDecibels (place) * 10.0f); // 0 dB as "0.0", never "-0.0"
        return (tenths > 0.0f ? "+" : "") + juce::String (std::abs (tenths) < 0.5f ? 0.0f : tenths / 10.0f, 1);
    }

    // A level typed, in decibels ("-18", "-18 dB", "+3.5") or silence ("silence", "-inf"), as a place on the axis; none for
    // text that says neither.
    std::optional<float> readLevel (juce::String text)
    {
        // A number alone, with its sign, a decimal point (a comma is one already: splitLevels()) and its unit, "dB"; or silence.
        // Anything else is no level: "1e3" isn't 13 dB, nor "abc5" 5.
        text = text.trim().toLowerCase();

        if (text == "silence" || text == "-inf" || text == "inf")
            return 0.0f;

        if (text.endsWith ("db"))
            text = text.dropLastCharacters (2);

        const auto digits = text.trimCharactersAtStart ("+-");

        if (text.length() - digits.length() > 1 || ! digits.containsOnly ("0123456789.") || ! digits.containsAnyOf ("0123456789")
            || digits.indexOfChar ('.') != digits.lastIndexOfChar ('.'))
            return {};

        return juce::jlimit (0.0f, 1.0f, curves::axisForDecibels (text.getFloatValue()));
    }

    // The levels typed: separated by a semicolon, white space, or a comma followed by a space or a sign; a comma between digits
    // is a decimal comma ("-18,5"), so that "-3,5" is -3.5 dB; "dB" after a number is its unit.
    juce::StringArray splitLevels (const juce::String& text)
    {
        juce::String spaced;
        const auto typed = text.trim();

        for (int i = 0; i < typed.length(); ++i)
        {
            const auto c = typed[i];
            const auto next = i + 1 < typed.length() ? typed[i + 1] : (juce::juce_wchar) 0;

            if (c == ';' || (c == ',' && (next == 0 || juce::CharacterFunctions::isWhitespace (next) || next == '-' || next == '+')))
                spaced << ' ';
            else
                spaced << juce::String::charToString (c == ',' ? (juce::juce_wchar) '.' : c);
        }

        juce::StringArray parts;
        parts.addTokens (spaced, " \t", "");
        parts.removeEmptyStrings();

        for (int i = parts.size(); --i >= 0;)
            if (parts[i].equalsIgnoreCase ("dB"))
                parts.remove (i);

        return parts;
    }

    constexpr int typeValuesItem = 101;     // the point menu's Type Values...
    constexpr int keyRunMilliseconds = 500; // a run of arrow presses ends after this long without one, as a turn of the wheel
    constexpr float limitRatio = 20.0f, gateRatio = 10.0f;

    // To the nearest of the grid's lines (the bottom one silence) and the top, within a few pixels of it, as a place.
    float snap (float place, float size)
    {
        const auto share = getShare (place);
        auto best = place;
        auto bestDistance = snapDistance / std::max (1.0f, size);

        for (const auto line : getGridShares())
        {
            if (const auto distance = std::abs (line - share); distance <= bestDistance)
            {
                best = getPlace (line);
                bestDistance = distance;
            }
        }

        return best;
    }
} // namespace

//==============================================================================
DynamicsCurve::DynamicsCurve (const DeviceHandle& handle, const std::vector<float>& editorValues)
    : device (handle),
      values (editorValues)
{
    setTitle ("Compression curve");
    setDescription ("How loud each level comes out of the compressor: level in across, level out up, lit up to the level it hears "
                    "now. Drag a point (Shift keeps it level, Ctrl keeps it where it is across); drag a hollow handle up or down "
                    "to bend the line before it (Ctrl in fine steps) and right-click it to straighten it; right-click a point for "
                    "its line's shape, to type its levels or to delete it (Alt+click deletes it too); right-click elsewhere to add "
                    "a point. From the keyboard, Left and Right choose a point, and with Ctrl the arrows move it a tenth of a "
                    "decibel in or out, with Shift a decibel.");
    // A Tab stop, for its points by numbers; but a click on it leaves the keys where they were, as on every graph.
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (false);

    drag.onCancelled = [this]
    {
        dragged.reset();
        edited();
    };
}

DynamicsCurve::~DynamicsCurve()
{
    endKeyRun();
}

//==============================================================================
juce::String DynamicsCurve::describePoint (int point) const
{
    if (! juce::isPositiveAndBelow (point, getCount()))
        return {};

    const auto level = [] (float place) { return isSilent (place) ? juce::String ("silence") : formatLevel (place) + " dB"; };
    return "In " + level (getValue (point, curves::pointX)) + ", Out " + level (getValue (point, curves::pointY));
}

juce::String DynamicsCurve::describeRatio (int point) const
{
    // The line that leaves the point, to the next, where it is a single curve: how many decibels in for one out, between its
    // ends.
    const auto next = point + 1;

    if (point < 0 || next >= getCount() || juce::roundToInt (getValue (next, curves::pointType)) != (int) curves::PointType::singleCurve)
        return {};

    const auto fromX = getValue (point, curves::pointX), toX = getValue (next, curves::pointX);
    const auto fromY = getValue (point, curves::pointY), toY = getValue (next, curves::pointY);

    if (isSilent (fromX) || toX <= fromX)
        return {};

    if (isSilent (fromY))
        return isSilent (toY) ? "Limit" : "Gate"; // from silence out: as steep as it gets

    const auto in = toDecibels (toX) - toDecibels (fromX), out = toDecibels (toY) - toDecibels (fromY);

    if (out <= 0.0f || in / out >= limitRatio)
        return "Limit";

    if (const auto ratio = in / out; ratio >= 1.0f)
        return juce::String (ratio, 1) + ":1";

    if (const auto expansion = out / in; expansion <= gateRatio)
        return "1:" + juce::String (expansion, 1);

    return "Gate";
}

std::optional<int> DynamicsCurve::getShownPoint() const
{
    if (dragged.has_value())
        return dragged->point ? std::optional<int> (dragged->index) : std::nullopt;

    if (hovered.has_value() && hovered->point)
        return hovered->index;

    if (chosen >= 0 && chosen < getCount() && hasKeyboardFocus (true))
        return chosen;

    return {};
}

juce::String DynamicsCurve::getReadout() const
{
    if (refusal.isNotEmpty())
        return refusal;

    const auto point = getShownPoint();

    if (! point.has_value() || frozen)
        return {};

    const auto ratio = describeRatio (*point);
    return describePoint (*point) + (ratio.isNotEmpty() ? "\n" + ratio : juce::String());
}

bool DynamicsCurve::setPointLevels (int point, const juce::String& text)
{
    if (frozen || ! juce::isPositiveAndBelow (point, getCount()))
        return false;

    const auto parts = splitLevels (text);
    const auto isEnd = point == 0 || point == getCount() - 1;
    const auto refuse = [this] (const juce::String& why)
    {
        refusal = why;
        repaint();
        announce();
        return false;
    };

    // The curve's ends stay at its ends: their level out alone. A point between: its level in and its level out.
    if (isEnd ? parts.size() != 1 : parts.size() != 2)
        return refuse (isEnd ? "Type its level out alone,\nin decibels: -3" : "Type its levels in and out,\nin decibels: -18, -12");

    for (const auto& part : parts)
        if (! readLevel (part).has_value())
            return refuse ("\"" + part + "\" isn't a level:\ntype decibels, or silence");

    const auto out = readLevel (parts[parts.size() - 1]);
    const auto in = isEnd ? std::optional<float> (getValue (point, curves::pointX)) : readLevel (parts[0]);
    refusal = {};

    const auto earliest = point > 0 ? getValue (point - 1, curves::pointX) : 0.0f;
    const auto latest = point < getCount() - 1 ? getValue (point + 1, curves::pointX) : 1.0f;
    endKeyRun();

    // One step, named as a drag's: the place across holding it, the place up joining it.
    auto across = device.getParameter (getID (point, curves::pointX), "Move Point");
    auto up = device.getParameter (getID (point, curves::pointY), "Move Point");
    across.beginGesture();

    if (! isEnd)
        across.setValue (juce::jlimit (earliest, std::max (earliest, latest), *in));

    up.beginGesture();
    up.setValue (*out);
    up.endGesture();
    across.endGesture();
    edited();
    return true;
}

void DynamicsCurve::typeValues (int point)
{
    auto* parent = TypedField::findParent (*this);

    if (parent == nullptr || frozen || ! juce::isPositiveAndBelow (point, getCount()))
        return;

    // Over the point, as wide as the levels' texts, its levels in decibels: "in, out", or the curve's ends' level out.
    const auto isEnd = point == 0 || point == getCount() - 1;
    const auto centre = getPointPosition (point).roundToInt();
    valuesField = std::make_unique<TypedField> (*this);
    valuesField->setTitle ("Point " + juce::String (point + 1) + " levels");
    valuesField->getTarget = [this, centre] (const juce::Component& in)
    { return in.getLocalArea (this, juce::Rectangle<int> (TypedField::getMinimumWidth(), controlHeight).withCentre (centre)); };
    valuesField->getTexts = [] { return juce::StringArray { "-120.0, -120.0", "silence, silence" }; };
    valuesField->onApply = [this, point] (const juce::String& text) { setPointLevels (point, text); };
    valuesField->onFinished = [safeThis = juce::Component::SafePointer<DynamicsCurve> (this)]
    {
        juce::MessageManager::callAsync (
            [safeThis]
            {
                if (safeThis != nullptr && safeThis->valuesField != nullptr && safeThis->valuesField->isFinished())
                    safeThis->valuesField = nullptr;
            });
    };

    parent->addAndMakeVisible (*valuesField);
    const auto outText = formatLevel (getValue (point, curves::pointY));
    valuesField->show (isEnd ? outText : formatLevel (getValue (point, curves::pointX)) + ", " + outText);
}

juce::TextEditor* DynamicsCurve::getValuesField() const
{
    return valuesField != nullptr && ! valuesField->isFinished() ? valuesField.get() : nullptr;
}

//==============================================================================
bool DynamicsCurve::keyPressed (const juce::KeyPress& key)
{
    // A refusal shows until the next edit or move of the pointer: a key is one.
    if (refusal.isNotEmpty())
    {
        refusal = {};
        repaint();
    }
    const auto code = key.getKeyCode();
    const auto across = code == juce::KeyPress::leftKey ? -1.0f : code == juce::KeyPress::rightKey ? 1.0f : 0.0f;
    const auto up = code == juce::KeyPress::downKey ? -1.0f : code == juce::KeyPress::upKey ? 1.0f : 0.0f;

    // Not the curve's: a run of arrow presses ends first, so that what the key does is a step of its own.
    if (frozen || (juce::exactlyEqual (across, 0.0f) && juce::exactlyEqual (up, 0.0f)))
    {
        endKeyRun();
        return false;
    }

    const auto mods = key.getModifiers();
    const auto step = mods.isShiftDown() ? 1.0f : mods.isCommandDown() ? 0.1f : 0.0f;

    // Left and Right alone choose a point; Up and Down alone do nothing here.
    if (juce::exactlyEqual (step, 0.0f))
    {
        endKeyRun();

        if (juce::exactlyEqual (across, 0.0f))
            return false;

        endKeyRun();
        chosen = juce::jlimit (0, getCount() - 1, chosen < 0 ? 0 : chosen + (int) across);
        repaint();
        announce();
        return true;
    }

    chosen = juce::jlimit (0, getCount() - 1, std::max (0, chosen));
    movePointBy (chosen, across * step, up * step);
    return true;
}

void DynamicsCurve::movePointBy (int point, float inDecibels, float outDecibels)
{
    if (! juce::isPositiveAndBelow (point, getCount()))
        return;

    // A run of presses on one point is one step, which the place across holds, as a turn of the wheel is (graph::WheelGesture).
    if (keyedPoint != point || ! keyedAcross.isInGesture())
    {
        endKeyRun();
        keyedAcross = device.getParameter (getID (point, curves::pointX), "Move Point");
        keyedUp = device.getParameter (getID (point, curves::pointY));
        keyedAcross.beginGesture();
        keyedPoint = point;
    }

    const auto isEnd = point == 0 || point == getCount() - 1;

    if (! juce::exactlyEqual (inDecibels, 0.0f) && ! isEnd)
    {
        const auto earliest = getValue (point - 1, curves::pointX), latest = getValue (point + 1, curves::pointX);
        const auto moved = curves::axisForDecibels (toDecibels (getValue (point, curves::pointX)) + inDecibels);
        keyedAcross.setValue (juce::jlimit (earliest, std::max (earliest, latest), moved));
    }

    if (! juce::exactlyEqual (outDecibels, 0.0f))
    {
        keyedUp.beginGesture();
        keyedUp.setValue (juce::jlimit (0.0f, 1.0f, curves::axisForDecibels (toDecibels (getValue (point, curves::pointY)) + outDecibels)));
        keyedUp.endGesture();
    }

    startTimer (keyRunMilliseconds);
    edited();
    announce();
}

void DynamicsCurve::endKeyRun()
{
    stopTimer();

    if (keyedAcross.isInGesture())
        keyedAcross.endGesture();

    keyedPoint = -1;
}

void DynamicsCurve::focusGained (FocusChangeType)
{
    if (chosen < 0 || chosen >= getCount())
        chosen = 0;

    repaint();
    announce();
}

void DynamicsCurve::focusLost (FocusChangeType)
{
    endKeyRun();
    repaint();
}

void DynamicsCurve::announce()
{
    if (auto* handler = getAccessibilityHandler())
        handler->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
}

std::unique_ptr<juce::AccessibilityHandler> DynamicsCurve::createAccessibilityHandler()
{
    // Screen readers read the chosen point's numbers as its value, which the arrow keys change.
    class Value final : public juce::AccessibilityTextValueInterface
    {
    public:
        explicit Value (const DynamicsCurve& c)
            : curve (c)
        {
        }

        bool isReadOnly() const override { return true; }

        juce::String getCurrentValueAsString() const override
        {
            if (curve.refusal.isNotEmpty())
                return curve.refusal.replace ("\n", " ");

            if (curve.chosen < 0)
                return {};

            const auto ratio = curve.describeRatio (curve.chosen);
            return "Point " + juce::String (curve.chosen + 1) + ": " + curve.describePoint (curve.chosen)
                   + (ratio.isNotEmpty() ? ", " + ratio : juce::String());
        }

        void setValueAsString (const juce::String&) override {}

    private:
        const DynamicsCurve& curve;
    };

    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::slider, juce::AccessibilityActions(),
                                                         juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
}

void DynamicsCurve::setCurve (int firstParameter, std::optional<juce::Colour> newColour)
{
    // A run of arrow presses on the curve shown before is a step of its own, ended before the curve's points are another's.
    endKeyRun();
    refusal = {};
    first = firstParameter;
    colour = newColour;
    hovered.reset();
    repaint();
}

void DynamicsCurve::setHeardLevel (float decibels)
{
    heard = decibels;
}

float DynamicsCurve::getHeardX() const
{
    return heard <= -90.0f ? getArea().getX() : getPosition (std::min (1.0f, curves::axisForDecibels (heard)), 0.0f).x;
}

void DynamicsCurve::setFrozen (bool shouldBeFrozen)
{
    frozen = shouldBeFrozen;
    hovered.reset();
    showCursorFor (hovered);
    repaint();
}

juce::String DynamicsCurve::getTooltip()
{
    if (! hovered.has_value() || hovered->index >= getCount())
        return {};

    const auto i = hovered->index;

    if (! hovered->point)
        return "The line before point " + juce::String (i + 1)
               + ": drag up or down to bend it (Ctrl in fine steps), or right-click it to straighten it";

    const auto levels = describeLevel (getValue (i, curves::pointX)) + " in, " + describeLevel (getValue (i, curves::pointY)) + " out";

    if (i == 0 || i == getCount() - 1)
        return "The curve's " + juce::String (i == 0 ? "start" : "end") + " (" + levels
               + "): drag it up or down, as it stays at the curve's end; right-click for the line's shape or to type its level";

    return "Point " + juce::String (i + 1) + " (" + levels
           + "): drag it (Shift keeps it level, Ctrl where it is across); right-click for its line's shape, to type its "
             "levels or to delete it, or Alt-click to delete it";
}

void DynamicsCurve::showCursorFor (const std::optional<Target>& target)
{
    // A point moves both ways, but the ends of the curve only up and down, as a handle bends its line.
    const auto isEnd = target.has_value() && target->point && (target->index == 0 || target->index == getCount() - 1);
    setMouseCursor (! target.has_value()       ? juce::MouseCursor::NormalCursor
                    : target->point && ! isEnd ? juce::MouseCursor::DraggingHandCursor
                                               : juce::MouseCursor::UpDownResizeCursor);
}

juce::Rectangle<float> DynamicsCurve::getArea() const
{
    return getLocalBounds().toFloat().reduced (GraphTint::handleRadius * 1.5f + 1.0f);
}

float DynamicsCurve::getShareForDecibels (float decibels)
{
    return juce::jlimit (0.0f, 1.0f, (decibels - lowestShown) / (getHighestShown() - lowestShown));
}

juce::Point<float> DynamicsCurve::getPosition (float across, float up) const
{
    const auto area = getArea();
    return { area.getX() + getShare (across) * area.getWidth(), area.getBottom() - getShare (up) * area.getHeight() };
}

float DynamicsCurve::acrossAt (float x) const
{
    return getPlace ((x - getArea().getX()) / std::max (1.0f, getArea().getWidth()));
}

float DynamicsCurve::upAt (float y) const
{
    return getPlace ((getArea().getBottom() - y) / std::max (1.0f, getArea().getHeight()));
}

int DynamicsCurve::getCount() const
{
    return juce::jlimit (2, curves::maxPoints, juce::roundToInt (values[(size_t) first]));
}

float DynamicsCurve::getValue (int point, curves::PointField field) const
{
    return values[(size_t) (first + curves::getPointParameter (point, field))];
}

std::string_view DynamicsCurve::getID (int point, curves::PointField field) const
{
    return device.getInfo()->parameters[(size_t) (first + curves::getPointParameter (point, field))].id;
}

std::vector<curves::Point> DynamicsCurve::getPoints() const
{
    std::vector<curves::Point> points;

    for (int i = 0; i < getCount(); ++i)
        points.push_back ({ getValue (i, curves::pointX), getValue (i, curves::pointY), getValue (i, curves::pointTension),
                            getValue (i, curves::pointType) });

    return points;
}

juce::Point<float> DynamicsCurve::getPointPosition (int point) const
{
    return getPosition (getValue (point, curves::pointX), getValue (point, curves::pointY));
}

std::optional<juce::Point<float>> DynamicsCurve::getTensionPosition (int point) const
{
    if (point <= 0 || point >= getCount())
        return {};

    // Half way across the line as it shows, on it.
    const auto middleX = 0.5f * (getPointPosition (point - 1).x + getPointPosition (point).x);
    const auto middle =
        juce::jlimit (getValue (point - 1, curves::pointX),
                      std::max (getValue (point - 1, curves::pointX), getValue (point, curves::pointX)), acrossAt (middleX));
    return juce::Point<float> (
        middleX, getPosition (0.0f, curves::getAt (std::span (values).subspan ((size_t) first, curves::parameterCount), middle)).y);
}

void DynamicsCurve::edited()
{
    if (onEdit != nullptr)
        onEdit();

    repaint();
}

// The points in order (the last one repeated in the places they leave), as one undo step, which the count's change holds.
void DynamicsCurve::setPoints (std::vector<curves::Point> points, const juce::String& undoName)
{
    if (points.size() < 2)
        return;

    points.resize (std::min (points.size(), (size_t) curves::maxPoints));
    auto holder = device.getParameter (device.getInfo()->parameters[(size_t) first].id, undoName);
    holder.beginGesture();

    for (int i = 0; i < curves::maxPoints; ++i)
    {
        const auto& point = points[(size_t) std::min (i, (int) points.size() - 1)];

        for (int field = 0; field < curves::pointFields; ++field)
        {
            auto handle = device.getParameter (getID (i, (curves::PointField) field));

            if (! juce::approximatelyEqual (handle.getValue(), point[(size_t) field]))
            {
                handle.beginGesture();
                handle.setValue (point[(size_t) field]);
                handle.endGesture();
            }
        }
    }

    holder.setValue ((float) points.size());
    holder.endGesture();
    edited();
}

void DynamicsCurve::setValue (int point, curves::PointField field, float value, const juce::String& undoName)
{
    auto handle = device.getParameter (getID (point, field), undoName);
    handle.setValue (value);
    edited();
}

void DynamicsCurve::setPointType (int point, int type)
{
    if (point > 0 && point < getCount())
        setValue (point, curves::pointType, (float) type, "Change Line");
}

void DynamicsCurve::deletePoint (int point)
{
    auto points = getPoints();

    if (point <= 0 || point >= (int) points.size() - 1)
        return;

    points.erase (points.begin() + point);
    setPoints (points, "Delete Point");
}

bool DynamicsCurve::applyShape (const juce::String& name)
{
    if (const auto* shape = curves::findShape (name.toStdString()))
    {
        setPoints (shape->points, "Change Curve");
        return true;
    }

    return false;
}

void DynamicsCurve::addPoint (juce::Point<float> position)
{
    auto points = getPoints();

    if ((int) points.size() >= curves::maxPoints)
        return;

    const auto across = acrossAt (position.x);
    auto at = points.begin() + 1;

    while (at != points.end() - 1 && (*at)[curves::pointX] < across)
        ++at;

    points.insert (at, curves::makePoint (across, upAt (position.y)));
    setPoints (points, "Add Point");
}

//==============================================================================
void DynamicsCurve::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto tint = frozen ? p.textMuted : colour.value_or (getDeviceTint (*this));
    const auto area = getArea();
    graph::paintBackground (g, p, getLocalBounds().toFloat());

    // The grid, the same across and up: a line every 6 dB, the 0 dB lines stronger, and level in equalling level out; numbers
    // every 12 dB, up the left and along the bottom.
    g.setFont (juce::FontOptions (TextSize::small));
    const auto highest = getHighestShown();

    for (auto decibels = lowestShown + gridStep; decibels < highest - 0.5f * gridStep; decibels += gridStep)
    {
        const auto place = curves::axisForDecibels (decibels);
        const auto at = getPosition (place, place);
        const auto isZero = std::abs (decibels) < 0.5f * gridStep;
        g.setColour (p.text.withAlpha (isZero ? GraphTint::zeroLine : GraphTint::grid));
        g.drawVerticalLine (juce::roundToInt (at.x), area.getY(), area.getBottom());
        g.drawHorizontalLine (juce::roundToInt (at.y), area.getX(), area.getRight());

        if (std::abs (std::remainder (decibels, numberStep)) < 0.5f)
        {
            const auto text = juce::String (juce::roundToInt (decibels));
            const auto textWidth = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 1.0f;
            g.setColour (p.textMuted);
            g.drawText (text, juce::Rectangle<float> (area.getX() + (float) Space::xs, at.y - TextSize::small, textWidth, TextSize::small),
                        juce::Justification::centredLeft, false);
            g.drawText (text,
                        juce::Rectangle<float> (at.x + (float) Space::xs, area.getBottom() - TextSize::small - (float) Space::xs, textWidth,
                                                TextSize::small),
                        juce::Justification::centredLeft, false);
        }
    }

    g.setColour (p.text.withAlpha (GraphTint::zeroLine));
    g.drawLine ({ getPosition (0.0f, 0.0f), getPosition (1.0f, 1.0f) });
    g.setColour (p.textMuted);
    g.drawText ("OUT", area.reduced (Space::xs).toNearestInt(), juce::Justification::topLeft);
    g.drawText ("IN", area.reduced (Space::xs).toNearestInt(), juce::Justification::bottomRight);

    // The curve, over a wash of the area under it, lit up to the level its compressor hears now: at each pixel across, the
    // level out for the level in there, as the device plays it.
    const auto curve = std::span (values).subspan ((size_t) first, curves::parameterCount);
    const auto line =
        graph::makeCurve (area, [this, curve] (float x) { return getPosition (0.0f, curves::getAt (curve, acrossAt (x))).y; }, 1.0f);
    auto wash = line;
    wash.lineTo (area.getBottomRight());
    wash.lineTo (area.getBottomLeft());
    wash.closeSubPath();
    g.setColour (tint.withAlpha (GraphTint::curveWash));
    g.fillPath (wash);

    if (const auto heardX = getHeardX(); heardX > area.getX())
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (area.withRight (heardX).getSmallestIntegerContainer());
        g.setColour (tint.withAlpha (GraphTint::heardWash));
        g.fillPath (wash);
    }

    g.setColour (tint);
    g.strokePath (line, juce::PathStrokeType (GraphTint::curveWidth));

    // Its points filled, its tension handles hollow; lit under the pointer and while dragged. Frozen, none.
    if (frozen)
        return;

    for (int i = 0; i < getCount(); ++i)
    {
        if (const auto handle = getTensionPosition (i))
            paintHandle (g, p, tint, { false, i }, *handle);

        paintHandle (g, p, tint, { true, i }, getPointPosition (i));
    }

    // The point under the pointer, dragged or chosen, as numbers in its top right corner, the ratio of its line under them.
    const auto readout = juce::StringArray::fromLines (getReadout());
    auto corner = area.reduced ((float) Space::xs);

    for (const auto& row : readout)
        graph::paintReadout (g, p, corner.removeFromTop (TextSize::small + (float) Space::xs), row, juce::Justification::topRight);
}

void DynamicsCurve::paintHandle (juce::Graphics& g, const Palette& p, juce::Colour tint, Target target, juce::Point<float> at) const
{
    const auto radius = GraphTint::handleRadius;
    const auto dot = juce::Rectangle<float> (2.0f * radius, 2.0f * radius).withCentre (at);
    g.setColour (p.background);
    g.fillEllipse (dot.expanded (1.0f));
    g.setColour (tint);

    if (target.point)
        g.fillEllipse (dot);
    else
        g.drawEllipse (dot.reduced (GraphTint::handleOutline / 2.0f), GraphTint::handleOutline);

    // Under the pointer, dragged, or chosen from the keyboard, lit as the other graphs' handles are: ringed in the text colour
    // from its edge out.
    const auto isChosen = target.point && target.index == chosen && ! dragged.has_value() && hasKeyboardFocus (true);

    if (target == (dragged.has_value() ? dragged : hovered) || isChosen)
    {
        g.setColour (p.text);
        g.drawEllipse (dot.expanded (GraphTint::litGap), GraphTint::litWidth);
    }
}

//==============================================================================
std::optional<DynamicsCurve::Target> DynamicsCurve::findTarget (juce::Point<float> position) const
{
    std::optional<Target> nearest;
    auto nearestDistance = GraphTint::grabDistance;

    const auto consider = [&] (Target target, juce::Point<float> at)
    {
        if (const auto distance = at.getDistanceFrom (position); distance <= nearestDistance)
        {
            nearest = target;
            nearestDistance = distance;
        }
    };

    for (int i = 0; i < getCount(); ++i)
    {
        consider ({ true, i }, getPointPosition (i));

        if (const auto handle = getTensionPosition (i))
            consider ({ false, i }, *handle);
    }

    return nearest;
}

void DynamicsCurve::mouseMove (const juce::MouseEvent& e)
{
    // The pointer at each move, so that it is never one left from before (on entering away from a handle, say).
    const auto target = frozen ? std::optional<Target>() : findTarget (e.position);
    showCursorFor (target);

    // A refusal shows until the next edit or move of the pointer.
    if (refusal.isNotEmpty())
    {
        refusal = {};
        repaint();
    }

    if (target != hovered)
    {
        hovered = target;
        repaint();
    }
}

void DynamicsCurve::mouseExit (const juce::MouseEvent&)
{
    setMouseCursor (juce::MouseCursor::NormalCursor);

    if (hovered.has_value())
    {
        hovered.reset();
        repaint();
    }
}

void DynamicsCurve::mouseDown (const juce::MouseEvent& e)
{
    if (frozen)
        return;

    endKeyRun(); // a run of arrow presses is a step of its own, which a drag doesn't join
    refusal = {};
    const auto target = findTarget (e.position);

    if (target.has_value() && target->point)
        chosen = target->index;

    // The right button: a point's menu, a handle straightened, or a point added where it is.
    if (e.mods.isPopupMenu())
    {
        if (! target.has_value())
            addPoint (e.position);
        else if (target->point)
            showPointMenu (target->index);
        else
            setValue (target->index, curves::pointTension, 0.0f, "Straighten Line");

        return;
    }

    if (! target.has_value())
        return;

    if (target->point && e.mods.isAltDown())
    {
        deletePoint (target->index);
        return;
    }

    dragged = target;

    // Each from its value set by hand, the press changing nothing (graph::HandleDrag): a point's place across, whose gesture
    // holds the step, and up; or its line's tension.
    if (target->point)
    {
        firstDragged = device.getParameter (getID (target->index, curves::pointX), "Move Point");
        secondDragged = device.getParameter (getID (target->index, curves::pointY));
        drag.take ({ &firstDragged, &secondDragged });
    }
    else
    {
        firstDragged = device.getParameter (getID (target->index, curves::pointTension), "Bend Line");
        drag.take ({ &firstDragged });
    }
}

void DynamicsCurve::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragged.has_value() || ! drag.isTaken())
        return;

    const auto area = getArea();
    const auto i = dragged->index;

    if (! dragged->point)
    {
        // Up bends the line up; Ctrl in fine steps.
        const auto up = (float) -e.getDistanceFromDragStartY() / std::max (1.0f, area.getHeight());
        const auto fine = e.mods.isCtrlDown() ? fineTension : 1.0f;
        drag.set (0, juce::jlimit (-1.0f, 1.0f, drag.getStart (0) + tensionDrag * up * fine));
        edited();
        return;
    }

    const auto startAcross = drag.getStart (0), startUp = drag.getStart (1);

    // As far as the pointer has moved on the display, from where the point showed as it was taken: unmoved along an axis, its
    // place there exactly. Shift keeps it level, Ctrl keeps it where it is across; the first and last points stay at the
    // curve's ends.
    const auto started = getPosition (startAcross, startUp);
    const auto movedX = (float) e.getDistanceFromDragStartX(), movedY = (float) e.getDistanceFromDragStartY();
    const auto isEnd = i == 0 || i == getCount() - 1;
    const auto movesAcross = ! (isEnd || e.mods.isCtrlDown() || juce::exactlyEqual (movedX, 0.0f));
    const auto movesUp = ! (e.mods.isShiftDown() || juce::exactlyEqual (movedY, 0.0f));
    auto across = movesAcross ? acrossAt (started.x + movedX) : startAcross;
    auto height = movesUp ? upAt (started.y + movedY) : startUp;

    // Snap takes it onto a grid line along the way it moves only: left where it is along the other, it keeps its place there
    // exactly (a point under the display's lowest level, dragged straight up, keeps its level in).
    if (snaps)
    {
        if (movesAcross)
            across = snap (across, area.getWidth());

        if (movesUp)
            height = snap (height, area.getHeight());
    }

    const auto earliest = i > 0 ? getValue (i - 1, curves::pointX) : 0.0f;
    const auto latest = i < getCount() - 1 ? getValue (i + 1, curves::pointX) : 1.0f;
    drag.set (0, isEnd ? startAcross : juce::jlimit (earliest, std::max (earliest, latest), across));
    drag.set (1, juce::jlimit (0.0f, 1.0f, height));
    edited();
}

void DynamicsCurve::mouseUp (const juce::MouseEvent&)
{
    drag.end();

    if (dragged.has_value())
    {
        dragged.reset();
        repaint();
    }
}

void DynamicsCurve::showPointMenu (int point)
{
    juce::PopupMenu menu;
    const auto current = juce::roundToInt (getValue (point, curves::pointType));

    // Its levels typed, first, as an automation point's value is.
    menu.addItem (juce::PopupMenu::Item ("Type Values...").setID (typeValuesItem));
    menu.addSeparator();

    for (int type = 0; type < (int) std::size (curves::pointTypeNames); ++type)
    {
        const auto name = curves::pointTypeNames[(size_t) type];
        menu.addItem (juce::PopupMenu::Item (juce::String (name.data(), name.size()))
                          .setID (type + 1)
                          .setEnabled (point > 0)
                          .setTicked (point > 0 && type == current));
    }

    menu.addSeparator();
    menu.addItem (juce::PopupMenu::Item ("Delete Point").setID (deleteItem).setEnabled (point > 0 && point < getCount() - 1));

    const juce::Component::SafePointer<DynamicsCurve> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                        [safeThis, point] (int result)
                        {
                            if (safeThis == nullptr || result <= 0)
                                return;

                            if (result == deleteItem)
                                safeThis->deletePoint (point);
                            else if (result == typeValuesItem)
                                safeThis->typeValues (point);
                            else
                                safeThis->setPointType (point, result - 1);
                        });
}

//==============================================================================
DynamicsCurveTools::DynamicsCurveTools (DynamicsCurve& c)
    : curve (c)
{
    menuButton.setTooltip ("The curve's menu: shapes to start from, copy and paste, flip, smooth all points, reset");
    menuButton.onClick = [this] { showMenu(); };
    freezeButton.setClickingTogglesState (true);
    freezeButton.setTooltip ("Freeze: the curve can't be edited while this is on");
    freezeButton.onClick = [this] { curve.setFrozen (freezeButton.getToggleState()); };
    snapButton.setClickingTogglesState (true);
    snapButton.setToggleState (curve.snaps, juce::dontSendNotification);
    snapButton.setTooltip ("Snap: points dragged near the grid's lines go onto them");
    snapButton.onClick = [this] { curve.snaps = snapButton.getToggleState(); };

    for (auto* button : { &menuButton, &freezeButton, &snapButton })
        addAndMakeVisible (button);
}

int DynamicsCurveTools::getPreferredWidth() const
{
    return compact ? controlHeight + Space::s : 3 * (controlHeight + Space::s) + 2 * Space::xs;
}

void DynamicsCurveTools::setCompact (bool shouldBeCompact)
{
    if (compact == shouldBeCompact)
        return;

    compact = shouldBeCompact;
    menuButton.setTooltip (compact ? "The curve's menu: shapes to start from, copy and paste, flip, smooth all points, reset, Freeze "
                                     "and Snap"
                                   : "The curve's menu: shapes to start from, copy and paste, flip, smooth all points, reset");
    resized();
}

void DynamicsCurveTools::resized()
{
    auto area = getLocalBounds();
    const auto width = area.getHeight() + Space::s;

    for (auto* button : { &menuButton, &freezeButton, &snapButton })
    {
        button->setVisible (button == &menuButton || ! compact);
        button->setBounds (area.removeFromLeft (width));
        area.removeFromLeft (Space::xs);
    }
}

void DynamicsCurveTools::showMenu()
{
    getMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton));
}

juce::PopupMenu DynamicsCurveTools::getMenu()
{
    juce::PopupMenu shapes;
    const juce::Component::SafePointer<DynamicsCurveTools> safe (this);

    // The gates under a header that says how they work on this curve: they close as the gain goes down, at the Attack, and
    // open as it comes back, at the Release, the reverse of a gate's own words.
    auto inGates = false;

    for (const auto& shape : curves::getShapes())
    {
        const auto name = juce::String (shape.name.data(), shape.name.size());

        if (name.startsWith ("Gate") != inGates)
        {
            inGates = ! inGates;

            shapes.addSeparator();

            if (inGates)
                shapes.addSectionHeader ("Gates: the Attack closes them, the Release opens them");
        }

        shapes.addItem (name,
                        [safe, name]
                        {
                            if (safe != nullptr)
                                safe->curve.applyShape (name);
                        });
    }

    // Each a change of the points the curve has, in the menu's name.
    const auto change = [safe] (const char* undoName, auto transform)
    {
        return [safe, undoName, transform]
        {
            if (safe == nullptr)
                return;

            auto points = safe->curve.getPoints();

            for (auto& point : points)
                transform (point);

            safe->curve.setPoints (points, undoName);
        };
    };

    juce::PopupMenu menu;
    menu.addSubMenu ("Shapes", shapes);
    menu.addSeparator();
    menu.addItem ("Copy Curve",
                  [safe]
                  {
                      if (safe != nullptr)
                          getCopiedCurve() = safe->curve.getPoints();
                  });
    menu.addItem ("Paste Curve", getCopiedCurve().has_value(), false,
                  [safe]
                  {
                      if (safe != nullptr && getCopiedCurve().has_value())
                          safe->curve.setPoints (*getCopiedCurve(), "Paste Curve");
                  });
    menu.addItem ("Flip Vertically",
                  change ("Flip Curve", [] (curves::Point& point) { point[curves::pointY] = 1.0f - point[curves::pointY]; }));
    menu.addItem ("Smooth All Points", change ("Smooth Curve", [] (curves::Point& point)
                                               { point[curves::pointType] = (float) curves::PointType::singleCurve; }));
    menu.addItem ("Reset Curve",
                  [safe]
                  {
                      if (safe != nullptr)
                          safe->curve.applyShape (
                              juce::String (curves::getShapes().front().name.data(), curves::getShapes().front().name.size()));
                  });

    // Compact, Freeze and Snap are here, as their buttons don't show.
    if (compact)
    {
        menu.addSeparator();

        for (auto* button : { &freezeButton, &snapButton })
            menu.addItem (button->getTitle(), true, button->getToggleState(),
                          [safe, which = button == &freezeButton]
                          {
                              if (safe != nullptr)
                              {
                                  auto& toggled = which ? safe->freezeButton : safe->snapButton;
                                  toggled.setToggleState (! toggled.getToggleState(), juce::sendNotificationSync);
                              }
                          });
    }

    return menu;
}
} // namespace phazite::ui
