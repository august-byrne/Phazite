// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DynamicsCurve.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    namespace curves = devices::curves;

    constexpr float gridDecibels[] = { -18.0f, -12.0f, -9.0f, -6.0f, -3.0f, 3.0f, 6.0f, 9.0f }; // as Maximus's grid
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

    // To the nearest of the grid's lines, the middle and the ends, within a few pixels of it.
    float snap (float place, float size)
    {
        auto best = place, bestDistance = snapDistance / std::max (1.0f, size);

        const auto consider = [&] (float line)
        {
            if (const auto distance = std::abs (line - place); distance <= bestDistance)
            {
                best = line;
                bestDistance = distance;
            }
        };

        for (const auto decibels : gridDecibels)
            consider (curves::axisForDecibels (decibels));

        for (const auto line : { 0.0f, 0.5f, 1.0f })
            consider (line);

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
                    "its line's shape or to delete it (Alt+click deletes it too); right-click elsewhere to add a point.");
}

void DynamicsCurve::setCurve (int firstParameter, std::optional<juce::Colour> newColour)
{
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
    repaint();
}

juce::Rectangle<float> DynamicsCurve::getArea() const
{
    return getLocalBounds().toFloat().reduced (GraphTint::handleRadius * 1.5f + 1.0f);
}

juce::Point<float> DynamicsCurve::getPosition (float across, float up) const
{
    const auto area = getArea();
    return { area.getX() + across * area.getWidth(), area.getBottom() - up * area.getHeight() };
}

float DynamicsCurve::acrossAt (float x) const
{
    return juce::jlimit (0.0f, 1.0f, (x - getArea().getX()) / getArea().getWidth());
}

