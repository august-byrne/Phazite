// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PaintCheck.h"

#include <cmath>

namespace phazite::test
{
juce::Image paintComponent (juce::Component& component, float scale)
{
    const auto width = std::max (1, (int) std::ceil ((float) component.getWidth() * scale));
    const auto height = std::max (1, (int) std::ceil ((float) component.getHeight() * scale));

    // Cleared, unlike a snapshot of an opaque component (Component::createComponentSnapshot()), so that what it leaves
    // unpainted reads as nothing.
    juce::Image image (juce::Image::ARGB, width, height, true, juce::SoftwareImageType());
    juce::Graphics g (image);
    g.addTransform (juce::AffineTransform::scale (scale));
    component.paintEntireComponent (g, true);
    return image;
}

bool drawsSomething (const juce::Image& image)
{
    const juce::Image::BitmapData pixels (image, juce::Image::BitmapData::readOnly);

    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
            if (pixels.getPixelColour (x, y).getAlpha() != 0)
                return true;

    return false;
}

void checkPaints (juce::Component& component, const juce::String& what, float scale)
{
    INFO ("painting " << what << " at " << component.getWidth() << " x " << component.getHeight() << ", scale " << scale);
    REQUIRE_FALSE (component.getLocalBounds().isEmpty());
    CHECK (drawsSomething (paintComponent (component, scale)));
}
} // namespace phazite::test
