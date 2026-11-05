// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Popups.h"

#include "DesignTokens.h"
#include "LookAndFeel.h"

#include <array>
#include <cmath>

namespace phazite::ui
{
namespace
{
    constexpr const char* popupsInsideProperty = "phazitePopupsInside";

    // The radii of three box blurs in a row that blur as a Gaussian of this standard deviation does. A box of radius r adds
    // r (r + 1) / 3 to the variance: the largest radius that stays under it, and as many passes one wider as come nearest.
    std::array<int, 3> getBoxRadii (float standardDeviation)
    {
        const auto variance = (double) standardDeviation * standardDeviation;
        const auto r = juce::jmax (0, (int) std::floor ((std::sqrt (1.0 + 4.0 * variance) - 1.0) / 2.0));
        const auto wider = juce::jlimit (0, 3, (int) std::lround ((variance - r * (r + 1.0)) * 3.0 / (2.0 * (r + 1.0))));
        return { wider > 0 ? r + 1 : r, wider > 1 ? r + 1 : r, wider > 2 ? r + 1 : r };
    }

    // One box blur along a line of pixels, each `step` bytes after the one before, its ends repeated beyond it.
    void blurLine (juce::uint8* first, int count, int step, int radius, std::vector<int>& line)
    {
        if (radius <= 0 || count <= 1)
            return;

        for (int i = 0; i < count; ++i)
            for (int c = 0; c < 4; ++c)
                line[(size_t) (i * 4 + c)] = first[i * step + c];

        const auto at = [count] (int i) { return (size_t) juce::jlimit (0, count - 1, i) * 4; };
        const auto width = 2 * radius + 1;

        for (int c = 0; c < 4; ++c)
        {
            auto sum = 0;

            for (int i = -radius; i <= radius; ++i)
                sum += line[at (i) + (size_t) c];

            for (int i = 0; i < count; ++i)
            {
                first[i * step + c] = (juce::uint8) ((sum + width / 2) / width);
                sum += line[at (i + radius + 1) + (size_t) c] - line[at (i - radius) + (size_t) c];
            }
        }
    }

    // keepKeysInMenu(): every key the menu's, whether it uses it or not.
    struct MenuKeys final : juce::KeyListener
    {
        bool keyPressed (const juce::KeyPress& key, juce::Component* menu) override
        {
            if (menu != nullptr)
                menu->keyPressed (key);

            return true;
        }