float DynamicsCurve::upAt (float y) const
{
    return juce::jlimit (0.0f, 1.0f, (getArea().getBottom() - y) / getArea().getHeight());
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

    const auto middle = 0.5f * (getValue (point - 1, curves::pointX) + getValue (point, curves::pointX));
    return getPosition (middle, curves::getAt (std::span (values).subspan ((size_t) first, curves::parameterCount), middle));
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

    // The grid at Maximus's levels, the 0 dB lines through the middle stronger, and level in equalling level out.
    for (const auto decibels : gridDecibels)
    {
        const auto place = curves::axisForDecibels (decibels);
        const auto at = getPosition (place, place);
        g.setColour (p.text.withAlpha (GraphTint::grid));
        g.drawVerticalLine (juce::roundToInt (at.x), area.getY(), area.getBottom());
        g.drawHorizontalLine (juce::roundToInt (at.y), area.getX(), area.getRight());
    }

    const auto middle = getPosition (0.5f, 0.5f);
    g.setColour (p.text.withAlpha (GraphTint::zeroLine));
    g.drawVerticalLine (juce::roundToInt (middle.x), area.getY(), area.getBottom());
    g.drawHorizontalLine (juce::roundToInt (middle.y), area.getX(), area.getRight());
    g.drawLine ({ getPosition (0.0f, 0.0f), getPosition (1.0f, 1.0f) });
    g.setColour (p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText ("OUT", area.reduced (Space::xs).toNearestInt(), juce::Justification::topLeft);
    g.drawText ("IN", area.reduced (Space::xs).toNearestInt(), juce::Justification::bottomRight);

    // The curve, over a wash of the area under it, lit up to the level its compressor hears now.
    const auto curve = std::span (values).subspan ((size_t) first, curves::parameterCount);
    const auto line = graph::makeCurve (
        area, [this, area, curve] (float x) { return area.getBottom() - curves::getAt (curve, acrossAt (x)) * area.getHeight(); }, 1.0f);
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

    // Its points filled, its tension handles hollow; larger under the pointer and while dragged. Frozen, none.
    if (frozen)
        return;

    for (int i = 0; i < getCount(); ++i)
    {
        if (const auto handle = getTensionPosition (i))
            paintHandle (g, p.background, tint, { false, i }, *handle);

        paintHandle (g, p.background, tint, { true, i }, getPointPosition (i));
    }
}

void DynamicsCurve::paintHandle (juce::Graphics& g, juce::Colour background, juce::Colour tint, Target target, juce::Point<float> at) const
{
    const auto radius = target == hovered || target == dragged ? GraphTint::handleRadius * 1.5f : GraphTint::handleRadius;
    const auto dot = juce::Rectangle<float> (2.0f * radius, 2.0f * radius).withCentre (at);
    g.setColour (background);
    g.fillEllipse (dot.expanded (1.0f));
    g.setColour (tint);

    if (target.point)
        g.fillEllipse (dot);
    else
        g.drawEllipse (dot.reduced (GraphTint::handleOutline / 2.0f), GraphTint::handleOutline);
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
    const auto target = frozen ? std::optional<Target>() : findTarget (e.position);

    if (target != hovered)
    {
        hovered = target;
        setMouseCursor (! target.has_value() ? juce::MouseCursor::NormalCursor
                        : target->point      ? juce::MouseCursor::DraggingHandCursor
                                             : juce::MouseCursor::UpDownResizeCursor);
        repaint();
    }
}

void DynamicsCurve::mouseExit (const juce::MouseEvent&)
{
    hovered.reset();
    repaint();
}

void DynamicsCurve::mouseDown (const juce::MouseEvent& e)
{
    if (frozen)
        return;

    const auto target = findTarget (e.position);

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

    if (target->point)
    {
        startAcross = getValue (target->index, curves::pointX);
        startUp = getValue (target->index, curves::pointY);
        firstDragged = device.getParameter (getID (target->index, curves::pointX), "Move Point");
        secondDragged = device.getParameter (getID (target->index, curves::pointY));
        firstDragged.beginGesture(); // its gesture holds the step, which the other's joins
        secondDragged.beginGesture();
    }
    else
    {
        startUp = getValue (target->index, curves::pointTension);
        firstDragged = device.getParameter (getID (target->index, curves::pointTension), "Bend Line");
        firstDragged.beginGesture();
    }
}

void DynamicsCurve::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragged.has_value())
        return;

    const auto area = getArea();
    const auto up = (float) -e.getDistanceFromDragStartY() / std::max (1.0f, area.getHeight());
    const auto i = dragged->index;

    if (! dragged->point)
    {
        // Up bends the line up; Ctrl in fine steps.
        const auto fine = e.mods.isCtrlDown() ? fineTension : 1.0f;
        firstDragged.setValue (juce::jlimit (-1.0f, 1.0f, startUp + tensionDrag * up * fine));
        edited();
        return;
    }

    // Shift keeps it level, Ctrl keeps it where it is across; the first and last points stay at the curve's ends.
    const auto along = (float) e.getDistanceFromDragStartX() / std::max (1.0f, area.getWidth());
    const auto isEnd = i == 0 || i == getCount() - 1;
    auto across = isEnd || e.mods.isCtrlDown() ? startAcross : startAcross + along;
    auto height = e.mods.isShiftDown() ? startUp : startUp + up;

    if (snaps)
    {
        across = snap (across, area.getWidth());
        height = snap (height, area.getHeight());
    }

    const auto earliest = i > 0 ? getValue (i - 1, curves::pointX) : 0.0f;
    const auto latest = i < getCount() - 1 ? getValue (i + 1, curves::pointX) : 1.0f;
    firstDragged.setValue (isEnd ? startAcross : juce::jlimit (earliest, std::max (earliest, latest), across));
    secondDragged.setValue (juce::jlimit (0.0f, 1.0f, height));
    edited();
}

void DynamicsCurve::mouseUp (const juce::MouseEvent&)
{
    if (! dragged.has_value())
        return;

    if (secondDragged.isInGesture())
        secondDragged.endGesture();

    if (firstDragged.isInGesture())
        firstDragged.endGesture(); // its gesture holds the step: last

    dragged.reset();
    repaint();
}

void DynamicsCurve::showPointMenu (int point)
{
    juce::PopupMenu menu;
    const auto current = juce::roundToInt (getValue (point, curves::pointType));

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
    return 3 * (controlHeight + Space::s) + 2 * Space::xs;
}

void DynamicsCurveTools::resized()
{
    auto area = getLocalBounds();
    const auto width = area.getHeight() + Space::s;

    for (auto* button : { &menuButton, &freezeButton, &snapButton })
    {
        button->setBounds (area.removeFromLeft (width));
        area.removeFromLeft (Space::xs);
    }
}

void DynamicsCurveTools::showMenu()
{
    juce::PopupMenu shapes;
    const juce::Component::SafePointer<DynamicsCurveTools> safe (this);

    for (const auto& shape : curves::getShapes())
    {
        const auto name = juce::String (shape.name.data(), shape.name.size());
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
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton));
}
} // namespace phazite::ui
