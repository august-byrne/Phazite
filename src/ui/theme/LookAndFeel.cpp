// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LookAndFeel.h"
#include "Popups.h"

#include <juce_gui_extra/juce_gui_extra.h> // KeyMappingEditorComponent

namespace phazite::ui
{
namespace
{
    // Inter ships as separate files next to the app (fonts/Inter), under the SIL Open Font License.
    juce::Typeface::Ptr loadInter (const juce::File& folder, const char* fileName)
    {
        const auto file = folder.getChildFile (fileName);
        juce::MemoryBlock data;

        if (! file.loadFileAsData (data) || data.isEmpty())
            return {};

        return juce::Typeface::createSystemTypefaceFor (data.getData(), data.getSize());
    }

    // A chevron, as VS Code's codicons draw them: pointing down (a list to pick from) or right (a submenu), this big around
    // its centre.
    void drawChevron (juce::Graphics& g, juce::Point<float> centre, float size, bool down)
    {
        const auto half = size / 2.0f, quarter = size / 4.0f;
        juce::Path chevron;

        if (down)
        {
            chevron.startNewSubPath (centre.x - half, centre.y - quarter);
            chevron.lineTo (centre.x, centre.y + quarter);
            chevron.lineTo (centre.x + half, centre.y - quarter);
        }
        else
        {
            chevron.startNewSubPath (centre.x - quarter, centre.y - half);
            chevron.lineTo (centre.x + quarter, centre.y);
            chevron.lineTo (centre.x - quarter, centre.y + half);
        }

        g.strokePath (chevron, juce::PathStrokeType (1.25f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // A tick, as VS Code's menus and tick boxes show one, inside the area.
    void drawTick (juce::Graphics& g, juce::Rectangle<float> area)
    {
        juce::Path tick;
        tick.startNewSubPath (area.getX(), area.getCentreY());
        tick.lineTo (area.getX() + area.getWidth() * 0.38f, area.getBottom() - area.getHeight() * 0.15f);
        tick.lineTo (area.getRight(), area.getY() + area.getHeight() * 0.15f);
        g.strokePath (tick, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // A rectangle's outline as a path, rounded but where it joins another button (setConnectedEdges()), as the two parts of
    // Commit do.
    juce::Path getButtonShape (const juce::Button& button, juce::Rectangle<float> area)
    {
        const auto left = ! button.isConnectedOnLeft(), right = ! button.isConnectedOnRight();
        juce::Path path;
        path.addRoundedRectangle (area.getX(), area.getY(), area.getWidth(), area.getHeight(), cornerRadius, cornerRadius, left, right,
                                  left, right);
        return path;
    }

    juce::TextLayout layOutTooltip (const juce::String& text, juce::Colour colour)
    {
        juce::AttributedString string;
        string.setJustification (juce::Justification::centredLeft);
        string.append (text, juce::FontOptions (TextSize::body), colour);

        juce::TextLayout layout;
        layout.createLayoutWithBalancedLineLengths (string, (float) Popup::tooltipMaximumWidth);
        return layout;
    }
} // namespace

LookAndFeel::LookAndFeel()
    : LookAndFeel (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("fonts").getChildFile ("Inter"))
{
}

LookAndFeel::LookAndFeel (const juce::File& interFolder)
    : regular (loadInter (interFolder, "Inter-Regular.ttf")),
      medium (loadInter (interFolder, "Inter-Medium.ttf")),
      semiBold (loadInter (interFolder, "Inter-SemiBold.ttf"))
{
    setTheme (ThemeName::dark);

    for (int type = 0; type < juce::MouseCursor::NumStandardCursorTypes; ++type)
        standardPointers.emplace_back ((juce::MouseCursor::StandardCursorType) type);
}

void LookAndFeel::setTheme (ThemeName newTheme)
{
    theme = newTheme;
    palette = phazite::ui::getPalette (theme);
    const auto& p = palette;

    setColourScheme ({ p.background, p.surface, p.surfaceRaised, p.border, p.text, p.accent, p.textOnAccent, p.accent, p.text });

    // As VS Code's: the frame behind the panels, buttons clear but for the main one (drawButtonBackground()), fields and lists to
    // pick from on the panels' surface in the control border, lists without a background of their own.
    setColour (juce::ResizableWindow::backgroundColourId, p.surface);
    setColour (juce::DocumentWindow::textColourId, p.text);
    setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    setColour (juce::TextButton::buttonOnColourId, p.accent);
    setColour (juce::TextButton::textColourOffId, p.text);
    setColour (juce::TextButton::textColourOnId, p.textOnAccent);
    setColour (juce::Label::textColourId, p.text);
    setColour (juce::TextEditor::backgroundColourId, p.surface);
    setColour (juce::TextEditor::textColourId, p.text);
    setColour (juce::TextEditor::highlightColourId, p.focus.withAlpha (0.4f));
    setColour (juce::TextEditor::outlineColourId, p.controlBorder);
    setColour (juce::TextEditor::focusedOutlineColourId, p.focus);
    setColour (juce::CaretComponent::caretColourId, p.text);
    setColour (juce::Slider::rotarySliderFillColourId, p.accent); // a knob's arc, unless its device's editor gives its colour
    setColour (juce::Slider::textBoxTextColourId, p.text);        // number fields' text (ui::NumberField)
    setColour (juce::Slider::textBoxBackgroundColourId, p.surface);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, p.focus.withAlpha (0.4f));
    setColour (juce::ListBox::backgroundColourId, p.surface);
    setColour (juce::PopupMenu::backgroundColourId, p.surfaceRaised.withAlpha (getPopupAlpha()));
    setColour (juce::PopupMenu::textColourId, p.text);
    setColour (juce::PopupMenu::headerTextColourId, p.text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, p.focus.withAlpha (Popup::selectionWash));
    setColour (juce::PopupMenu::highlightedTextColourId, p.text);
    setColour (juce::TabbedButtonBar::tabTextColourId, p.textMuted);
    setColour (juce::TabbedButtonBar::frontTextColourId, p.textBright);
    setColour (juce::TabbedComponent::backgroundColourId, p.surface);
    setColour (juce::TabbedComponent::outlineColourId, p.border);
    setColour (juce::TooltipWindow::backgroundColourId, p.surfaceRaised);
    setColour (juce::TooltipWindow::textColourId, p.text);
    setColour (juce::TooltipWindow::outlineColourId, p.border);
    setColour (juce::AlertWindow::backgroundColourId, p.surfaceRaised);
    setColour (juce::AlertWindow::textColourId, p.text);
    setColour (juce::AlertWindow::outlineColourId, p.border);
    setColour (juce::ScrollBar::thumbColourId, p.scrollThumb);
    setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
    setColour (juce::ToggleButton::textColourId, p.text);
    setColour (juce::ToggleButton::tickColourId, p.text);
    setColour (juce::ToggleButton::tickDisabledColourId, p.textMuted); // also the box's outline
    setColour (juce::ComboBox::backgroundColourId, p.surface);
    setColour (juce::ComboBox::textColourId, p.text);
    setColour (juce::ComboBox::outlineColourId, p.controlBorder);
    setColour (juce::ComboBox::arrowColourId, p.text);
    setColour (juce::ComboBox::focusedOutlineColourId, p.focus);
    setColour (juce::BubbleComponent::backgroundColourId, p.surfaceRaised); // a slider's value while dragging
    setColour (juce::BubbleComponent::outlineColourId, p.border);
    setColour (juce::KeyMappingEditorComponent::backgroundColourId, p.surface);
    setColour (juce::KeyMappingEditorComponent::textColourId, p.text);
    setColour (juce::TreeView::backgroundColourId, p.surface);
    setColour (juce::TreeView::linesColourId, p.border);
    setColour (juce::TreeView::selectedItemBackgroundColourId, p.selected);
    setColour (juce::ProgressBar::backgroundColourId, p.surfaceRaised);
    setColour (juce::ProgressBar::foregroundColourId, p.accent);
}

juce::Font LookAndFeel::getNumberFont (float height)
{
    return juce::Font (juce::FontOptions().withHeight (height).withFeatureEnabled ("tnum"));
}

juce::Typeface::Ptr LookAndFeel::getTypefaceForFont (const juce::Font& font)
{
    // Fonts that ask for a specific family keep it; everything else (the default sans-serif) is Inter.
    if (regular != nullptr && font.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
    {
        if (font.isBold())
            return semiBold != nullptr ? semiBold : regular;

        if (font.getTypefaceStyle().containsIgnoreCase ("Medium"))
            return medium != nullptr ? medium : regular;

        return regular;
    }

    return juce::LookAndFeel_V4::getTypefaceForFont (font);
}

//==============================================================================
void LookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float startAngle,
                                    float endAngle, juce::Slider& slider)
{
    const auto& p = palette;
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
    const auto centre = bounds.getCentre();
    const auto lineWidth = juce::jmax (2.0f, radius * 0.16f);
    const auto arcRadius = radius - lineWidth;
    const auto angle = startAngle + sliderPos * (endAngle - startAngle);

    const auto strokeArc = [&] (float from, float to, juce::Colour colour)
    {
        juce::Path arc;
        arc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, from, to, true);
        g.setColour (colour);
        g.strokePath (arc, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };

    strokeArc (startAngle, endAngle, p.controlBorder);

    // The value's arc starts at the bottom of the range, or in the middle of ranges such as pan.
    if (slider.isEnabled())
    {
        const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
        const auto origin = bipolar ? startAngle + (float) slider.valueToProportionOfLength (0.0) * (endAngle - startAngle) : startAngle;

        // In the colour of the device whose editor holds it (ui/devices/DeviceTint.h), else the accent.
        strokeArc (juce::jmin (origin, angle), juce::jmax (origin, angle),
                   slider.findColour (juce::Slider::rotarySliderFillColourId, true));
    }

    g.setColour (slider.isEnabled() ? p.text : p.textMuted);
    g.drawLine (juce::Line<float> (centre.getPointOnCircumference (arcRadius * 0.3f, angle),
                                   centre.getPointOnCircumference (arcRadius * 0.85f, angle)),
                lineWidth);

    if (slider.hasKeyboardFocus (false))
    {
        g.setColour (p.focus);
        g.drawEllipse (bounds.withSizeKeepingCentre (radius * 2.0f, radius * 2.0f).reduced (0.75f), 1.5f);
    }
}

void LookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                                    float maxSliderPos, juce::Slider::SliderStyle style, juce::Slider& slider)
{
    // A number field (ui::NumberField): a box as VS Code's inputs, outlined in the focus colour while it has the keyboard
    // focus. Its text box, over it, shows the value.
    if (slider.isBar())
    {
        const auto box = juce::Rectangle<int> (x, y, width, height).toFloat();
        const bool focused = slider.hasKeyboardFocus (true);
        g.setColour (palette.surface);
        g.fillRoundedRectangle (box, cornerRadius);
        g.setColour (focused ? palette.focus : palette.controlBorder);
        g.drawRoundedRectangle (box.reduced (0.5f), cornerRadius, 1.0f);
        return;
    }

    if (style != juce::Slider::LinearVertical && style != juce::Slider::LinearHorizontal)
    {
        juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const auto& p = palette;
    const bool vertical = style == juce::Slider::LinearVertical;
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    constexpr float trackThickness = 4.0f, thumbLength = 10.0f, thumbWidth = 22.0f;

    const auto track = vertical ? bounds.withSizeKeepingCentre (trackThickness, bounds.getHeight())
                                : bounds.withSizeKeepingCentre (bounds.getWidth(), trackThickness);
    g.setColour (p.controlBorder);
    g.fillRoundedRectangle (track, trackThickness / 2.0f);

    // Filled from the bottom (or left) to the value.
    g.setColour (slider.isEnabled() ? p.accent : p.textMuted);
    g.fillRoundedRectangle (vertical ? track.withTop (sliderPos) : track.withRight (sliderPos), trackThickness / 2.0f);

    // The thumb, outlined in the focus colour when it has the keyboard focus.
    const auto thumb = vertical ? juce::Rectangle<float> (juce::jmin (bounds.getWidth(), thumbWidth), thumbLength)
                                      .withCentre ({ bounds.getCentreX(), sliderPos })
                                : juce::Rectangle<float> (thumbLength, juce::jmin (bounds.getHeight(), thumbWidth))
                                      .withCentre ({ sliderPos, bounds.getCentreY() });
    const bool focused = slider.hasKeyboardFocus (false);

    g.setColour (p.surfaceRaised);
    g.fillRoundedRectangle (thumb, 2.0f);
    g.setColour (focused ? p.focus : p.textMuted);
    g.drawRoundedRectangle (thumb.reduced (0.5f), 2.0f, focused ? 2.0f : 1.0f);
}

int LookAndFeel::getSliderThumbRadius (juce::Slider& slider)
{
    if (slider.getSliderStyle() == juce::Slider::LinearVertical || slider.getSliderStyle() == juce::Slider::LinearHorizontal)
        return 6; // half the thumb, so it stays inside the fader at both ends

    return juce::LookAndFeel_V4::getSliderThumbRadius (slider);
}

juce::Label* LookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* label = juce::LookAndFeel_V4::createSliderTextBox (slider);

    if (slider.isBar())
        label->setFont (getNumberFont (TextSize::body));

    return label;
}

//==============================================================================
void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour, bool isHighlighted,
                                        bool isDown)
{
    const auto& p = palette;
    const auto bounds = button.getLocalBounds().toFloat();
    const bool focused = button.hasKeyboardFocus (false);
    const bool clear = backgroundColour.isTransparent(); // a button that isn't the main one, nor on, as VS Code's secondary ones
    const auto enabledAlpha = button.isEnabled() ? 1.0f : 0.5f;

    // While it has the keyboard focus, its outline in the focus colour at its edge, and its fill inside it, as VS Code draws its
    // focus outline outside the button.
    const auto fillArea = focused ? bounds.reduced (ButtonTint::focusGap) : bounds;

    if (clear)
    {
        if (button.isEnabled() && (isDown || isHighlighted))
        {
            g.setColour (isDown ? p.selected : p.hover);
            g.fillPath (getButtonShape (button, fillArea));
        }

        if (! focused)
        {
            g.setColour (p.controlBorder.withMultipliedAlpha (enabledAlpha));
            g.strokePath (getButtonShape (button, bounds.reduced (0.5f)), juce::PathStrokeType (1.0f));
        }
    }
    else
    {
        auto colour = backgroundColour.withMultipliedAlpha (enabledAlpha);

        if (button.isEnabled() && (isDown || isHighlighted))
            colour = isDown ? colour.darker (0.1f) : colour.brighter (0.05f);

        g.setColour (colour);
        g.fillPath (getButtonShape (button, fillArea));

        // Where two filled parts join (Commit and its menu), a faint line between them, as VS Code's split buttons have.
        if (button.isConnectedOnLeft())
        {
            g.setColour (button.findColour (juce::TextButton::textColourOnId).withAlpha (0.4f));
            g.fillRect (fillArea.withWidth (1.0f).reduced (0.0f, (float) Space::xs));
        }
    }

    if (focused)
    {
        g.setColour (p.focus);
        g.strokePath (getButtonShape (button, bounds.reduced (0.5f)), juce::PathStrokeType (1.0f));
    }
}

juce::Font LookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::FontOptions (juce::jmin (TextSize::body, (float) buttonHeight * 0.6f));
}

void LookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool isHighlighted, bool isDown)
{
    const auto boxSize = juce::jmin (ButtonTint::tickBoxSize, (float) button.getHeight() - 2.0f);
    const auto box = juce::Rectangle<float> (boxSize, boxSize).withCentre ({ boxSize / 2.0f + 1.0f, (float) button.getHeight() / 2.0f });
    drawTickBox (g, button, box.getX(), box.getY(), box.getWidth(), box.getHeight(), button.getToggleState(), button.isEnabled(),
                 isHighlighted, isDown);

    g.setColour (button.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
    g.setFont (juce::FontOptions (TextSize::body));
    g.drawFittedText (button.getButtonText(), button.getLocalBounds().withTrimmedLeft (juce::roundToInt (box.getRight()) + Space::s),
                      juce::Justification::centredLeft, 10);
}

void LookAndFeel::drawTickBox (juce::Graphics& g, juce::Component& component, float x, float y, float width, float height, bool ticked,
                               bool isEnabled, bool isHighlighted, bool)
{
    // As VS Code's tick boxes: a rounded box on the raised surface in the muted text colour (brighter under the pointer, the
    // focus colour while it has the keyboard focus), its tick in the text colour.
    const auto& p = palette;
    const auto box = juce::Rectangle<float> (x, y, width, height);
    const auto alpha = isEnabled ? 1.0f : 0.5f;

    g.setColour (p.surfaceRaised);
    g.fillRoundedRectangle (box, ButtonTint::tickBoxCorner);
    g.setColour ((component.hasKeyboardFocus (false) ? p.focus
                  : isHighlighted && isEnabled       ? p.text
                                                     : p.textMuted)
                     .withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (box.reduced (0.5f), ButtonTint::tickBoxCorner, 1.0f);

    if (ticked)
    {
        g.setColour (component.findColour (juce::ToggleButton::tickColourId).withMultipliedAlpha (alpha));
        drawTick (g, box.reduced (box.getWidth() * 0.25f));
    }
}

//==============================================================================
void LookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar& bar, int x, int y, int width, int height, bool isVertical,
                                 int thumbStart, int thumbSize, bool isMouseOver, bool isMouseDown)
{
    if (thumbSize <= 0)
        return;

    // No track behind the thumb, as VS Code's, but where a bar is given one (the playlist's, see-through over its lanes).
    if (const auto track = bar.findColour (juce::ScrollBar::trackColourId); ! track.isTransparent())
    {
        g.setColour (track);
        g.fillRoundedRectangle (juce::Rectangle<int> (x, y, width, height).toFloat(), cornerRadius);
    }

    const auto thumb =
        isVertical ? juce::Rectangle<int> (x, thumbStart, width, thumbSize) : juce::Rectangle<int> (thumbStart, y, thumbSize, height);
    const auto colour = bar.findColour (juce::ScrollBar::thumbColourId);
    g.setColour (isMouseDown   ? colour.withMultipliedAlpha (ScrollTint::dragAlpha)
                 : isMouseOver ? colour.withMultipliedAlpha (ScrollTint::hoverAlpha)
                               : colour);
    g.fillRoundedRectangle (thumb.toFloat(), cornerRadius);
}

//==============================================================================
void LookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    const auto colour = editor.findColour (juce::TextEditor::backgroundColourId);

    if (colour.isTransparent())
        return;

    g.setColour (colour);
    g.fillRoundedRectangle (juce::Rectangle<int> (width, height).toFloat(), cornerRadius);
}

void LookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    const bool focused = editor.hasKeyboardFocus (true) && ! editor.isReadOnly();
    const auto colour = editor.findColour (focused ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId);

