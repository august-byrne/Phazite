// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "ui/theme/Icons.h"

#include <cmath>

TEST_CASE ("Every icon reads as a drawing that can be recolored")
{
    using phazite::ui::Icon;

    for (const auto icon :
         { Icon::play,      Icon::stop,   Icon::record,     Icon::capture,    Icon::keyboard,    Icon::search,    Icon::detach,
           Icon::plus,      Icon::magnet, Icon::loop,       Icon::metronome,  Icon::collapseAll, Icon::expanded,  Icon::folded,
           Icon::smartTool, Icon::draw,   Icon::paint,      Icon::slice,      Icon::slip,        Icon::mute,      Icon::erase,
           Icon::select,    Icon::zoom,   Icon::folder,     Icon::openFolder, Icon::expandAll,   Icon::addFolder, Icon::favorite,
           Icon::recent,    Icon::sound,  Icon::midi,       Icon::instrument, Icon::effect,      Icon::preview,   Icon::download,
           Icon::close,     Icon::frozen, Icon::lowLatency, Icon::alias,      Icon::normalize })
    {
        CAPTURE ((int) icon);
        const auto drawable = phazite::ui::createIcon (icon);
        REQUIRE (drawable != nullptr);
        CHECK_FALSE (drawable->getDrawableBounds().isEmpty());

        // Drawn in black, so that IconButton can swap black for the theme's color.
        CHECK (drawable->replaceColour (juce::Colours::black, juce::Colours::red));
    }
}

TEST_CASE ("Phazite's logo reads as a drawing in its own colours, a hexagon with a glowing face, and as the window's icon")
{
    const auto logo = phazite::ui::createLogo();
    REQUIRE (logo != nullptr);
    // A hexagon standing on a point, as tall as the app's icon's largest size, which juceaide makes only from a drawing at
    // least that big (CMakeLists.txt).
    const auto bounds = logo->getDrawableBounds();
    CHECK (bounds.getY() == doctest::Approx (0.0f));
    CHECK (bounds.getHeight() == doctest::Approx (256.0f));
    CHECK (bounds.getWidth() / bounds.getHeight() == doctest::Approx (std::sqrt (3.0f) / 2.0f).epsilon (0.001));

    // In its own colours, whatever the theme: transparent around it, a frame of blue faces, and a smaller cube in it in
    // darker, blue-tinted faces.
    const auto image = phazite::ui::createLogoImage (64);
    REQUIRE (image.isValid());
    CHECK (image.getWidth() == 64);
    CHECK (image.getHeight() == 64);
    CHECK (image.getPixelAt (0, 0).getAlpha() == 0); // outside the hexagon: the corner
    const auto frame = image.getPixelAt (10, 32);    // the frame's left face
    CHECK (frame.getAlpha() == 255);
    CHECK (frame.getBlue() > frame.getRed() + 100);
    const auto cube = image.getPixelAt (20, 32); // the cube's left face
    CHECK (cube.getAlpha() == 255);
    CHECK (cube.getBrightness() < frame.getBrightness() - 0.4f);
    const auto cubeTop = image.getPixelAt (32, 22); // the cube's top
    CHECK (cubeTop.getBlue() > cubeTop.getRed() + 60);
    CHECK_FALSE (phazite::ui::createLogoImage (0).isValid());
}
