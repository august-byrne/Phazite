// SPDX-License-Identifier: AGPL-3.0-or-later
// The rules by which the main window's panels hide, show and fill the window (ui/shell/PanelLayout.h), without a window.
#include "TestHelpers.h"
#include "ui/shell/PanelLayout.h"

using phazite::ui::Panel;
using phazite::ui::PanelLayout;

TEST_CASE ("Panels hide and show, and the centre keeps the song's views or the editor")
{
    PanelLayout layout;

    for (const auto panel : { Panel::browser, Panel::song, Panel::editor, Panel::inspector })
        CHECK (layout.takesRoom (panel));

    layout.hide (Panel::browser);
    CHECK_FALSE (layout.isShown (Panel::browser));
    CHECK_FALSE (layout.takesRoom (Panel::browser));
    layout.toggle (Panel::browser);
    CHECK (layout.takesRoom (Panel::browser));

    // Hiding the song's views while the editor is hidden shows the editor, and the other way round.
    layout.hide (Panel::editor);
    layout.hide (Panel::song);
    CHECK (layout.isShown (Panel::editor));
    CHECK_FALSE (layout.isShown (Panel::song));
    layout.hide (Panel::editor);
    CHECK (layout.isShown (Panel::song));
    CHECK_FALSE (layout.isShown (Panel::editor));

    // The song's views stay while the editor is in a window of its own, whose Show or Hide does nothing then.
    layout.show (Panel::editor);
    layout.setDetached (Panel::editor, true);
    CHECK_FALSE (layout.canHide (Panel::song));
    CHECK_FALSE (layout.canToggle (Panel::song));
    layout.hide (Panel::song);
    CHECK (layout.takesRoom (Panel::song));
    CHECK_FALSE (layout.canToggle (Panel::editor));
    CHECK_FALSE (layout.takesRoom (Panel::editor));

    // The song's views never leave the main window.
    layout.setDetached (Panel::song, true);
    CHECK_FALSE (layout.isDetached (Panel::song));
}

TEST_CASE ("One panel at a time fills the window, and the others come back as they were")
{
    PanelLayout layout;
    layout.hide (Panel::inspector);
    layout.maximize (Panel::song);
    CHECK (layout.isMaximized (Panel::song));
    CHECK (layout.takesRoom (Panel::song));
    CHECK_FALSE (layout.takesRoom (Panel::browser));
    CHECK_FALSE (layout.takesRoom (Panel::editor));
    CHECK (layout.isShown (Panel::browser)); // only out of sight

    // Another one in its place; then restored, the inspector still hidden.
    layout.maximize (Panel::editor);
    CHECK_FALSE (layout.takesRoom (Panel::song));
    layout.toggleMaximized (Panel::editor);
    CHECK_FALSE (layout.getMaximized().has_value());
    CHECK (layout.takesRoom (Panel::browser));
    CHECK (layout.takesRoom (Panel::song));
    CHECK_FALSE (layout.takesRoom (Panel::inspector));

    // Asking to see another panel restores the window, and so does hiding the maximized one.
    layout.maximize (Panel::song);
    layout.toggle (Panel::browser); // shown, but out of sight: it comes back
    CHECK_FALSE (layout.getMaximized().has_value());
    CHECK (layout.takesRoom (Panel::browser));
    layout.maximize (Panel::browser);
    layout.hide (Panel::browser);
    CHECK_FALSE (layout.getMaximized().has_value());
    CHECK_FALSE (layout.isShown (Panel::browser));

    // Maximizing a hidden panel shows it.
    layout.maximize (Panel::inspector);
    CHECK (layout.takesRoom (Panel::inspector));
    layout.restore();
    CHECK (layout.isShown (Panel::inspector));

    // A panel in a window of its own isn't maximized, and a maximized one that leaves restores the window; one that leaves
    // while another fills the window leaves it so, and shows in its own.
    layout.maximize (Panel::song);
    layout.hide (Panel::inspector);
    layout.setDetached (Panel::inspector, true);
    CHECK (layout.isMaximized (Panel::song));
    CHECK (layout.isShown (Panel::inspector));
    layout.restore();
    CHECK_FALSE (layout.canMaximize (Panel::inspector));
    layout.maximize (Panel::inspector);
    CHECK_FALSE (layout.getMaximized().has_value());
    layout.maximize (Panel::editor);
    layout.setDetached (Panel::editor, true);
    CHECK_FALSE (layout.getMaximized().has_value());
    CHECK (layout.takesRoom (Panel::song));
}