    if (colour.isTransparent())
        return;

    g.setColour (colour.withMultipliedAlpha (editor.isEnabled() ? 1.0f : 0.5f));
    g.drawRoundedRectangle (juce::Rectangle<int> (width, height).toFloat().reduced (0.5f), cornerRadius, 1.0f);
}

void LookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<int> (width, height).toFloat();
    const auto alpha = box.isEnabled() ? 1.0f : 0.5f;

    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (bounds, cornerRadius);
    g.setColour (box.findColour (box.hasKeyboardFocus (true) ? juce::ComboBox::focusedOutlineColourId : juce::ComboBox::outlineColourId)
                     .withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (bounds.reduced (0.5f), cornerRadius, 1.0f);

    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withMultipliedAlpha (alpha));
    drawChevron (g, { bounds.getRight() - (float) Space::m, bounds.getCentreY() }, 8.0f, true);
}

juce::Font LookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return juce::FontOptions (juce::jmin (TextSize::body, (float) box.getHeight() * 0.85f));
}

void LookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (Space::xs / 2, 1, box.getWidth() - Space::xl, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

//==============================================================================
float LookAndFeel::getPopupAlpha() const noexcept
{
    return isFrosted() ? Popup::frostTint : 1.0f;
}

void LookAndFeel::setTransparencyReduced (bool reduced)
{
    transparencyReduced = reduced;
    setTheme (theme); // the popups' colours
}

void LookAndFeel::drawCallOutBoxBackground (juce::CallOutBox& box, juce::Graphics& g, const juce::Path& path, juce::Image& cachedImage)
{
    // A soft shadow; then, on frosted glass, what is behind blurred under the raised surface as a tint, or the raised
    // surface itself; outlined in the border colour, rounded as VS Code's widgets are (getCallOutBoxCornerSize()).
    if (cachedImage.isNull())
    {
        cachedImage = { juce::Image::ARGB, box.getWidth(), box.getHeight(), true };
        juce::Graphics shadow (cachedImage);
        juce::DropShadow (juce::Colours::black.withAlpha (Popup::shadowAlpha), Popup::shadowRadius, { 0, 2 }).drawForPath (shadow, path);
    }

    g.setColour (juce::Colours::black);
    g.drawImageAt (cachedImage, 0, 0);

    const auto* glass = FrostedGlass::findBehind (box);
    const auto frosted = glass != nullptr && glass->isVisible();

    if (frosted)
        glass->paintBlurred (g, path);

    g.setColour (palette.surfaceRaised.withAlpha (frosted ? getPopupAlpha() : 1.0f));
    g.fillPath (path);
    g.setColour (palette.border);
    g.strokePath (path, juce::PathStrokeType (1.0f));
}

std::unique_ptr<juce::DropShadower> LookAndFeel::createDropShadowerForComponent (juce::Component&)
{
    // Menus' and tooltips' shadows, as soft as VS Code's widgets'.
    return std::make_unique<juce::DropShadower> (
        juce::DropShadow (juce::Colours::black.withAlpha (Popup::shadowAlpha), Popup::shadowRadius, { 0, 2 }));
}

juce::Component* LookAndFeel::getParentComponentForMenuOptions (const juce::PopupMenu::Options& options)
{
    // A submenu opens where its menu is; a menu from the main window, or opened over it, inside it.
    if (auto* parent = options.getParentComponent())
        return parent;

    if (const auto* target = options.getTopLevelTargetComponent())
        return findWindowForPopups (*target);

    return findWindowForPopups (options.getTargetScreenArea().getCentre());
}

void LookAndFeel::preparePopupMenuWindow (juce::Component& menuWindow)
{
    // A menu inside a window: on frosted glass, with the shadow its window would have had on the desktop, and its keys to
    // itself.
    if (menuWindow.getParentComponent() != nullptr)
    {
        FrostedGlass::putBehind (menuWindow, FrostedGlass::Shape::rounded, true);
        keepKeysInMenu (menuWindow);
    }
}

void LookAndFeel::drawPopupMenuBackgroundWithOptions (juce::Graphics& g, int width, int height, const juce::PopupMenu::Options& options)
{
    const auto background = findColour (juce::PopupMenu::backgroundColourId);
    const auto bounds = juce::Rectangle<int> (width, height).toFloat();
    const auto inside = options.getParentComponent() != nullptr;

    // An opaque menu's window (the high contrast theme's) shows its corners, so it is square.
    if (background.isOpaque() || (! inside && ! juce::Desktop::canUseSemiTransparentWindows()))
    {
        g.fillAll (background.withAlpha (1.0f));
        g.setColour (palette.border);
        g.drawRect (bounds, 1.0f);
        return;
    }

    // Inside a window its colour is a tint over its frosted glass; on the desktop, solid.
    g.setColour (inside ? background : background.withAlpha (1.0f));
    g.fillRoundedRectangle (bounds, cardCornerRadius);
    g.setColour (palette.border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), cardCornerRadius, 1.0f);
}

void LookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                     bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                                     const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColour)
{
    const auto& p = palette;

    if (isSeparator)
    {
        g.setColour (p.border);
        g.fillRect (area.toFloat().withSizeKeepingCentre ((float) area.getWidth() - 2.0f * (float) Popup::itemInset, 1.0f));
        return;
    }

    const auto item = area.toFloat();

    if (isHighlighted && isActive)
    {
        g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
        g.fillRoundedRectangle (item, itemCornerRadius);
        g.setColour (p.focus);
        g.drawRoundedRectangle (item.reduced (0.5f), itemCornerRadius, 1.0f);
    }

    const auto colour = (textColour != nullptr ? *textColour : findColour (juce::PopupMenu::textColourId));
    const auto ink = isActive ? colour : p.textMuted;
    auto content = item.reduced ((float) Popup::itemTextInset, 0.0f);

    // A tick in the item's left margin, as VS Code's menus put theirs; an icon before the text.
    if (icon != nullptr)
    {
        icon->drawWithin (g, content.removeFromLeft (TextSize::body).withSizeKeepingCentre (TextSize::body, TextSize::body),
                          juce::RectanglePlacement::centred, isActive ? 1.0f : 0.5f);
        content.removeFromLeft ((float) Space::xs);
    }
    else if (isTicked)
    {
        g.setColour (ink);
        drawTick (g, item.withWidth ((float) Popup::itemTextInset).withSizeKeepingCentre (9.0f, 9.0f).translated (1.0f, 0.0f));
    }

    if (hasSubMenu)
    {
        g.setColour (ink);
        drawChevron (g, { content.getRight() - 3.0f, content.getCentreY() }, 8.0f, false);
        content.removeFromRight (TextSize::small);
    }

    const auto font = getPopupMenuFont();
    g.setFont (font);

    if (shortcutKeyText.isNotEmpty())
    {
        g.setColour (ink.withMultipliedAlpha (Popup::shortcutAlpha));
        g.drawText (shortcutKeyText, content, juce::Justification::centredRight, true);
        content.removeFromRight (juce::TextLayout::getStringWidth (font, shortcutKeyText) + (float) Popup::itemTextInset);
    }

    g.setColour (ink);
    g.drawFittedText (text, content.toNearestInt(), juce::Justification::centredLeft, 1);
}

void LookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName)
{
    // As VS Code's menus' group titles: in semibold, a little lower in its row.
    g.setFont (getPopupMenuFont().boldened());
    g.setColour (findColour (juce::PopupMenu::headerTextColourId));
    g.drawFittedText (sectionName, area.reduced (Popup::itemTextInset, 0).withTrimmedTop (Space::xs), juce::Justification::centredLeft, 1);
}

void LookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth,
                                             int& idealHeight)
{
    if (isSeparator)
    {
        idealWidth = 50;
        idealHeight = Popup::separatorHeight;
        return;
    }

    // The text JUCE measures holds the shortcut too; room for a submenu's chevron after it; at least as wide as VS Code's menus.
    idealHeight = standardMenuItemHeight > 0 ? standardMenuItemHeight : Popup::itemHeight;
    idealWidth =
        juce::jmax (Popup::menuMinimumWidth - 2 * Popup::itemInset,
                    juce::roundToInt (juce::TextLayout::getStringWidth (getPopupMenuFont(), text)) + 2 * Popup::itemTextInset + Space::xl);
}

juce::Font LookAndFeel::getPopupMenuFont()
{
    return juce::FontOptions (TextSize::small);
}

//==============================================================================
void LookAndFeel::drawMenuBarBackground (juce::Graphics& g, int width, int height, bool, juce::MenuBarComponent& menuBar)
{
    if (menuBar.getProperties()[onBarProperty])
        return; // the bar behind it paints its background

    g.setColour (palette.surfaceRaised);
    g.fillRect (0, 0, width, height);
}

