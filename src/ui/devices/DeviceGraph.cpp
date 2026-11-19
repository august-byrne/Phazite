// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceGraph.h"
#include "ui/components/Controls.h"
#include "ui/components/DisplayTicker.h"
#include "ui/components/TypedField.h"
#include "ui/components/ValueText.h"
#include "ui/theme/DesignTokens.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace phazite::ui::graph
{
float FrequencyAxis::xFor (double frequency) const
{
    const auto proportion = std::log (std::max (frequency, 1.0) / low) / std::log (high / low);
    return area.getX() + (float) proportion * area.getWidth();
}

double FrequencyAxis::frequencyAt (float x) const
{
    const auto proportion = (double) ((x - area.getX()) / std::max (1.0f, area.getWidth()));
    return low * std::pow (high / low, proportion);
}

float LevelAxis::yFor (float decibels) const
{
    return area.getBottom() - (decibels - low) / (high - low) * area.getHeight();
}

float LevelAxis::levelAt (float y) const
{
    return low + (area.getBottom() - y) / std::max (1.0f, area.getHeight()) * (high - low);
}

//==============================================================================
void paintBackground (juce::Graphics& g, const Palette& p, juce::Rectangle<float> area)
{
    g.setColour (p.background);
    g.fillRoundedRectangle (area, cornerRadius);
}

void paintFrequencyGrid (juce::Graphics& g, const Palette& p, const FrequencyAxis& axis, bool withNumbers)
{
    const auto& area = axis.area;

    for (double decade = 10.0; decade < axis.high; decade *= 10.0)
    {
        for (int step = 1; step < 10; ++step)
        {
            const auto frequency = decade * step;

            if (frequency <= axis.low || frequency >= axis.high)
                continue;

            const auto x = axis.xFor (frequency);
            g.setColour (p.text.withAlpha (step == 1 ? GraphTint::grid * 2.0f : GraphTint::grid));
            g.drawVerticalLine (juce::roundToInt (x), area.getY(), area.getBottom());

            // Its number beside it, where all of it fits in the graph: in a narrow one, the last decade's would run past its
            // edge ("10 k" half drawn).
            if (withNumbers && step == 1 && frequency >= 100.0)
            {
                const juce::Font font { juce::FontOptions (TextSize::small) };
                const auto text = formatFrequency (frequency);
                const auto width = juce::GlyphArrangement::getStringWidth (font, text);

                if (x + 2.0f + width > area.getRight())
                    continue;

                g.setColour (p.textMuted);
                g.setFont (font);
                g.drawText (text,
                            juce::Rectangle<float> (x + 2.0f, area.getBottom() - TextSize::small - 2.0f, width + 1.0f, TextSize::small),
                            juce::Justification::centredLeft, false);
            }
        }
    }
}

void paintLevelGrid (juce::Graphics& g, const Palette& p, const LevelAxis& axis, float step, bool withNumbers)
{
    const auto& area = axis.area;

    for (auto level = std::ceil (axis.low / step) * step; level <= axis.high; level += step)
    {
        const auto y = axis.yFor (level);
        g.setColour (p.text.withAlpha (juce::approximatelyEqual (level, 0.0f) ? GraphTint::zeroLine : GraphTint::grid));
        g.drawHorizontalLine (juce::roundToInt (y), area.getX(), area.getRight());

        if (withNumbers && y - TextSize::small > area.getY() && y < area.getBottom() - 2.0f)
        {
            g.setColour (p.textMuted);
            g.setFont (juce::FontOptions (TextSize::small));
            g.drawText (juce::String (juce::roundToInt (level)),
                        juce::Rectangle<float> (area.getX() + 2.0f, y - TextSize::small, 40.0f, TextSize::small),
                        juce::Justification::centredLeft, false);
        }
    }
}

juce::Path makeCurve (juce::Rectangle<float> area, const std::function<float (float)>& yAt, float step)
{
    juce::Path curve;

    for (auto x = area.getX(); x <= area.getRight() + step * 0.5f; x += step)
    {
        const auto shownX = std::min (x, area.getRight());
        const juce::Point<float> point { shownX, juce::jlimit (area.getY(), area.getBottom(), yAt (shownX)) };

        // Not Path::isEmpty(), which holds while a path has nothing but its start, so that every point began a path of its own.
        if (juce::exactlyEqual (x, area.getX()))
            curve.startNewSubPath (point);
        else
            curve.lineTo (point);
    }

    return curve;
}

void paintReadout (juce::Graphics& g, const Palette& p, juce::Rectangle<float> area, const juce::String& text, juce::Justification where)
{
    if (text.isEmpty())
        return;

    g.setColour (p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText (text, area.reduced ((float) Space::xs), where, true);
}

void paintReadouts (juce::Graphics& g, const Palette& p, juce::Rectangle<float> area, const juce::String& left, const juce::String& right)
{
    const juce::Font font { juce::FontOptions (TextSize::small) };
    const auto both =
        juce::GlyphArrangement::getStringWidth (font, left) + juce::GlyphArrangement::getStringWidth (font, right) + (float) Space::m;
    paintReadout (g, p, area, left, juce::Justification::topLeft);

    if (left.isEmpty() || both <= area.reduced ((float) Space::xs).getWidth())
        paintReadout (g, p, area, right, juce::Justification::topRight);
}

juce::String formatFrequency (double hertz)
{
    if (hertz >= 1000.0)
        return juce::String (hertz / 1000.0, hertz >= 10000.0 ? 1 : 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " kHz";

    return juce::String (juce::roundToInt (hertz)) + " Hz";
}

juce::String formatDecibels (float decibels)
{
    const auto rounded = std::abs (decibels) < 10.0f ? juce::String (decibels, 1) : juce::String (juce::roundToInt (decibels));
    return (decibels > 0.0f ? "+" : "") + rounded + " dB";
}

//==============================================================================
SpectrumTrace::SpectrumTrace()
    : lefts (8192),
      rights (8192),
      ways (8192)
{
}

void SpectrumTrace::setSampleRate (double newRate)
{
    sampleRate = newRate;
    left.setSampleRate (newRate);
    right.setSampleRate (newRate);

    if (middle != nullptr)
    {
        middle->setSampleRate (newRate);
        side->setSampleRate (newRate);
    }
}

void SpectrumTrace::analyze (bool middleAndSide)
{
    if (middleAndSide == middleAndSideShown)
        return;

    middleAndSideShown = middleAndSide;

    if (! middleAndSide)
        return;

    // From silence: what they heard before they stopped is long gone by.
    if (middle == nullptr)
    {
        middle = std::make_unique<devices::Spectrum>();
        side = std::make_unique<devices::Spectrum>();
    }

    for (auto* each : { middle.get(), side.get() })
    {
        each->setSampleRate (sampleRate);
        each->clear();
    }
}

void SpectrumTrace::add (int numPulled)
{
    left.add (lefts.data(), numPulled);
    right.add (rights.data(), numPulled);

    if (! middleAndSideShown)
        return;

    for (const auto& [shown, sign] : { std::pair { middle.get(), 1.0f }, std::pair { side.get(), -1.0f } })
    {
        for (int i = 0; i < numPulled; ++i)
            ways[(size_t) i] = 0.5f * (lefts[(size_t) i] + sign * rights[(size_t) i]);

        shown->add (ways.data(), numPulled);
    }
}

void SpectrumTrace::clear()
{
    left.clear();
    right.clear();

    if (middle != nullptr)
    {
        middle->clear();
        side->clear();
    }
}

void SpectrumTrace::pull (devices::StereoTap& tap)
{
    tap.setListening (true);

    for (int n; (n = tap.pull (lefts.data(), rights.data(), (int) lefts.size())) > 0;)
        add (n);
}

void SpectrumTrace::pull (devices::SampleTap& tap)
{
    tap.setListening (true);

    for (int n; (n = tap.pull (lefts.data(), rights.data(), (int) lefts.size())) > 0;)
        add (n);
}

bool SpectrumTrace::update()
{
    const auto fall = fallPerSecond / (float) DisplayTicker::ticksPerSecond;
    auto moved = left.update (fall);
    moved = right.update (fall) || moved;

    if (middleAndSideShown)
    {
        moved = middle->update (fall) || moved;
        moved = side->update (fall) || moved;
    }

    return moved;
}

float SpectrumTrace::getRawLevelOver (double from, double to, Channels channels) const
{
    switch (channels)
    {
        case Channels::left:
            return left.getLevelOver (from, to);
        case Channels::right:
            return right.getLevelOver (from, to);
        case Channels::middle:
            return middleAndSideShown ? middle->getLevelOver (from, to) : devices::Spectrum::floorDecibels;
        case Channels::side:
            return middleAndSideShown ? side->getLevelOver (from, to) : devices::Spectrum::floorDecibels;
        case Channels::louder:
            break;
    }

    return std::max (left.getLevelOver (from, to), right.getLevelOver (from, to));
}

float SpectrumTrace::getLevelAt (double frequency, Channels channels) const
{
    const auto at = [frequency] (const devices::Spectrum& spectrum) { return spectrum.getLevelAt (frequency); };
    float level = devices::Spectrum::floorDecibels;

    switch (channels)
    {
        case Channels::left:
            level = at (left);
            break;
        case Channels::right:
            level = at (right);
            break;
        case Channels::middle:
            level = middleAndSideShown ? at (*middle) : level;
            break;
        case Channels::side:
            level = middleAndSideShown ? at (*side) : level;
            break;
        case Channels::louder:
            level = std::max (at (left), at (right));
            break;
    }

    return level + tiltPerOctave * (float) std::log2 (frequency / 1000.0);
}

float SpectrumTrace::getLevelOver (double from, double to, Channels channels) const
{
    return getRawLevelOver (from, to, channels) + tiltPerOctave * (float) std::log2 (std::sqrt (from * to) / 1000.0);
}

void SpectrumTrace::paint (juce::Graphics& g, const FrequencyAxis& frequencies, const LevelAxis& levels, juce::Colour fill,
                           juce::Colour line, Channels channels) const
{
    const auto& area = frequencies.area;
    constexpr float step = 2.0f; // pixels between the curve's points, each the loudest of the frequencies around it
    const auto curve = makeCurve (
        area,
        [&] (float x)
        {
            return levels.yFor (
                getLevelOver (frequencies.frequencyAt (x - 0.5f * step), frequencies.frequencyAt (x + 0.5f * step), channels));
        },
        step);

    auto filled = curve;
    filled.lineTo (area.getBottomRight());
    filled.lineTo (area.getBottomLeft());
    filled.closeSubPath();

    g.setColour (fill);
    g.fillPath (filled);
    g.setColour (line);
    g.strokePath (curve, juce::PathStrokeType (1.0f));
}

//==============================================================================
LevelHistory::LevelHistory (int length, float initialValue)
    : values ((size_t) std::max (2, length), initialValue),
      times (values.size(), 0.0)
{
}

void LevelHistory::push (float value)
{
    push (value, 0.0);
}

void LevelHistory::push (float value, double milliseconds)
{
    values[(size_t) next] = value;
    times[(size_t) next] = milliseconds;
    next = (next + 1) % (int) values.size();
}

float LevelHistory::get (int index) const noexcept
{
    return values[(size_t) ((next + index) % (int) values.size())];
}

double LevelHistory::getTime (int index) const noexcept
{
    return times[(size_t) ((next + index) % (int) values.size())];
}

int LevelHistory::findAt (double milliseconds) const noexcept
{
    // From the newest back, to the oldest reading taken at the moment or after it whose one before was taken before it.
    auto found = -1;

    for (int index = size() - 1; index >= 0; --index)
    {
        const auto time = getTime (index);

        if (time <= 0.0)
            break;

        if (time < milliseconds)
            return found;

        found = index;
    }

    // The oldest with a time covers its own moment alone: what came before it isn't known.
    return found >= 0 && getTime (found) <= milliseconds ? found : -1;
}

void LevelHistory::clear (float value)
{
    std::fill (values.begin(), values.end(), value);
    std::fill (times.begin(), times.end(), 0.0);
    next = 0;
}

void LevelHistory::appendTo (std::vector<float>& numbers) const
{
    numbers.insert (numbers.end(), values.begin() + next, values.end());
    numbers.insert (numbers.end(), values.begin(), values.begin() + next);
}

juce::Path LevelHistory::makeLine (juce::Rectangle<float> area, const std::function<float (float)>& yFor) const
{
    juce::Path line;
    const auto step = area.getWidth() / (float) (size() - 1);

    for (int i = 0; i < size(); ++i)
    {
        const juce::Point<float> point { area.getX() + (float) i * step, juce::jlimit (area.getY(), area.getBottom(), yFor (get (i))) };

        if (i == 0)
            line.startNewSubPath (point);
        else
            line.lineTo (point);
    }

    return line;
}

juce::Path LevelHistory::makeArea (juce::Rectangle<float> area, const std::function<float (float)>& yFor, float baseY) const
{
    auto shape = makeLine (area, yFor);
    shape.lineTo (area.getRight(), baseY);
    shape.lineTo (area.getX(), baseY);
    shape.closeSubPath();
    return shape;
}

//==============================================================================
namespace
{
    // Every graph's drag, for HandleDrag::findOf(). Message thread.
    std::vector<HandleDrag*>& getDrags()
    {
        static std::vector<HandleDrag*> all;
        return all;
    }

    juce::String describeValue (const ParameterHandle& parameter, float value)
    {
        if (const auto info = parameter.getInfo())
            return formatParameterValue (*info, value);

        return parameter.getText (value);
    }

    // Each once, in order.
    void addOnce (std::vector<ParameterHandle*>& to, ParameterHandle* parameter)
    {
        if (parameter != nullptr && std::find (to.begin(), to.end(), parameter) == to.end())
            to.push_back (parameter);
    }

    // What a handle's menu and readout show: what a drag of it moves, what the wheel turns there, and the others it names.
    std::vector<ParameterHandle*> getShown (const Handle& handle)
    {
        auto shown = handle.parameters;
        addOnce (shown, handle.wheel);

        for (auto* parameter : handle.alsoShown)
            addOnce (shown, parameter);

        return shown;
    }

    // What a double-click on it puts back: what a drag of it moves, and the others it names.
    std::vector<ParameterHandle*> getReverted (const Handle& handle)
    {
        auto reverted = handle.parameters;

        for (auto* parameter : handle.alsoReverted)
            addOnce (reverted, parameter);

        return reverted;
    }

    // The words a name begins with up to its last one ("Band 4 " of "Band 4 Frequency"), for describe() to say once.
    juce::String getLeadingWords (const juce::String& name)
    {
        return name.containsChar (' ') ? name.upToLastOccurrenceOf (" ", true, false) : juce::String();
    }
} // namespace

juce::String describe (const std::vector<ParameterHandle*>& parameters)
{
    std::vector<const ParameterHandle*> shown;

    for (const auto* parameter : parameters)
        if (parameter != nullptr && parameter->isValid())
            shown.push_back (parameter);

    if (shown.empty())
        return {};

    auto common = shown.size() > 1 ? getLeadingWords (shown.front()->getName()) : juce::String();

    for (const auto* parameter : shown)
        if (common.isNotEmpty() && ! parameter->getName().startsWith (common))
            common = {};

    juce::StringArray parts;

    if (common.isNotEmpty())
        parts.add (common.trimEnd());

    for (const auto* parameter : shown)
        parts.add (parameter->getName().substring (common.length()) + " " + describeValue (*parameter, parameter->getBaseValue()));

    return parts.joinIntoString ("  ");
}

float getFineFactor (const juce::ModifierKeys& mods)
{
    return mods.isShiftDown() ? fineDragFactor : 1.0f;
}

//==============================================================================
HandleDrag::HandleDrag (juce::Component& g)
    : graph (g)
{
    // A click on the graph leaves the keys where they were: else JUCE gives them to the first control beside it that takes
    // them (Component::grabKeyboardFocus() from a click on a component that doesn't), a knob or a button, which took them
    // from the drum sampler's pads, say.
    graph.setMouseClickGrabsKeyboardFocus (false);

    // After the graph, which takes its presses and drags first: the double-click, the right-click and the pointer's place.
    graph.addMouseListener (this, false);
    getDrags().push_back (this);
}

HandleDrag::~HandleDrag()
{
    std::erase (getDrags(), this);
    graph.removeMouseListener (this);

    valueField = nullptr; // a field left open goes without setting what it says

    // A drag in progress is the handles' to end: each ends its gesture as it goes, keeping it. The graph's wheels go first
    // (they are declared after it).
    listen (false);
}

HandleDrag* HandleDrag::findOf (const juce::Component& g)
{
    for (auto* drag : getDrags())
        if (&drag->graph == &g)
            return drag;

    return nullptr;
}

juce::Point<float> HandleDrag::getMoved (const juce::MouseEvent& e)
{
    // Each step of the pointer since the last, a fifth as far with Shift: Shift pressed or let go moves only what comes after.
    // In whole pixels, as JUCE's offset from the press is (MouseEvent::getOffsetFromDragStart()): a tremor under a pixel moves
    // nothing.
    if (! movedFromPress)
    {
        movedFromPress = true;
        lastPosition = e.getMouseDownPosition().toFloat();
        movedSoFar = {};
    }

    const auto at = e.getPosition().toFloat();
    movedSoFar += (at - lastPosition) * getFineFactor (e.mods);
    lastPosition = at;
    return movedSoFar;
}

std::optional<Handle> HandleDrag::getHandleAt (juce::Point<float> position) const
{
    return findHandle != nullptr ? findHandle (position) : std::nullopt;
}

void HandleDrag::changed()
{
    if (onCancelled != nullptr)
        onCancelled();

    graph.repaint();
}

void HandleDrag::revert (const std::vector<ParameterHandle*>& parameters)
{
    end();
    WheelBurst::endAll();

    // One step, named after the first, as its drag's is; a parameter at its default already changes nothing.
    for (auto* parameter : parameters)
        if (parameter != nullptr && parameter->isValid())
            parameter->beginGesture();

    for (auto* parameter : parameters)
        if (parameter != nullptr && parameter->isValid())
            if (const auto initial = parameter->getDefaultValue();
                initial.has_value() && ! juce::exactlyEqual (*initial, parameter->getBaseValue()))
                parameter->setValue (*initial);

    for (auto each = parameters.rbegin(); each != parameters.rend(); ++each)
        if (*each != nullptr && (*each)->isInGesture())
            (*each)->endGesture();

    changed();
}

juce::PopupMenu HandleDrag::getMenu (const Handle& handle)
{
    juce::PopupMenu menu;
    const juce::Component::SafePointer<juce::Component> safeGraph (&graph);

    for (auto* parameter : getShown (handle))
    {
        if (parameter == nullptr || ! parameter->isValid())
            continue;

        const auto section = makeParameterMenu (*parameter, graph,
                                                [this, safeGraph, parameter, at = handle.at]
                                                {
                                                    if (safeGraph != nullptr)
                                                        typeValue (*parameter, at);
                                                });

        for (juce::PopupMenu::MenuItemIterator item (section); item.next();)
            menu.addItem (item.getItem());
    }

    if (handle.addItems != nullptr)
    {
        juce::PopupMenu own;
        handle.addItems (own);

        if (own.getNumItems() > 0)
        {
            menu.addSeparator();

            for (juce::PopupMenu::MenuItemIterator item (own); item.next();)
                menu.addItem (item.getItem());
        }
    }

    return menu;
}

juce::PopupMenu HandleDrag::getMenuAt (juce::Point<float> position)
{
    const auto handle = getHandleAt (position);
    return handle.has_value() ? getMenu (*handle) : juce::PopupMenu();
}

void HandleDrag::typeValue (ParameterHandle& parameter, juce::Point<float> at)
{
    auto* parent = TypedField::findParent (graph);

    if (parent == nullptr || ! parameter.isValid() || ! graph.isShowing())
        return;

    // Over the handle, as wide as the parameter's least, most and value as its knob writes them, as a knob's field is.
    auto field = std::make_unique<TypedField> (graph);
    auto* const typed = &parameter;
    const auto range = parameter.getRange();
    field->setTitle (parameter.getName());
    field->getTarget = [this, at] (const juce::Component& in)
    { return in.getLocalArea (&graph, juce::Rectangle<int> (TypedField::getMinimumWidth(), controlHeight).withCentre (at.roundToInt())); };
    field->getTexts = [typed, range]
    {
        return juce::StringArray { describeValue (*typed, range.start), describeValue (*typed, range.end),
                                   describeValue (*typed, typed->getBaseValue()) };
    };
    field->onApply = [this, typed] (const juce::String& text)
    {
        const auto info = typed->getInfo();
        const auto value = info.has_value() ? parseParameterValue (*info, text) : typed->readText (text);

        if (! value.has_value())
            return;

        // A step of its own, as a value typed on a knob is, named as a drag of it.
        WheelBurst::endAll();
        typed->beginGesture();
        typed->setValue (*value);
        typed->endGesture();
        changed();
    };
    field->onFinished = [safeGraph = juce::Component::SafePointer<juce::Component> (&graph), this]
    {
        juce::MessageManager::callAsync (
            [safeGraph, this]
            {
                if (safeGraph == nullptr)
                    return;

                if (valueField != nullptr && valueField->isFinished())
                    valueField = nullptr;
            });
    };

    parent->addAndMakeVisible (*field);
    field->show (describeValue (parameter, parameter.getBaseValue()));
    valueField = std::move (field);
}

juce::TextEditor* HandleDrag::getValueField() const noexcept
{
    return valueField != nullptr && ! valueField->isFinished() ? valueField.get() : nullptr;
}

void HandleDrag::passWheelOn (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // As juce::Component's own passes it to its parent: past the graph's, which turns its handles.
    graph.juce::Component::mouseWheelMove (e, wheel); // NOLINT(bugprone-parent-virtual-call): the graph's own is what calls this
}

juce::String HandleDrag::getReadout() const
{
    if (taken)
        return describe (handles);

    if (! pointer.has_value())
        return {};

    const auto handle = getHandleAt (*pointer);
    return handle.has_value() ? describe (getShown (*handle)) : juce::String();
}

juce::String HandleDrag::getTooltip() const
{
    const auto handle = pointer.has_value() ? getHandleAt (*pointer) : std::nullopt;

    if (! handle.has_value())
        return {};

    juce::StringArray names;

    for (const auto* parameter : handle->parameters)
        if (parameter != nullptr && parameter->isValid())
            names.add (parameter->getName());

    const auto turns = handle->wheel != nullptr && handle->wheel->isValid();

    // The graph's own words for what is under the pointer first (the hint bar shows a tooltip in place of a description),
    // then the grammar, kept short: the bar is one line.
    auto text = graph.getDescription().trim();

    if (text.isNotEmpty() && ! text.endsWithChar ('.'))
        text << ".";

    if (text.isNotEmpty())
        text << " ";

    if (names.isEmpty())
        return turns ? text + "Wheel: " + handle->wheel->getName() + " (Shift fine). Right-click: menu" : juce::String();

    text << "Shift: fine. " << (handle->revertsOnDoubleClick ? "Double-click: default. " : "") << "Right-click: menu";

    if (turns)
        text << ". Wheel: " << handle->wheel->getName();

    return text;
}

void HandleDrag::pointAt (std::optional<juce::Point<float>> position)
{
    pointer = position;

    // The readout follows the pointer from handle to handle.
    if (findHandle == nullptr)
        return;

    if (auto readout = getReadout(); readout != shownReadout)
    {
        shownReadout = std::move (readout);
        graph.repaint();
    }
}

void HandleDrag::mouseEnter (const juce::MouseEvent& e)
{
    pointAt (e.position);
}

void HandleDrag::mouseMove (const juce::MouseEvent& e)
{
    pointAt (e.position);
}

void HandleDrag::mouseExit (const juce::MouseEvent&)
{
    pointAt (std::nullopt);
}

void HandleDrag::mouseDown (const juce::MouseEvent& e)
{
    movedFromPress = false;
    pointAt (e.position);

    if (! e.mods.isPopupMenu() || findHandle == nullptr)
        return;

    // A right-click never acts as a left click: what a graph took for it goes back, and its menu shows.
    cancel();

    if (const auto handle = getHandleAt (e.position))
        if (auto menu = getMenu (*handle); menu.getNumItems() > 0)
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&graph));
}

void HandleDrag::mouseDrag (const juce::MouseEvent& e)
{
    pointAt (e.position);
}

void HandleDrag::mouseUp (const juce::MouseEvent& e)
{
    pointAt (graph.getLocalBounds().toFloat().contains (e.position) ? std::optional (e.position) : std::nullopt);
}

void HandleDrag::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;

    if (const auto handle = getHandleAt (e.position); handle.has_value() && handle->revertsOnDoubleClick)
        revert (getReverted (*handle));
}

