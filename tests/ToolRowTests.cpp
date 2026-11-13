// SPDX-License-Identifier: AGPL-3.0-or-later
// A row of a view's controls in its area's tab row (src/ui/components/ToolRow.h): laid out from the left, and where it is too
// narrow, the least needed of them go into its More menu, so that nothing drops off its edge.
#include "UiTestHelpers.h"
#include "ui/components/ToolRow.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/timeline/TimelineEditor.h"

#include <vector>

namespace
{
// A row of four buttons, 24 pixels each with 2 between them: the first needed most, the third least.
struct Row
{
    Row()
    {
        for (int i = 0; i < 4; ++i)
        {
            auto& button = buttons.emplace_back (std::make_unique<juce::TextButton> (juce::String (i)));
            row.add ({ .control = button.get(),
                       .getWidth = [] { return phazite::ui::ToolRowTint::button; },
                       .addToMenu = [i] (juce::PopupMenu& menu) { menu.addItem ("Button " + juce::String (i), [] {}); },
                       .priority = i == 0   ? 100
                                   : i == 2 ? 10
                                            : 50,
                       .gapBefore = phazite::ui::ToolRowTint::gap });
        }
    }

    std::vector<std::unique_ptr<juce::TextButton>> buttons;
    phazite::ui::ToolRow row;
};
} // namespace

TEST_CASE ("A tool row lays its controls out from the left, and where it is too narrow the least needed go into its More menu")
{
    namespace ToolRowTint = phazite::ui::ToolRowTint;
    Row screen;
    auto& row = screen.row;
    const auto all = 4 * ToolRowTint::button + 3 * ToolRowTint::gap;
    CHECK (row.getIdealWidth() == all);

    // Room for all: each in the row, left to right, in the middle of its height, and no More button.
    row.setBounds (0, 0, all, 32);
    for (int i = 0; i < 4; ++i)
    {
        const auto& button = *screen.buttons[(size_t) i];
        CHECK (row.isInRow (button));
        CHECK (button.getX() == i * (ToolRowTint::button + ToolRowTint::gap));
        CHECK (button.getY() == (32 - ToolRowTint::button) / 2);
    }
    CHECK_FALSE (row.getMoreButton().isVisible());

    // A pixel too narrow: the least needed (the third) goes; the More button takes the room it left, so the rightmost of the
    // equals after it (the fourth) goes too, and the More button follows the second.
    row.setSize (all - 1, 32);
    CHECK (row.isInRow (*screen.buttons[0]));
    CHECK (row.isInRow (*screen.buttons[1]));
    CHECK_FALSE (row.isInRow (*screen.buttons[2]));
    CHECK_FALSE (row.isInRow (*screen.buttons[3]));
    CHECK (row.getMoreButton().isVisible());
    CHECK (row.getMoreButton().getX() == 2 * (ToolRowTint::button + ToolRowTint::gap));
    CHECK (row.getMoreButton().getRight() <= row.getWidth());

    // Once they all fit again, all are back in their places, and the More button goes.
    row.setSize (all, 32);
    for (int i = 0; i < 4; ++i)
    {
        CHECK (row.isInRow (*screen.buttons[(size_t) i]));
        CHECK (screen.buttons[(size_t) i]->getX() == i * (ToolRowTint::button + ToolRowTint::gap));
    }
    CHECK_FALSE (row.getMoreButton().isVisible());

    // Narrower: of the equals, the rightmost goes first; the one needed most stays to the end.
    row.setSize (2 * ToolRowTint::button + ToolRowTint::gap, 32);
    CHECK (row.isInRow (*screen.buttons[0]));
    CHECK_FALSE (row.isInRow (*screen.buttons[1]));
    CHECK_FALSE (row.isInRow (*screen.buttons[3]));
    CHECK (row.getMoreButton().getRight() <= row.getWidth());

    // Nothing ever lies past the row's edge.
    for (const auto width : { 0, 10, 30, 60, all })
    {
        row.setSize (width, 32);
        for (const auto* child : row.getChildren())
            if (child->isVisible())
                CHECK (child->getRight() <= width);
    }

    // An owner's own items in the More menu (the piano roll's notes): the button always shows, even with room for the rest.
    row.setMoreItems ([] (juce::PopupMenu& menu) { menu.addItem ("Owner's", [] {}); }, "More");
    CHECK (row.getIdealWidth() == all + ToolRowTint::gap + ToolRowTint::button);
    row.setSize (row.getIdealWidth(), 32);
    CHECK (row.getMoreButton().isVisible());
    for (const auto& button : screen.buttons)
        CHECK (row.isInRow (*button));
}

TEST_CASE ("A tool row leaves out what isn't wanted, as a tool an editor can't use")
{
    Row screen;
    auto wanted = true;
    juce::TextButton extra ("Extra");
    screen.row.add ({ .control = &extra,
                      .getWidth = [] { return phazite::ui::ToolRowTint::button; },
                      .gapBefore = phazite::ui::ToolRowTint::gap,
                      .isWanted = [&wanted] { return wanted; } });
    screen.row.setBounds (0, 0, 400, 32);
    CHECK (screen.row.isInRow (extra));

    wanted = false;
    screen.row.update();
    CHECK_FALSE (extra.isVisible());
    CHECK_FALSE (screen.row.isInRow (extra));
    CHECK_FALSE (screen.row.getMoreButton().isVisible()); // not left out for room: nothing for the More menu
}

TEST_CASE ("The grid's button is as wide as what it says: its size, \"auto\" while the grid adapts, and its arrow")
{
    // Its texts laid out as it paints them (GridButton::paintButton()): each must keep all of its letters, as a text drawn in
    // less room than it measures loses its last ones (the README's picture showed "2 bars aut", 2026-11-13).
    namespace ui = phazite::ui;
    const auto bodyFont = juce::Font (juce::FontOptions (ui::TextSize::body));
    const auto smallFont = juce::Font (juce::FontOptions (ui::TextSize::small));
    const auto lettersShown = [] (const juce::Font& font, const juce::String& text, float width)
    {
        juce::GlyphArrangement glyphs;
        glyphs.addCurtailedLineOfText (font, text, 0.0f, 0.0f, width, false);
        return glyphs.getNumGlyphs();
    };

    for (const auto& size : { "1/16 beat", "1/4 beat", "Beat", "Bar", "2 bars", "1/8 beat triplet" })
    {
        CAPTURE (size);

        for (const auto adaptive : { false, true })
        {
            ui::GridButton button;
            button.setGrid (size, adaptive);
            const auto room = (float) button.getIdealWidth() - 2.0f * (float) ui::Space::s - ui::GridButtonTint::arrowSize
                              - (float) ui::GridButtonTint::arrowGap;
            const auto sizeWidth = juce::GlyphArrangement::getStringWidth (bodyFont, size) + 1.0f;
            CHECK (lettersShown (bodyFont, size, sizeWidth) == lettersShown (bodyFont, size, 1.0e6f));

            if (adaptive)
                CHECK (lettersShown (smallFont, "auto", room - sizeWidth - (float) ui::Space::xs)
                       == lettersShown (smallFont, "auto", 1.0e6f));
            else
                CHECK (room >= sizeWidth);
        }
    }
}