void LookAndFeel::drawMenuBarItem (juce::Graphics& g, int width, int height, int itemIndex, const juce::String& itemText,
                                   bool isMouseOverItem, bool isMenuOpen, bool, juce::MenuBarComponent& menuBar)
{
    // As VS Code's menu bar: each name in a rounded box under the pointer and while its menu is open, as tall as its status
    // bar's items.
    if (menuBar.isEnabled() && (isMouseOverItem || isMenuOpen))
    {
        g.setColour (palette.hover);
        g.fillRoundedRectangle (juce::Rectangle<int> (width, height)
                                    .toFloat()
                                    .withSizeKeepingCentre ((float) width, (float) juce::jmin (height, Shell::statusItemHeight)),
                                cornerRadius);
    }

    g.setColour (menuBar.isEnabled() ? palette.text : palette.textMuted);
    g.setFont (getMenuBarFont (menuBar, itemIndex, itemText));
    g.drawFittedText (itemText, 0, 0, width, height, juce::Justification::centred, 1);
}

juce::Font LookAndFeel::getMenuBarFont (juce::MenuBarComponent&, int, const juce::String&)
{
    return juce::Font (juce::FontOptions (TextSize::body));
}

int LookAndFeel::getMenuBarItemWidth (juce::MenuBarComponent& menuBar, int itemIndex, const juce::String& itemText)
{
    return juce::roundToInt (juce::GlyphArrangement::getStringWidth (getMenuBarFont (menuBar, itemIndex, itemText), itemText))
           + 2 * Space::s;
}