void HandleDrag::listen (bool shouldListen)
{
    if (window != nullptr)
        window->removeKeyListener (this);

    window = shouldListen ? graph.getTopLevelComponent() : nullptr;

    // Added last, it hears a key before the window's other listeners: its shortcuts, which use up every key while a mouse
    // button is down (KeyDispatcher), and the computer keyboard. It takes Escape alone.
    if (window != nullptr)
        window->addKeyListener (this);
}

void HandleDrag::take (std::vector<ParameterHandle*> taking)
{
    end(); // a drag that never let go (a press while another was held) keeps what it did
    movedFromPress = false;

    // A turn of the wheel still open, the graph's or a knob's, is a step of its own, which the drag doesn't join.
    WheelBurst::endAll();

    handles = std::move (taking);
    starts.clear();

    for (auto* handle : handles)
        starts.push_back (handle != nullptr ? handle->getBaseValue() : 0.0f);

    // Its step begins now, as a knob's does as it is pressed, named after the first handle, whose gesture holds it and which
    // the others join; without a change it leaves nothing.
    for (auto* handle : handles)
        if (handle != nullptr)
            handle->beginGesture();

    taken = true;
    listen (true);
}

float HandleDrag::getStart (size_t handle) const
{
    return handle < starts.size() ? starts[handle] : 0.0f;
}