        bool keyStateChanged (bool, juce::Component*) override { return true; }
    };
} // namespace

//==============================================================================
bool isTransparencyReducedBySystem()
{
#if JUCE_WINDOWS
    return juce::WindowsRegistry::getValue (
               "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize\\EnableTransparency", "1")
               .getIntValue()
           == 0;
#else
    return false; // no other system's setting is read yet
#endif
}

void setPopupsInside (juce::Component& window)
{
    window.getProperties().set (popupsInsideProperty, true);
}

juce::Component* findWindowForPopups (const juce::Component& component)
{
    auto* window = component.getTopLevelComponent();
    return window != nullptr && (bool) window->getProperties()[popupsInsideProperty] ? window : nullptr;
}

juce::Component* findWindowForPopups (juce::Point<int> screenPosition)
{
    const auto* component = juce::Desktop::getInstance().findComponentAt (screenPosition);
    return component != nullptr ? findWindowForPopups (*component) : nullptr;
}

juce::Image paintBehind (const juce::Component& front, juce::Rectangle<int> area, float scale)
{
    juce::Image image (juce::Image::ARGB, juce::jmax (1, (int) std::ceil ((float) area.getWidth() * scale)),
                       juce::jmax (1, (int) std::ceil ((float) area.getHeight() * scale)), true, juce::SoftwareImageType());
    auto* parent = front.getParentComponent();

    if (parent == nullptr)
        return image;

    juce::Graphics g (image);
    g.addTransform (juce::AffineTransform::translation ((float) -area.getX(), (float) -area.getY()).scaled (scale));
    g.reduceClipRegion (area);

    {
        const juce::Graphics::ScopedSaveState state (g);
        parent->paint (g);
    }

    // The children in the order the parent paints them, up to the front one.
    for (auto* child : parent->getChildren())
    {
        if (child == &front)
            break;

        if (! child->isVisible() || ! child->getBounds().intersects (area))
            continue;

        const juce::Graphics::ScopedSaveState state (g);

        if (child->isTransformed())
            g.addTransform (child->getTransform());

        g.setOrigin (child->getPosition());
        g.reduceClipRegion (child->getLocalBounds());
        child->paintEntireComponent (g, false);
    }

    return image;
}

juce::Image shrink (const juce::Image& source, int factor)
{
    jassert (source.getFormat() == juce::Image::ARGB && factor > 0);
    const auto width = (source.getWidth() + factor - 1) / factor, height = (source.getHeight() + factor - 1) / factor;
    juce::Image result (juce::Image::ARGB, juce::jmax (1, width), juce::jmax (1, height), true, juce::SoftwareImageType());
    const juce::Image::BitmapData from (source, juce::Image::BitmapData::readOnly);
    const juce::Image::BitmapData to (result, juce::Image::BitmapData::writeOnly);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            std::array<int, 4> sums {};
            auto count = 0;

            for (int sourceY = y * factor; sourceY < juce::jmin ((y + 1) * factor, source.getHeight()); ++sourceY)
            {
                for (int sourceX = x * factor; sourceX < juce::jmin ((x + 1) * factor, source.getWidth()); ++sourceX, ++count)
                {
                    const auto* in = from.getPixelPointer (sourceX, sourceY);

                    for (size_t c = 0; c < 4; ++c)
                        sums[c] += in[c];
                }
            }

            auto* out = to.getPixelPointer (x, y);

            for (size_t c = 0; c < 4; ++c)
                out[c] = (juce::uint8) ((sums[c] + count / 2) / juce::jmax (1, count));
        }
    }

    return result;
}

void blur (juce::Image& image, float standardDeviation)
{
    jassert (image.isNull() || image.getFormat() == juce::Image::ARGB);

    if (image.isNull() || standardDeviation <= 0.0f)
        return;

    const juce::Image::BitmapData data (image, juce::Image::BitmapData::readWrite);
    std::vector<int> line ((size_t) juce::jmax (image.getWidth(), image.getHeight()) * 4);

    for (const auto radius : getBoxRadii (standardDeviation))
    {
        for (int y = 0; y < image.getHeight(); ++y)
            blurLine (data.getLinePointer (y), image.getWidth(), data.pixelStride, radius, line);

        for (int x = 0; x < image.getWidth(); ++x)
            blurLine (data.getPixelPointer (x, 0), image.getHeight(), data.lineStride, radius, line);
    }
}

//==============================================================================
FrostedGlass& FrostedGlass::putBehind (juce::Component& popup, Shape shape, bool withShadow)
{
    jassert (popup.getParentComponent() != nullptr);

    if (auto* glass = findBehind (popup))
        return *glass;

    // Deleted with the popup (componentBeingDeleted()).
    auto* glass = new FrostedGlass (popup, shape);

    if (withShadow)
        if ((glass->shadower = popup.getLookAndFeel().createDropShadowerForComponent (popup)) != nullptr)
            glass->shadower->setOwner (&popup);

    return *glass;
}

FrostedGlass* FrostedGlass::findBehind (const juce::Component& popup)
{
    if (const auto* parent = popup.getParentComponent())
        for (auto* child : parent->getChildren())
            if (auto* glass = dynamic_cast<FrostedGlass*> (child); glass != nullptr && &glass->popup == &popup)
                return glass;

    return nullptr;
}

FrostedGlass::FrostedGlass (juce::Component& p, Shape s)
    : popup (p),
      shape (s)
{
    setOpaque (true);
    setInterceptsMouseClicks (false, false);
    setAccessible (false);
    popup.getParentComponent()->addChildComponent (this);
    popup.addComponentListener (this);
    follow();
    take();
}