//==============================================================================
juce::Rectangle<int> LookAndFeel::getTooltipBounds (const juce::String& text, juce::Point<int> screenPosition,
                                                    juce::Rectangle<int> parentArea)
{
    // Beside the pointer, on the side of the screen with more room.
    const auto layout = layOutTooltip (text, juce::Colours::black);
    const auto width = (int) std::ceil (layout.getWidth()) + 2 * Popup::tooltipPaddingX;
    const auto height = (int) std::ceil (layout.getHeight()) + 2 * Popup::tooltipPaddingY;

    return juce::Rectangle<int> (screenPosition.x > parentArea.getCentreX() ? screenPosition.x - (width + Popup::tooltipOffset)
                                                                            : screenPosition.x + 2 * Popup::tooltipOffset,
                                 screenPosition.y > parentArea.getCentreY() ? screenPosition.y - (height + Popup::tooltipOffset / 2)
                                                                            : screenPosition.y + Popup::tooltipOffset / 2,
                                 width, height)
        .constrainedWithin (parentArea);
}

void LookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    paintTooltip (g, text, juce::Rectangle<int> (width, height).toFloat(), false);
}

void LookAndFeel::paintTooltip (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> bounds, bool frosted)
{
    g.setColour (findColour (juce::TooltipWindow::backgroundColourId).withAlpha (frosted ? getPopupAlpha() : 1.0f));
    g.fillRoundedRectangle (bounds, cardCornerRadius);
    g.setColour (findColour (juce::TooltipWindow::outlineColourId));
    g.drawRoundedRectangle (bounds.reduced (0.5f), cardCornerRadius, 1.0f);

    layOutTooltip (text, findColour (juce::TooltipWindow::textColourId))
        .draw (g, bounds.reduced ((float) Popup::tooltipPaddingX, (float) Popup::tooltipPaddingY));
}