TEST_CASE ("The panels' commands say what they do now")
{
    PanelLayout layout;
    CHECK (layout.getToggleName (Panel::browser) == juce::String ("Hide the Browser"));
    CHECK (layout.getMaximizeName (Panel::song).isEmpty());
    CHECK (layout.getMaximizeInUseName().isEmpty());

    layout.maximize (Panel::song);
    CHECK (layout.getToggleName (Panel::browser) == juce::String ("Show the Browser")); // out of sight
    CHECK (layout.getToggleName (Panel::song) == juce::String ("Hide the Playlist and Mixer"));
    CHECK (layout.getMaximizeName (Panel::song) == juce::String ("Restore the Panels"));
    CHECK (layout.getMaximizeName (Panel::editor).isEmpty());
    CHECK (layout.getMaximizeInUseName() == juce::String ("Restore the Panels"));
}

TEST_CASE ("The panels' layout goes into the settings as text, and back")
{
    PanelLayout layout;
    layout.hide (Panel::browser);
    layout.maximize (Panel::editor);
    layout.browserWidth = 300;
    layout.inspectorWidth = 200;
    layout.editorShare = 0.6;
    const auto text = layout.toText();
    CHECK (text == juce::String ("shown=song,editor,inspector maximized=editor browser=300 inspector=200 editor=0.600"));

    PanelLayout read;
    read.setFromText (text);
    CHECK (read.toText() == text);
    CHECK (read.isMaximized (Panel::editor));
    CHECK_FALSE (read.isShown (Panel::browser));

    // What a text doesn't say, or says wrongly, stays as it was; the centre keeps one of its panels.
    PanelLayout odd;
    odd.setFromText ("shown=browser browser=9999 editor=7 maximized=nothing nonsense");
    CHECK (odd.isShown (Panel::song));
    CHECK (odd.isShown (Panel::editor));
    CHECK (odd.browserWidth == phazite::ui::Shell::maximumBrowserWidth);
    CHECK (odd.editorShare == doctest::Approx (phazite::ui::Shell::editorShare));
    CHECK_FALSE (odd.getMaximized().has_value());
}

TEST_CASE ("The side panels keep their widths while the window has room, and give way before the centre does")
{
    using phazite::ui::Shell::dividerSize, phazite::ui::Shell::minimumCentreWidth;
    PanelLayout layout;
    auto widths = layout.getSideWidths (1280);
    CHECK (widths.browser == phazite::ui::Shell::browserWidth);
    CHECK (widths.inspector == phazite::ui::Shell::inspectorWidth);

    // 800 wide, 10 short of both and the centre at its least: each gives way in proportion to what it has above its least.
    widths = layout.getSideWidths (800);
    CHECK (widths.browser + widths.inspector + 2 * dividerSize + minimumCentreWidth == 800);
    CHECK (widths.browser == phazite::ui::Shell::browserWidth - 5);

    // Never below their least.
    widths = layout.getSideWidths (400);
    CHECK (widths.browser == phazite::ui::Shell::minimumBrowserWidth);
    CHECK (widths.inspector == phazite::ui::Shell::minimumInspectorWidth);

    // A hidden one takes none, and a maximized one all.
    layout.hide (Panel::inspector);
    CHECK (layout.getSideWidths (1280).inspector == 0);
    layout.maximize (Panel::browser);
    CHECK (layout.getSideWidths (1280).browser == 1280);

    // The editor dock: its share of the centre, leaving the song's views their least.
    PanelLayout dock;
    CHECK (dock.getEditorHeight (600) == juce::roundToInt (600 * phazite::ui::Shell::editorShare));
    dock.editorShare = phazite::ui::Shell::maximumEditorShare;
    CHECK (dock.getEditorHeight (200) == phazite::ui::Shell::minimumCentreHeight);
}
