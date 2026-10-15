// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "ui/theme/Icons.h"

TEST_CASE ("Every icon reads as a drawing that can be recolored")
{
    using phazite::ui::Icon;

    for (const auto icon : { Icon::play,      Icon::stop,       Icon::record, Icon::capture,   Icon::keyboard, Icon::search,   Icon::detach,
                             Icon::plus,      Icon::magnet,     Icon::loop,   Icon::metronome, Icon::grip,     Icon::expanded, Icon::folded,
                             Icon::smartTool, Icon::draw,       Icon::paint,  Icon::slice,     Icon::slip,     Icon::mute,     Icon::erase,
                             Icon::select,    Icon::zoom,       Icon::folder, Icon::addFolder, Icon::favorite, Icon::recent,   Icon::sound,
                             Icon::midi,      Icon::instrument, Icon::effect, Icon::preview,   Icon::download })
    {
        CAPTURE ((int) icon);
        const auto drawable = phazite::ui::createIcon (icon);
        REQUIRE (drawable != nullptr);
        CHECK_FALSE (drawable->getDrawableBounds().isEmpty());

        // Drawn in black, so that IconButton can swap black for the theme's color.
        CHECK (drawable->replaceColour (juce::Colours::black, juce::Colours::red));
    }
}