//==============================================================================
int LookAndFeel::getTabButtonBestWidth (juce::TabBarButton& button, int tabDepth)
{
    auto width =
        juce::roundToInt (juce::TextLayout::getStringWidth (getTabButtonFont (button, (float) tabDepth), button.getButtonText().trim()))
        + 2 * (TabTint::textInset + (int) TabTint::pillInset);

    if (auto* extra = button.getExtraComponent())
        width += button.getTabbedButtonBar().isVertical() ? extra->getHeight() : extra->getWidth();

    return juce::jlimit (tabDepth, tabDepth * 8, width);
}

void LookAndFeel::drawTabButton (juce::TabBarButton& button, juce::Graphics& g, bool isMouseOver, bool isMouseDown)
{
    // Tabs down a side keep JUCE's look.
    if (button.getTabbedButtonBar().isVertical())
    {
        juce::LookAndFeel_V4::drawTabButton (button, g, isMouseOver, isMouseDown);
        return;
    }

    const auto& p = palette;
    const auto front = button.isFrontTab();
    const auto area = button.getActiveArea().toFloat();
    const auto pill =
        area.reduced (TabTint::pillInset, 0.0f)
            .withSizeKeepingCentre (area.getWidth() - 2.0f * TabTint::pillInset, juce::jmin (TabTint::pillHeight, area.getHeight()));

    if (front || isMouseOver || isMouseDown)
    {
        g.setColour (front ? p.selectedInactive : p.hover);
        g.fillRoundedRectangle (pill, cornerRadius);
    }

    if (button.hasKeyboardFocus (false))
    {
        g.setColour (p.focus);
        g.drawRoundedRectangle (pill.reduced (0.5f), cornerRadius, 1.0f);
    }

    g.setColour (front ? p.textBright : isMouseOver ? p.text : p.textMuted);
    g.setFont (getTabButtonFont (button, pill.getHeight()));
    g.drawText (button.getButtonText().trim(), pill, juce::Justification::centred, false);
}

juce::Font LookAndFeel::getTabButtonFont (juce::TabBarButton&, float)
{
    return juce::FontOptions (TextSize::body);
}

//==============================================================================
const Palette& getPalette (const juce::Component& component)
{
    if (auto* lookAndFeel = dynamic_cast<const LookAndFeel*> (&component.getLookAndFeel()))
        return lookAndFeel->getPalette();

    static const auto fallback = getPalette (ThemeName::dark);
    return fallback;
}

//==============================================================================
namespace
{
    // The palette's colours by their places, which a component keeps of a colour set by role (setThemeColour()).
    constexpr juce::Colour Palette::* paletteRoles[] = {
        &Palette::background,    &Palette::surface,
        &Palette::surfaceRaised, &Palette::border,
        &Palette::controlBorder, &Palette::text,
        &Palette::textMuted,     &Palette::textBright,
        &Palette::accent,        &Palette::textOnAccent,
        &Palette::focus,         &Palette::hover,
        &Palette::selected,      &Palette::selectedInactive,
        &Palette::scrollThumb,   &Palette::indentGuide,
        &Palette::warning,       &Palette::danger,
        &Palette::loop,          &Palette::modulation,
        &Palette::frozen,        &Palette::boost,
        &Palette::playing,       &Palette::soloed,
    };
    static_assert (sizeof (Palette) == std::size (paletteRoles) * sizeof (juce::Colour), "paletteRoles lists every colour of Palette's");

    // The colours a component has set by role, each [its colour ID, its role's place, its alpha, the colour it set].
    const juce::Identifier themeColoursProperty { "phaziteThemeColours" };