void HandleDrag::set (size_t handle, float value)
{
    if (! taken || handle >= handles.size() || handles[handle] == nullptr)
        return;

    const auto range = handles[handle]->getRange();
    const auto kept = juce::jlimit (range.start, range.end, value);

    if (! juce::exactlyEqual (kept, handles[handle]->getBaseValue()))
        handles[handle]->setValue (kept);
}

void HandleDrag::moveAlong (size_t handle, float moved, const std::function<float (float)>& placeOf,
                            const std::function<float (float)>& valueAt)
{
    // Not moved along this axis: as it was, exactly, as a round trip through the axis's pixels may not give it back.
    set (handle, juce::exactlyEqual (moved, 0.0f) ? getStart (handle) : valueAt (placeOf (getStart (handle)) + moved));
}

void HandleDrag::letGo()
{
    handles.clear();
    taken = false;
    listen (false);
}

void HandleDrag::end()
{
    if (! taken)
        return;

    // The others' first, the first's last: its gesture holds the step.
    for (auto each = handles.rbegin(); each != handles.rend(); ++each)
        if (*each != nullptr)
            (*each)->endGesture();

    letGo();
}

bool HandleDrag::cancel()
{
    if (! taken)
        return false;

    // The others' first, which joined the step; then the first's, which takes the whole step back.
    for (auto each = handles.rbegin(); each != handles.rend(); ++each)
        if (*each != nullptr)
            (*each)->cancelGesture();

    letGo();

    if (onCancelled != nullptr)
        onCancelled();

    return true;
}

bool HandleDrag::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    return key == juce::KeyPress::escapeKey && cancel();
}

//==============================================================================
WheelGesture::WheelGesture (HandleDrag& graphsDrag)
    : drag (graphsDrag),
      burst (graphsDrag.graph)
{
}

void WheelGesture::turn (ParameterHandle& handle, float newValue)
{
    // While the graph's drag is taken, the wheel would land in its step, which Escape takes back.
    if (drag.isTaken())
        return;

    // Another handle of the same graph (another band's Q) is a step of its own.
    if (turning != &handle)
        end();

    turning = &handle;
    burst.turn (
        [&handle]
        {
            if (! handle.isInGesture())
                handle.beginGesture();
        },
        [this]
        {
            if (turning != nullptr && turning->isInGesture())
                turning->endGesture();

            turning = nullptr;
        });

    handle.setValue (newValue);
}
} // namespace phazite::ui::graph
