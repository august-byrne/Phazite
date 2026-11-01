// SPDX-License-Identifier: AGPL-3.0-or-later
// The window's two areas of views (src/ui/shell/Panels.h; the owner, 2026-11-01: "the user should be able to drag the mixer
// tab down into the bottom panel, or the piano roll up"): where each view is, a tab dragged to the other area, the keys that
// show a view wherever it is, an area left without views, and Reset View Places.
#include "AppScreen.h"
#include "services/Settings.h"
#include "ui/shell/Panels.h"

#include <vector>

using phazite::ui::Panel;
using phazite::ui::View;
using phazite::ui::ViewArea;
using phazite::ui::ViewPlaces;
using phazite::ui::ViewTab;

TEST_CASE ("View places: each view in one area, moved before another or after them, and kept as text")
{
    ViewPlaces places;
    CHECK (places.getArea (View::mixer) == Panel::song);
    CHECK (places.getArea (View::devices) == Panel::editor);

    // The mixer down, after the lower area's views; the piano roll up, before the playlist.
    places.move (View::mixer, Panel::editor);
    CHECK (places.upper == std::vector { View::playlist });
    CHECK (places.lower == std::vector { View::clipEditor, View::devices, View::mixer });
    places.move (View::clipEditor, Panel::song, 0);
    CHECK (places.upper == std::vector { View::clipEditor, View::playlist });
    CHECK (places.lower == std::vector { View::devices, View::mixer });

    // Along its own area: the devices after the mixer.
    places.move (View::devices, Panel::editor, 2);
    CHECK (places.lower == std::vector { View::mixer, View::devices });

    // As the settings keep them, and back; anything that isn't every view once is where they start.
    CHECK (places.toString() == juce::String ("clipEditor,playlist;mixer,devices"));
    CHECK (ViewPlaces::fromString (places.toString()) == places);
    CHECK (ViewPlaces::fromString ({}) == ViewPlaces());
    CHECK (ViewPlaces::fromString ("playlist,mixer;clipEditor") == ViewPlaces());
    CHECK (ViewPlaces::fromString ("playlist,mixer,mixer;clipEditor,devices") == ViewPlaces());
    CHECK (ViewPlaces::fromString ("playlist,piano;clipEditor,devices,mixer") == ViewPlaces());
    CHECK (ViewPlaces::fromString (";playlist,mixer,clipEditor,devices").upper.empty());
}

TEST_CASE ("A view's tab dragged to the other area goes there and shows, the settings keep it, and its key shows it there")
{
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto* upper = dynamic_cast<ViewArea*> (&screen.shell.getPanel (Panel::song));
    auto* lower = dynamic_cast<ViewArea*> (&screen.shell.getPanel (Panel::editor));
    REQUIRE (upper != nullptr);
    REQUIRE (lower != nullptr);
    CHECK (upper->getTabNames() == juce::StringArray { "Playlist", "Mixer" });

    // The mixer's tab, pressed and moved, starts a drag that carries the mixer.
    auto* mixerTab = upper->getTabbedButtonBar().getTabButton (1);
    REQUIRE (mixerTab != nullptr);
    phazite::test::Mouse mouse (*mixerTab);
    mouse.press (mixerTab->getLocalBounds().getCentre());
    mouse.dragTo (mixerTab->getLocalBounds().getCentre() + juce::Point<int> (12, 0));
    CHECK (screen.shell.isDragAndDropActive());
    CHECK (phazite::ui::readViewDrag (screen.shell.getCurrentDragDescription()) == View::mixer);
    mouse.release();
    phazite::test::dispatchPendingMessages();

    // Over the lower area's views, a tint and where it would go; dropped, it is there, after them, and shows.
    const juce::DragAndDropTarget::SourceDetails mixerDrag (phazite::ui::makeViewDrag (View::mixer), nullptr,
                                                            lower->getLocalBounds().getCentre());
    REQUIRE (lower->isInterestedInDragSource (mixerDrag));
    lower->itemDragEnter (mixerDrag);
    CHECK (lower->getDropIndex() == -1);
    CHECK (lower->isShowing());
    lower->itemDropped (mixerDrag);
    CHECK_FALSE (lower->getDropIndex().has_value());
    CHECK (upper->getTabNames() == juce::StringArray { "Playlist" });
    CHECK (lower->getTabNames() == juce::StringArray { "Piano roll", "Instrument", "Effects", "Mixer" });
    CHECK (lower->getShownTab() == ViewTab::mixer);
    CHECK (screen.services.settings.get (phazite::Settings::viewPlaces) == juce::String ("playlist;clipEditor,devices,mixer"));

    // In the lower panel, F9 shows it, or hides the panel while it shows it, as F7 does the piano roll.
    lower->showTab (ViewTab::clipEditor);
    REQUIRE (screen.registry.invoke ("view.mixer"));
    CHECK (lower->getShownTab() == ViewTab::mixer);
    REQUIRE (screen.registry.invoke ("view.mixer"));
    CHECK_FALSE (lower->isShowing());
    REQUIRE (screen.registry.invoke ("view.mixer"));
    CHECK (lower->isShowing());
    CHECK (lower->getShownTab() == ViewTab::mixer);

    // The devices up, dropped on the upper area's first tab: Instrument and Effects go together, before the playlist, and F6
    // shows the instrument there.
    const juce::DragAndDropTarget::SourceDetails devicesDrag (phazite::ui::makeViewDrag (View::devices), nullptr, { 1, 1 });
    upper->itemDropped (devicesDrag);
    CHECK (upper->getTabNames() == juce::StringArray { "Instrument", "Effects", "Playlist" });
    CHECK (lower->getTabNames() == juce::StringArray { "Piano roll", "Mixer" });
    REQUIRE (screen.registry.invoke ("view.instrument"));
    CHECK (upper->getShownTab() == ViewTab::instrument);
}

TEST_CASE ("An area left without views hides, and Reset View Places puts every view back where it starts")
{
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto* upper = dynamic_cast<ViewArea*> (&screen.shell.getPanel (Panel::song));
    auto* lower = dynamic_cast<ViewArea*> (&screen.shell.getPanel (Panel::editor));
    REQUIRE (upper != nullptr);
    REQUIRE (lower != nullptr);
    const auto* reset = screen.registry.find ("view.resetViewPlaces");
    REQUIRE (reset != nullptr);
    CHECK_FALSE (reset->isEnabled());

    // Both of the upper area's views down: it hides, and the lower one fills the centre.
    for (const auto view : { View::playlist, View::mixer })
        lower->itemDropped ({ phazite::ui::makeViewDrag (view), nullptr, lower->getLocalBounds().getCentre() });

    CHECK (upper->isEmpty());
    CHECK_FALSE (upper->isShowing());
    CHECK (lower->isShowing());
    CHECK (lower->getY() == 0);
    CHECK (reset->isEnabled());

    REQUIRE (screen.registry.invoke ("view.resetViewPlaces"));
    CHECK (upper->getTabNames() == juce::StringArray { "Playlist", "Mixer" });
    CHECK (lower->getTabNames() == juce::StringArray { "Piano roll", "Instrument", "Effects" });
    CHECK (upper->isShowing());
    CHECK (lower->isShowing());
    CHECK_FALSE (reset->isEnabled());
}