    void applyThemeColour (juce::Component& component, juce::var& entry)
    {
        auto& fields = *entry.getArray();
        const auto& role = getPalette (component).*paletteRoles[(int) fields[1]];
        const auto colour = role.withMultipliedAlpha ((float) (double) fields[2]);
        component.setColour ((int) fields[0], colour);
        fields.set (3, (juce::int64) colour.getARGB());
    }

    void refreshThemeIn (juce::Component& component)
    {
        if (auto* entries = component.getProperties().getVarPointer (themeColoursProperty); entries != nullptr && entries->isArray())
            for (auto& entry : *entries->getArray())
                if (const auto colourID = (int) entry[0];
                    component.isColourSpecified (colourID)
                    && (juce::int64) component.findColour (colourID).getARGB() == (juce::int64) entry[3])
                    applyThemeColour (component, entry);

        if (auto* field = dynamic_cast<juce::TextEditor*> (&component))
        {
            field->applyColourToAllText (field->findColour (juce::TextEditor::textColourId), true);

            if (const auto placeholder = field->getTextToShowWhenEmpty(); placeholder.isNotEmpty())
                field->setTextToShowWhenEmpty (placeholder, getPalette (*field).textMuted);
        }

        for (auto* child : component.getChildren())
            refreshThemeIn (*child);
    }
} // namespace

void setThemeColour (juce::Component& component, int colourId, juce::Colour Palette::* role, float alpha)
{
    const auto* found = std::find (std::begin (paletteRoles), std::end (paletteRoles), role);
    jassert (found != std::end (paletteRoles));
    auto& properties = component.getProperties();

    if (! properties.contains (themeColoursProperty))
        properties.set (themeColoursProperty, juce::Array<juce::var>());

    auto& entries = *properties.getVarPointer (themeColoursProperty)->getArray();
    juce::var* entry = nullptr;

    for (auto& each : entries)
        if ((int) each[0] == colourId)
            entry = &each;

    if (entry == nullptr)
    {
        entries.add (juce::Array<juce::var> { colourId, 0, 1.0, 0 });
        entry = &entries.getReference (entries.size() - 1);
    }

    entry->getArray()->set (1, (int) std::distance (std::begin (paletteRoles), found));
    entry->getArray()->set (2, (double) alpha);
    applyThemeColour (component, *entry);
}

void refreshTheme (juce::Component& window)
{
    refreshThemeIn (window);
    window.sendLookAndFeelChange();
}

juce::Colour drawListRow (juce::Graphics& g, const juce::Component& list, juce::Rectangle<float> row, bool selected, bool hovered,
                          bool listHasFocus)
{
    const auto& p = getPalette (list);

    if (selected || hovered)
    {
        g.setColour (selected ? (listHasFocus ? p.selected : p.selectedInactive) : p.hover);
        g.fillRoundedRectangle (row.reduced (ListRow::inset, 0.0f), cornerRadius);
    }

    return selected ? p.textBright : p.text;
}

float drawKeyCaps (juce::Graphics& g, const Palette& p, const juce::String& shortcut, juce::Rectangle<float> area, juce::Colour text)
{
    // From the last key leftwards ("Ctrl+Shift+P": P, then Shift, then Ctrl); a key that is "+" itself stays one.
    auto keys = juce::StringArray::fromTokens (shortcut, "+", {});
    keys.removeEmptyStrings();

    if (shortcut.endsWith ("++") || shortcut == "+")
        keys.add ("+");

    const auto font = juce::Font (juce::FontOptions (KeyCap::textSize));
    g.setFont (font);
    auto right = area.getRight();

    for (int i = keys.size(); --i >= 0;)
    {
        const auto key = keys[i].trim();
        const auto width = juce::jmax (KeyCap::height, juce::TextLayout::getStringWidth (font, key) + 2.0f * KeyCap::padding);
        const auto box = juce::Rectangle<float> (right - width, area.getCentreY() - KeyCap::height / 2.0f, width, KeyCap::height);

        g.setColour (p.textMuted.withAlpha (KeyCap::fill));
        g.fillRoundedRectangle (box, KeyCap::corner);
        g.setColour (p.controlBorder);
        g.drawRoundedRectangle (box.reduced (0.5f), KeyCap::corner, 1.0f);
        g.setColour (text);
        g.drawText (key, box, juce::Justification::centred, false);

        right = box.getX() - KeyCap::gap;
    }

    return area.getRight() - right - KeyCap::gap;
}

float snapToPixels (juce::Graphics& g, float position)
{
    const auto scale = std::max (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    return std::round (position * scale) / scale;
}

juce::Rectangle<float> snapToPixels (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto left = snapToPixels (g, area.getX()), top = snapToPixels (g, area.getY());
    const auto right = snapToPixels (g, area.getRight()), bottom = snapToPixels (g, area.getBottom());
    return { left, top, std::max (0.0f, right - left), std::max (0.0f, bottom - top) };
}
} // namespace phazite::ui