FrostedGlass::~FrostedGlass()
{
    popup.removeComponentListener (this);
}

bool FrostedGlass::isWanted() const
{
    const auto* look = dynamic_cast<const LookAndFeel*> (&popup.getLookAndFeel());
    return look != nullptr && look->isFrosted();
}

void FrostedGlass::follow()
{
    setAlwaysOnTop (popup.isAlwaysOnTop());
    setBounds (popup.getBounds());
    setVisible (popup.isVisible() && isWanted());
    toBehind (&popup);
}

void FrostedGlass::take()
{
    sharp = {};
    blurred = {};
    auto* parent = getParentComponent();

    if (parent != nullptr && isVisible() && ! getBounds().isEmpty())
    {
        // What is behind, at the screen's resolution and a little beyond the popup, so that the blur takes in what is
        // around it: the popup's part as it is, and the whole shrunk, which the blur hides, and blurred.
        const auto scale = juce::Component::getApproximateScaleFactorForComponent (this);
        const auto factor = juce::roundToInt (1.0f / Popup::frostResolution);
        const auto area = getBounds().expanded (juce::roundToInt (Popup::frostBlur * 2.0f)).getIntersection (parent->getLocalBounds());
        const auto whole = paintBehind (*this, area, scale);
        const auto inside =
            ((getBounds() - area.getPosition()).toFloat() * scale).getSmallestIntegerContainer().getIntersection (whole.getBounds());

        sharp = juce::NativeImageType().convert (whole.getClippedImage (inside));
        auto shrunk = shrink (whole, factor);
        blur (shrunk, Popup::frostBlur * scale / (float) factor);
        blurred = juce::NativeImageType().convert (shrunk);
        blurredArea = area - getPosition();
    }

    repaint();
}

void FrostedGlass::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    if (sharp.isValid())
        g.drawImage (sharp, bounds);
    else
        g.fillAll (getPalette (*this).surfaceRaised);

    if (shape == Shape::rounded)
    {
        juce::Path rounded;
        rounded.addRoundedRectangle (bounds, cardCornerRadius);
        paintBlurred (g, rounded);
    }
}

void FrostedGlass::paintBlurred (juce::Graphics& g, const juce::Path& inside) const
{
    if (! blurred.isValid())
        return;

    const juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (inside);
    g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
    g.drawImage (blurred, blurredArea.toFloat());
}

void FrostedGlass::componentMovedOrResized (juce::Component&, bool, bool)
{
    follow();
    take();
}

void FrostedGlass::componentVisibilityChanged (juce::Component&)
{
    follow();
    take();
}

void FrostedGlass::componentBroughtToFront (juce::Component&)
{
    follow();
}

void FrostedGlass::componentBeingDeleted (juce::Component&)
{
    delete this;
}

//==============================================================================
void keepKeysInMenu (juce::Component& menuWindow)
{
    static MenuKeys keys;
    menuWindow.addKeyListener (&keys);
}

//==============================================================================
Tooltips::Tooltips (juce::Component& window, bool in)
    : juce::TooltipWindow (in ? &window : nullptr),
      mainWindow (window),
      inside (in)
{
    setOpaque (false); // rounded, its corners showing what is behind

    if (inside)
        FrostedGlass::putBehind (*this, FrostedGlass::Shape::rounded, true);
}

juce::String Tooltips::getTipFor (juce::Component& component)
{
    if (! inside && component.getTopLevelComponent() == &mainWindow)
        return {};

    tip = juce::TooltipWindow::getTipFor (component);
    return tip;
}

void Tooltips::paint (juce::Graphics& g)
{
    const auto* glass = FrostedGlass::findBehind (*this);

    if (auto* look = dynamic_cast<LookAndFeel*> (&getLookAndFeel()))
        look->paintTooltip (g, tip, getLocalBounds().toFloat(), glass != nullptr && glass->isVisible());
    else
        getLookAndFeel().drawTooltip (g, tip, getWidth(), getHeight());
}
} // namespace phazite::ui
