// SPDX-License-Identifier: AGPL-3.0-or-later
// The settings in their modal card over the main window, as VS Code's Settings in its modal editor
// (src/ui/components/ModalCard.h, AppShell::showModal(); docs/vscode-look.md, "Settings"), on the window as the app builds it
// (AppScreen.h): the card's size and place as the window's change, the window dimmed behind it, the header's and the card's
// sizes and colours in both themes, what a press and the keys do while it shows, the focus going round it and back, a
// drop-down opened in it, the narrow layout and its Contents, screen readers, Alt+F4, the window coming forward, what a
// repaint in the card paints of the window behind it, and the card's joins. And, skipped, pictures of the window with the
// card open (--test-suite=settings-pictures --no-skip, into the folder PHAZITE_PICTURES names).
#include "AppScreen.h"
#include "GoldenFiles.h"
#include "PaintCheck.h"
#include "TestAudioDevice.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/transport/TransportModel.h"
#include "services/Settings.h"
#include "ui/components/ModalCard.h"
#include "ui/components/TrackSwitch.h"
#include "ui/input/KeymapStore.h"
#include "ui/input/WindowKeys.h"
#include "ui/mixer/DevicesView.h"
#include "ui/mixer/MixerView.h"
#include "ui/shell/SettingsPanel.h"
#include "ui/shell/TopBar.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/theme/Popups.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace
{
using phazite::test::dispatchPendingMessages;
using phazite::ui::ModalCard;
using phazite::ui::SettingsPanel;

// The window's own title bar under the card: Phazite's on Windows, the system's (above the content) elsewhere.
int getTitleBarHeight()
{
#if JUCE_WINDOWS
    return phazite::ui::Shell::titleBarHeight;
#else
    return 0;
#endif
}

// The rows at the card's top that its 8 px corners cut into inside its 1 px edge (the first whole row: 8 - sqrt (8 * 8 - 7 * 7),
// rounded up), and at its foot as many as their radius, which the backdrop paints; the card's opaque part is the rest, inside
// the edge.
constexpr int cornerRows = 5, footRows = 8;

// Where VS Code puts its modal editor in a window so big (modalEditorPart.ts), written out from the specification: 80 % of the
// width and of the height under the title bar, at most 1400 by 900, centred in the whole window but under the title bar;
// maximized, the width less 16, the height under the title bar less 16 or the title bar's height.
juce::Rectangle<int> getExpectedCard (int width, int height, bool maximized)
{
    const auto titleBar = getTitleBarHeight();
    const auto available = height - titleBar;
    const auto cardWidth = maximized ? width - 16 : std::min ({ juce::roundToInt (width * 0.8), 1400, width });
    const auto cardHeight =
        maximized ? available - std::max (titleBar, 16) : std::min ({ juce::roundToInt (available * 0.8), 900, available });
    return { (width - cardWidth) / 2, std::max (titleBar, (height - cardHeight) / 2), cardWidth, cardHeight };
}

// The window as the app builds it, in Phazite's look, with an untitled project open and a track in it; the settings opened in
// their card as MainWindow::showSettings() opens them.
struct ModalScreen
{
    explicit ModalScreen (int width = 1280, int height = 800)
    {
        screen.window.setSize (width, height);
        screen.session.openAtStartup();
        dispatchPendingMessages();
        REQUIRE (screen.registry.invoke ("track.add"));
        dispatchPendingMessages();
    }

    ~ModalScreen() { screen.shell.closeModal(); }

    SettingsPanel& open (const juce::String& group = SettingsPanel::audio)
    {
        auto panel = std::make_unique<SettingsPanel> (
            screen.services.engineContext, screen.commands, screen.keyboard, screen.session.getContext(), screen.services.settings,
            SettingsPanel::Actions { {},
                                     [this] (float scale) { screen.services.settings.set (phazite::Settings::interfaceScale, scale); } });
        auto& shown = *panel;
        screen.shell.showModal (std::move (panel), "Settings", phazite::ui::Icon::settings);
        shown.showGroup (group);
        dispatchPendingMessages();
        return shown;
    }

    ModalCard& modal()
    {
        auto* shown = screen.shell.getModal();
        REQUIRE (shown != nullptr);
        return *shown;
    }

    bool isOpen() const { return screen.shell.getModal() != nullptr; }

    // Where a component is in the window.
    juce::Rectangle<int> inWindow (juce::Component& component)
    {
        return screen.window.getLocalArea (&component, component.getLocalBounds());
    }

    phazite::ui::LookAndFeel lookAndFeel;

    struct Look
    {
        explicit Look (juce::LookAndFeel& look) { juce::LookAndFeel::setDefaultLookAndFeel (&look); }
        ~Look() { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); }
    } look { lookAndFeel };

    phazite::test::AppScreen screen;
};

// Whether a colour is another halved toward black, as black at 50 % over it makes it (a step of rounding either way).
bool isHalved (juce::Colour dimmed, juce::Colour behind)
{
    const auto near = [] (int a, int b) { return std::abs (a - b) <= 1; };
    return near (dimmed.getRed(), behind.getRed() / 2) && near (dimmed.getGreen(), behind.getGreen() / 2)
           && near (dimmed.getBlue(), behind.getBlue() / 2);
}

juce::Component* getFocused()
{
    return juce::Component::getCurrentlyFocusedComponent();
}

// The tests' device open on the engine context's device manager (AudioSettingsTests.cpp), and closed again after.
struct OpenDevice
{
    OpenDevice() { REQUIRE (phazite::test::TestAudioDeviceType::open (devices(), 48000.0, 480) != nullptr); }
    ~OpenDevice() { phazite::test::TestAudioDeviceType::close (devices()); }
    static juce::AudioDeviceManager& devices() { return phazite::test::getEngineContext().getAudioDeviceManager(); }
};
} // namespace

TEST_CASE ("The settings open in a card over the whole window, as VS Code's modal editor, sized as it sizes it as the window's size "
           "changes, and maximized")
{
    ModalScreen s;
    auto& panel = s.open();
    auto& modal = s.modal();
    CHECK (&modal.getContent() == &panel);
    CHECK (modal.isCurrentlyModal (false));
    CHECK (s.screen.shell.getWindowControlAt ({ 300, 10 }) == juce::Component::WindowControlKind::client);

    // In front of everything else in the window: its backdrop over the whole of it (the title bar included), see-through, and
    // the card itself in front of that, opaque, all but the rows its rounded corners cut into.
    CHECK (s.screen.shell.getIndexOfChildComponent (&modal) == s.screen.shell.getNumChildComponents() - 1);
    CHECK (s.screen.shell.getIndexOfChildComponent (&modal.getBackdrop()) == s.screen.shell.getNumChildComponents() - 2);
    CHECK (modal.getBackdrop().getBounds() == s.screen.shell.getLocalBounds());
    CHECK_FALSE (modal.getBackdrop().isOpaque());
    CHECK (modal.isOpaque());
    CHECK (panel.isOpaque());

    // At three sizes and the largest, as the window is resized; and the spec's own example (1600 by 900, under a 35 px title
    // bar: 1280 by 692).
    for (const auto& size :
         { juce::Point<int> (1280, 800), juce::Point<int> (1600, 900), juce::Point<int> (800, 500), juce::Point<int> (2000, 1200) })
    {
        CAPTURE (size.toString().toStdString());
        s.screen.window.setSize (size.x, size.y);
        CHECK (modal.getBackdrop().getBounds() == s.screen.shell.getLocalBounds());
        CHECK (modal.getCardBounds() == getExpectedCard (size.x, size.y, false));
        CHECK (modal.getBounds() == modal.getCardBounds().withTrimmedTop (cornerRows).withTrimmedBottom (footRows).reduced (1, 0));
    }

#if JUCE_WINDOWS
    s.screen.window.setSize (1600, 900);
    CHECK (modal.getCardBounds() == juce::Rectangle<int> (160, 104, 1280, 692));
#endif

    // Maximized by its button, restored by a double-click on its header, maximized again as the window changes size.
    s.screen.window.setSize (1280, 800);
    phazite::test::Mouse mouse (s.screen.window);
    mouse.moveTo (s.inWindow (modal.getMaximizeButton()).getCentre());
    mouse.click (s.inWindow (modal.getMaximizeButton()).getCentre());
    CHECK (modal.isMaximized());
    CHECK (modal.getMaximizeButton().getTitle() == "Restore");
    CHECK (modal.getCardBounds() == getExpectedCard (1280, 800, true));
    const auto header = [&modal]
    { return modal.getCardBounds().getPosition() + juce::Point<int> (modal.getCardBounds().getWidth() / 2, 16); };
    mouse.doubleClick (header());
    CHECK_FALSE (modal.isMaximized());
    CHECK (modal.getCardBounds() == getExpectedCard (1280, 800, false));
    mouse.doubleClick (header());
    CHECK (modal.isMaximized());
    s.screen.window.setSize (1600, 900);
    CHECK (modal.getCardBounds() == getExpectedCard (1600, 900, true));
    CHECK (s.isOpen());
}

TEST_CASE ("The window behind the settings' card is halved toward black, its title bar too, and the card's header and body are "
           "VS Code's, in sizes and in colours, dark and then light as the theme switches")
{
    ModalScreen s;
    phazite::test::Mouse (s.screen.window).moveTo ({ -100, -100 });

    // Two places of the window, away from the card and its shadow: the activity bar's card, and the title bar.
    const juce::Point<int> frame (10, 400), titleBar (1000, 5);
    const auto before = phazite::test::paintComponent (s.screen.window);
    auto& panel = s.open();
    auto& modal = s.modal();

    // The header: 32 px and its line, the content under it inside the 1 px edge, down to where the corners start; Maximize and
    // Close 22 px, 4 px apart, 8 px from the right.
    const auto card = modal.getCardBounds();
    const auto maximize = s.inWindow (modal.getMaximizeButton()), close = s.inWindow (modal.getCloseButton());
    CHECK (s.inWindow (panel)
           == juce::Rectangle<int> (card.getX() + 1, card.getY() + 34, card.getWidth() - 2, card.getHeight() - 34 - footRows));
    CHECK (maximize.getWidth() == 22);
    CHECK (maximize.getHeight() == 22);
    CHECK (close.getWidth() == 22);
    CHECK (close.getX() - maximize.getRight() == 4);
    CHECK (card.getRight() - 1 - close.getRight() == 8);
    CHECK (close.getCentreY() == card.getY() + 1 + 16);

    struct Colours
    {
        phazite::ui::ThemeName theme;
        juce::uint32 header, line, body, edge, field, dropdown;
    };

    // VS Code's 2026 Dark and 2026 Light (docs/vscode-look.md): titleBar.activeBackground, titleBar.border, editor.background,
    // editorWidget.border, input.background, settings.dropdownBackground.
    for (const auto& expected :
         { Colours { phazite::ui::ThemeName::dark, 0xff191a1b, 0xff2a2b2c, 0xff121314, 0xff2a2b2c, 0xff191a1b, 0xff313131 },
           Colours { phazite::ui::ThemeName::light, 0xfffafafd, 0xfff0f1f2, 0xffffffff, 0xffe4e5e6, 0xffffffff, 0xffffffff } })
    {
        CAPTURE (expected.theme == phazite::ui::ThemeName::dark ? "dark" : "light");
        s.lookAndFeel.setTheme (expected.theme); // as MainWindow::applyTheme() switches it
        phazite::ui::refreshTheme (s.screen.window);
        const auto image = phazite::test::paintComponent (s.screen.window);
        const auto pixel = [&image] (juce::Point<int> at) { return image.getPixelAt (at.x, at.y); };

        CHECK (pixel ({ card.getCentreX(), card.getY() + 17 }) == juce::Colour (expected.header));
        CHECK (pixel ({ card.getCentreX(), card.getY() + 33 }) == juce::Colour (expected.line));
        CHECK (pixel ({ card.getX() + 10, card.getBottom() - 12 }) == juce::Colour (expected.body));
        CHECK (pixel ({ card.getX(), card.getCentreY() }) == juce::Colour (expected.edge));
        CHECK (pixel ({ card.getCentreX(), card.getY() + 33 + 1 }) == juce::Colour (expected.body));

        const auto field = s.inWindow (panel.getSearchField());
        CHECK (pixel (field.getCentre()) == juce::Colour (expected.field));
        auto* driver = dynamic_cast<juce::ComboBox*> (panel.findRow ("Driver")->getControl());
        REQUIRE (driver != nullptr);
        const auto box = s.inWindow (*driver);
        CHECK (box.getHeight() == 26);
        CHECK (pixel ({ box.getX() + box.getWidth() * 3 / 4, box.getY() + 3 }) == juce::Colour (expected.dropdown));

        // The sash's line, in the panel border's colour, between the table of contents and the list.
        CHECK (pixel ({ s.inWindow (panel).getX() + panel.getContentsWidth(), s.inWindow (panel.getList()).getCentreY() })
               == juce::Colour (expected.line));

        // Behind: what was there, halved toward black. (The window behind is painted in the theme it opened in: the dark one;
        // the light one's comes from a picture of its own.)
        if (expected.theme == phazite::ui::ThemeName::dark)
        {
            CHECK (isHalved (pixel (frame), before.getPixelAt (frame.x, frame.y)));
            CHECK (isHalved (pixel (titleBar), before.getPixelAt (titleBar.x, titleBar.y)));
        }
    }

    // The light window behind, halved too.
    s.screen.shell.closeModal();
    const auto light = phazite::test::paintComponent (s.screen.window);
    s.open();
    const auto dimmed = phazite::test::paintComponent (s.screen.window);
    CHECK (isHalved (dimmed.getPixelAt (frame.x, frame.y), light.getPixelAt (frame.x, frame.y)));
    CHECK (isHalved (dimmed.getPixelAt (titleBar.x, titleBar.y), light.getPixelAt (titleBar.x, titleBar.y)));
}

TEST_CASE ("A press on the dimmed window closes the settings and reaches nothing behind, one on the card doesn't close it, and "
           "Close does")
{
    // A large window without the left panel, where the card leaves the track headers' switches uncovered at its left.
    ModalScreen s (2000, 1200);
    REQUIRE (s.screen.registry.invoke ("view.toggleLeftPanel"));
    dispatchPendingMessages();
    auto& context = s.screen.session.getContext();
    const auto track = phazite::getAllPlaylistTracks (context).back();
    const auto isMuted = [&context, track] { return phazite::isTrackMuted (*phazite::findSong (context), track, false); };

    // A track's mute switch, which a click mutes while nothing is over it.
    phazite::ui::TrackSwitch* muteSwitch = nullptr;

    for (auto* each : phazite::test::findAll<phazite::ui::TrackSwitch> (s.screen.shell))
        if (each->isShowing() && muteSwitch == nullptr)
            muteSwitch = each;

    REQUIRE (muteSwitch != nullptr);
    const auto switchCentre = s.inWindow (*muteSwitch).getCentre();
    phazite::test::Mouse mouse (s.screen.window);
    mouse.click (switchCentre);
    dispatchPendingMessages();
    REQUIRE (isMuted());
    mouse.pause();
    mouse.click (switchCentre);
    dispatchPendingMessages();
    REQUIRE_FALSE (isMuted());
    mouse.pause();

    // Over the card: the press closes it, and mutes nothing.
    s.open();
    REQUIRE_FALSE (s.modal().getCardBounds().contains (switchCentre));
    mouse.click (switchCentre);
    dispatchPendingMessages();
    CHECK_FALSE (s.isOpen());
    CHECK_FALSE (isMuted());
    mouse.pause();

    // A press on the card's body or its header keeps it.
    s.open();
    mouse.click (s.modal().getCardBounds().getBottomLeft() + juce::Point<int> (10, -12));
    mouse.pause();
    mouse.click (s.modal().getCardBounds().getPosition() + juce::Point<int> (s.modal().getCardBounds().getWidth() / 2, 16));
    mouse.pause();
    CHECK (s.isOpen());

    // Close.
    mouse.click (s.inWindow (s.modal().getCloseButton()).getCentre());
    dispatchPendingMessages();
    CHECK_FALSE (s.isOpen());
}

TEST_CASE ("Escape closes the settings from the list and the table of contents, clears the search in its field, and does nothing "
           "in an empty search; the focus goes to the search as they open and back where it was as they close")
{
    ModalScreen s;
    auto& playlist = *phazite::test::findAll<phazite::ui::Playlist> (s.screen.shell).getFirst();
    playlist.grabKeyboardFocus();
    auto* focusBefore = getFocused();
    REQUIRE (focusBefore != nullptr);

    auto* panel = &s.open();
    auto& search = panel->getSearchField();
    CHECK (search.hasKeyboardFocus (false));

    // In the search: Escape takes back what is typed, and keeps the keys there; in an empty one it does nothing.
    search.setText ("audio", true);
    dispatchPendingMessages();
    REQUIRE (panel->getCountText().isNotEmpty());
    s.screen.window.pressKey (juce::KeyPress (juce::KeyPress::escapeKey));
    dispatchPendingMessages();
    CHECK (search.isEmpty());
    CHECK (panel->getCountText().isEmpty());
    CHECK (search.hasKeyboardFocus (false));
    CHECK (s.isOpen());
    s.screen.window.pressKey (juce::KeyPress (juce::KeyPress::escapeKey));
    dispatchPendingMessages();
    CHECK (s.isOpen());
    CHECK (search.hasKeyboardFocus (false));

    // In the list (a row's control): it closes, and the focus is back in the playlist.
    auto* control = panel->getShownRows().getFirst()->getControl();
    REQUIRE (control != nullptr);
    control->grabKeyboardFocus();
    s.screen.window.pressKey (juce::KeyPress (juce::KeyPress::escapeKey));
    dispatchPendingMessages();
    CHECK_FALSE (s.isOpen());
    CHECK (getFocused() == focusBefore);

    // In the table of contents.
    panel = &s.open();
    auto* entry = panel->getContents().getChildComponent (0);
    REQUIRE (entry != nullptr);
    entry->grabKeyboardFocus();
    REQUIRE (entry->hasKeyboardFocus (false));
    s.screen.window.pressKey (juce::KeyPress (juce::KeyPress::escapeKey));
    dispatchPendingMessages();
    CHECK_FALSE (s.isOpen());
}

TEST_CASE ("Tab goes round the settings' card: from its last control to its first, and back with Shift")
{
    ModalScreen s;
    s.open();
    auto& modal = s.modal();
    const auto controls = juce::KeyboardFocusTraverser().getAllComponents (&modal);
    REQUIRE (controls.size() > 2);
    CHECK (controls.front() == &modal.getMaximizeButton());

    controls.back()->grabKeyboardFocus();
    REQUIRE (controls.back()->hasKeyboardFocus (false));
    s.screen.window.pressKey (juce::KeyPress (juce::KeyPress::tabKey));
    CHECK (getFocused() == controls.front());
    s.screen.window.pressKey (juce::KeyPress (juce::KeyPress::tabKey, juce::ModifierKeys::shiftModifier, 0));
    CHECK (getFocused() == controls.back());

    // With nothing in it focused (a press on its body gave the keys away), Tab finds its first control again.
    juce::Component::unfocusAllComponents();
    s.screen.window.pressKey (juce::KeyPress (juce::KeyPress::tabKey));
    CHECK (getFocused() == controls.front());
}

TEST_CASE ("While the settings show, the window's keys run none of its commands but those VS Code lets through its modal: Space "
           "plays nothing, the computer keyboard hears no key, Save saves")
{
    ModalScreen s;
    phazite::ui::loadKeySettings (s.screen.commands, s.screen.services.settings);
    phazite::ui::WindowKeys windowKeys { s.screen.window, s.screen.shortcutKeys, s.screen.keyboard }; // as the main window's
    s.screen.keyboard.setEnabled (true);
    auto& context = s.screen.session.getContext();
    auto& playlist = *phazite::test::findAll<phazite::ui::Playlist> (s.screen.shell).getFirst();

    // What the window's key listeners hear, the computer keyboard among them (it plays from the keys' going down and up, which
    // it asks the system for: none are down in the tests).
    struct Heard final : juce::KeyListener
    {
        bool keyPressed (const juce::KeyPress&, juce::Component*) override
        {
            ++keys;
            return false;
        }

        bool keyStateChanged (bool, juce::Component*) override
        {
            ++changes;
            return false;
        }

        int keys = 0, changes = 0;
    } heard;

    s.screen.window.addKeyListener (&heard);
    const juce::ScopeGuard notHeard { [&s, &heard] { s.screen.window.removeKeyListener (&heard); } };

    // Without them, Space plays and stops, and the window's listeners hear a letter and its going down.
    playlist.grabKeyboardFocus();
    s.screen.window.pressKey (juce::KeyPress (juce::KeyPress::spaceKey));
    dispatchPendingMessages();
    REQUIRE (phazite::isPlaying (context));
    s.screen.window.releaseKeys();
    s.screen.window.pressKey (juce::KeyPress (juce::KeyPress::spaceKey));
    dispatchPendingMessages();
    REQUIRE_FALSE (phazite::isPlaying (context));
    s.screen.window.releaseKeys();
    s.screen.window.keyGoesDown();
    s.screen.window.pressKey (juce::KeyPress ('a', {}, 'a'));
    s.screen.window.releaseKeys();
    REQUIRE (heard.keys > 0);
    REQUIRE (heard.changes > 0);
    heard.keys = heard.changes = 0;

    // With them, from the list, and with nothing focused in the card: nothing plays, nothing reaches the window's listeners.
    auto& panel = s.open();
    auto* button = panel.getContents().getChildComponent (0);
    REQUIRE (button != nullptr);

    for (auto* focused : { button, static_cast<juce::Component*> (nullptr) })
    {
        if (focused != nullptr)
            focused->grabKeyboardFocus();
        else
            juce::Component::unfocusAllComponents();

        s.screen.window.keyGoesDown();
        s.screen.window.pressKey (juce::KeyPress (juce::KeyPress::spaceKey));
        dispatchPendingMessages();
        CHECK_FALSE (phazite::isPlaying (context));
        s.screen.window.releaseKeys();
        s.screen.window.keyGoesDown();
        s.screen.window.pressKey (juce::KeyPress ('a', {}, 'a'));
        s.screen.window.releaseKeys();
        CHECK (heard.keys == 0);
        CHECK (heard.changes == 0);
        CHECK (s.isOpen());
    }

    // Save runs over them (the first save, to where its dialog suggests).
    REQUIRE (s.screen.session.isUntitled());
    s.screen.window.pressKey (juce::KeyPress ('s', juce::ModifierKeys::commandModifier, 0));
    CHECK (phazite::test::dispatchMessagesUntil ([&s] { return ! s.screen.session.isUntitled(); }));
    CHECK (s.isOpen());
    s.screen.keyboard.setEnabled (false);
}

TEST_CASE ("A drop-down opened in the settings' card shows above it, inside the window, and picks; Escape closes it before the card")
{
    const OpenDevice open;
    const phazite::test::ScopedDisplay display; // the menu is really shown
    ModalScreen s;
    phazite::ui::setPopupsInside (s.screen.window);
    auto& panel = s.open();
    auto* rate = dynamic_cast<juce::ComboBox*> (panel.findRow ("Sample Rate")->getControl());
    REQUIRE (rate != nullptr);
    REQUIRE (rate->getText() == "48000 Hz");

    // Shown as a click shows it (which posts this): looked at before a message is delivered, as JUCE takes a menu down on a
    // timer where the program isn't the foreground one.
    rate->showPopup();
    auto* menu = juce::Component::getCurrentlyModalComponent();
    REQUIRE (menu != nullptr);
    REQUIRE (menu != &s.modal());

    // Over the card, a child of the window, after the shell that holds the card.
    CHECK (menu->getParentComponent() == &s.screen.window);
    CHECK (s.screen.window.getIndexOfChildComponent (menu) > s.screen.window.getIndexOfChildComponent (&s.screen.shell));

    // Escape closes the menu, not the card; then the item above the one picked, by the keys.
    s.screen.window.pressKey (juce::KeyPress (juce::KeyPress::escapeKey));
    dispatchPendingMessages();
    CHECK (s.isOpen());
    CHECK (juce::Component::getCurrentlyModalComponent() == &s.modal());
    rate->showPopup();
    REQUIRE (juce::Component::getCurrentlyModalComponent() != &s.modal());
    s.screen.window.pressKey (juce::KeyPress (juce::KeyPress::upKey));
    s.screen.window.pressKey (juce::KeyPress (juce::KeyPress::returnKey));

    CHECK (phazite::test::dispatchMessagesUntil ([] { return phazite::test::getEngineContext().getAudioOutput().sampleRate < 46000.0; }));
    CHECK (rate->getText() == "44100 Hz");
    CHECK (s.isOpen());
    CHECK (juce::Component::getCurrentlyModalComponent() == &s.modal());
}

TEST_CASE ("At the window's smallest size the settings' card is narrow: no table of contents, the rows further in, and Contents at "
           "the search's left goes to the groups and to Shortcuts")
{
    ModalScreen s (800, 500);
    auto& panel = s.open();
    CHECK (panel.getWidth() < phazite::ui::SettingsTint::tocWidth + phazite::ui::SettingsTint::listMinimumWidth);
    CHECK (panel.isNarrow());
    CHECK_FALSE (panel.getContents().isVisible());
    CHECK_FALSE (panel.getSash().isVisible());
    CHECK (panel.getList().getX() == 0);

    auto* row = panel.getShownRows().getFirst();
    REQUIRE (row->getControl() != nullptr);
    CHECK (row->getControl()->getX() == phazite::ui::SettingsTint::narrowRowInset + phazite::ui::SettingsTint::padSide);
    phazite::test::checkPaints (s.modal(), "the narrow settings");

    // Contents: 22 by 22 at the search's left, in its row; the keys still go to the search as the settings open, and Shift+Tab
    // from there goes to it (before the search in Tab's order).
    auto& contents = panel.getContentsButton();
    const auto field = s.inWindow (panel.getSearchField());
    CHECK (contents.isShowing());
    CHECK (contents.getTitle() == "Contents");
    CHECK (s.inWindow (contents).getWidth() == 22);
    CHECK (s.inWindow (contents).getHeight() == 22);
    CHECK (s.inWindow (contents).getRight() < field.getX());
    CHECK (s.inWindow (contents).getCentreY() == field.getCentreY());
    CHECK (panel.getSearchField().hasKeyboardFocus (false));
    s.screen.window.pressKey (juce::KeyPress (juce::KeyPress::tabKey, juce::ModifierKeys::shiftModifier, 0));
    CHECK (contents.hasKeyboardFocus (false));

    // Its menu: the groups the table of contents lists, the one shown ticked, then Shortcuts apart; an item goes there.
    auto menu = panel.getContentsMenu();
    juce::StringArray items;
    auto separators = 0;

    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
    {
        if (it.getItem().isSeparator)
            ++separators;
        else
            items.add (it.getItem().text);
    }

    auto expected = panel.getListedGroups();
    expected.add (SettingsPanel::shortcuts);
    CHECK (items == expected);
    CHECK (separators == 1);
    CHECK (phazite::test::findMenuItem (menu, { SettingsPanel::audio })->isTicked);
    CHECK_FALSE (phazite::test::findMenuItem (menu, { SettingsPanel::editing })->isTicked);

    CHECK (phazite::test::runMenuItem (panel.getContentsMenu(), { SettingsPanel::editing }));
    CHECK (panel.getMarkedGroup() == SettingsPanel::editing);
    CHECK (phazite::test::findMenuItem (panel.getContentsMenu(), { SettingsPanel::editing })->isTicked);
    CHECK (phazite::test::runMenuItem (panel.getContentsMenu(), { SettingsPanel::shortcuts }));
    CHECK (panel.showsShortcuts());
    CHECK (phazite::test::findMenuItem (panel.getContentsMenu(), { SettingsPanel::shortcuts })->isTicked);
    CHECK (phazite::test::runMenuItem (panel.getContentsMenu(), { SettingsPanel::audio })); // and back from Shortcuts
    CHECK_FALSE (panel.showsShortcuts());

    // While searching, each group with its count, as the table of contents counts.
    panel.search ("audio");
    const auto searched = panel.getContentsMenu();
    const auto count = panel.getCount (SettingsPanel::audio);
    REQUIRE (count > 0);
    CHECK (phazite::test::findMenuItem (searched, { juce::String (SettingsPanel::audio) + " (" + juce::String (count) + ")" }) != nullptr);
    panel.search ({});

    // Wide enough again: the table of contents comes back, the rows where they were, and Contents goes.
    s.screen.window.setSize (1280, 800);
    CHECK_FALSE (panel.isNarrow());
    CHECK (panel.getContents().isVisible());
    CHECK_FALSE (contents.isVisible());
    CHECK (row->getControl()->getX() == phazite::ui::SettingsTint::rowInset + phazite::ui::SettingsTint::padSide);
}

TEST_CASE ("Screen readers find the settings' card as a dialog named Settings, and nothing of the window behind it while it shows")
{
    // The handlers as a screen reader's requests make them (the tests' windows have none behind them to make them for).
    const auto handlerOf = [] (juce::Component& component) { return component.createAccessibilityHandler(); }; // public as a component's
    ModalScreen s;
    auto* behindButton = phazite::test::findAll<juce::Button> (s.screen.shell.getActivityBar()).getFirst(); // the browser's
    REQUIRE (behindButton != nullptr);
    const auto behind = handlerOf (*behindButton);
    REQUIRE (behind != nullptr);
    CHECK (behind->getCurrentState().isFocusable());

    // The dialog is the card's opaque part, the modal component, which holds every control of the card; its backdrop is nothing
    // to a screen reader.
    auto& panel = s.open();
    {
        const auto dialog = handlerOf (s.modal()); // gone before the card
        REQUIRE (dialog != nullptr);
        CHECK (juce::Component::getCurrentlyModalComponent() == &s.modal());
        CHECK (dialog->getRole() == juce::AccessibilityRole::dialogWindow);
        CHECK (dialog->getTitle() == "Settings");
        CHECK (handlerOf (s.modal().getBackdrop())->getRole() == juce::AccessibilityRole::ignored);
        CHECK_FALSE (behind->getCurrentState().isFocusable());
        CHECK (handlerOf (s.modal().getCloseButton())->getCurrentState().isFocusable());
        CHECK (handlerOf (panel.getSearchField())->getCurrentState().isFocusable());
        CHECK (s.modal().isParentOf (&panel.getSearchField()));
    }

    s.screen.shell.closeModal();
    CHECK (behind->getCurrentState().isFocusable());
}

//==============================================================================
// What a repaint inside the card costs behind it, and the other way round. Probes over the window's parts (each a child of
// one) and one over the settings (a child of the panel) count their paints when the window paints what was asked to repaint,
// as its peer would.
namespace
{
struct PaintProbe final : juce::Component
{
    PaintProbe() { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics&) override { ++paints; }
    int paints = 0;
};

// Paints what the window was asked to repaint since the last call, as its peer would; where that was.
juce::Rectangle<int> paintRepainted (phazite::test::TestWindow& window)
{
    const auto area = window.takeRepaintedArea();
    juce::Image image (juce::Image::ARGB, window.getWidth(), window.getHeight(), true, juce::SoftwareImageType());
    juce::Graphics g (image);

    if (g.reduceClipRegion (area))
        window.paintEntireComponent (g, true);

    return area.getBounds();
}

// The card's opaque part alone, painted onto nothing: every pixel of it must be painted, opaque.
bool paintsEveryPixel (ModalCard& modal)
{
    juce::Image image (juce::Image::ARGB, modal.getWidth(), modal.getHeight(), true, juce::SoftwareImageType());
    {
        juce::Graphics g (image);
        modal.paintEntireComponent (g, true);
    }

    const juce::Image::BitmapData pixels (image, juce::Image::BitmapData::readOnly);

    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
            if (pixels.getPixelColour (x, y).getAlpha() != 0xff)
                return false;

    return true;
}
} // namespace

// The card's opaque part is a sibling in front of the window's parts, which JUCE paints nothing of behind it: a repaint in the
// card (a row's wash, the list's scroll, a caret's blink, a hover) paints nothing of the window behind it, as the old dialog,
// a window of its own, didn't. (Before the card was split so, each of those painted the playlist behind it once.)
TEST_CASE ("A repaint inside the settings' card paints nothing of the window behind it, and one of the window beside the card nothing "
           "of the settings")
{
    ModalScreen s;
    auto& playlist = *phazite::test::findAll<phazite::ui::Playlist> (s.screen.shell).getFirst();
    auto& panel = s.open();
    phazite::test::Mouse mouse (s.screen.window);
    mouse.moveTo ({ -100, -100 });
    const auto card = s.modal().getCardBounds();

    // The playlist, and the other parts of the window the card covers some of (the transport bar and the mixer, where they are
    // under it, and the lower panel's view).
    juce::OwnedArray<PaintProbe> probes;
    juce::Array<juce::Component*> probed;
    const auto probe = [&probes, &probed] (juce::Component& part)
    {
        auto* added = probes.add (std::make_unique<PaintProbe>());
        part.addAndMakeVisible (*added);
        added->setBounds (part.getLocalBounds());
        probed.add (&part);
    };

    probe (playlist);

    for (auto* part : { static_cast<juce::Component*> (phazite::test::findAll<phazite::ui::TopBar> (s.screen.shell).getFirst()),
                        static_cast<juce::Component*> (phazite::test::findAll<phazite::ui::MixerView> (s.screen.shell).getFirst()),
                        static_cast<juce::Component*> (phazite::test::findAll<phazite::ui::DevicesView> (s.screen.shell).getFirst()) })
    {
        if (part != nullptr && part->isShowing() && s.inWindow (*part).intersects (card))
        {
            MESSAGE ("under the card too: " << part->getTitle() << " " << s.inWindow (*part).toString());
            probe (*part);
        }
    }

    PaintProbe over;
    panel.addAndMakeVisible (over);
    over.setBounds (panel.getLocalBounds());
    const juce::ScopeGuard probesGone { [&]
                                        {
                                            for (int i = 0; i < probes.size(); ++i)
                                                probed[i]->removeChildComponent (probes[i]);

                                            panel.removeChildComponent (&over);
                                        } };

    const auto behind = [&probes]
    {
        auto paints = 0;

        for (auto* each : probes)
            paints += each->paints;

        return paints;
    };

    const auto startCounting = [&]
    {
        paintRepainted (s.screen.window);

        for (auto* each : probes)
            each->paints = 0;

        over.paints = 0;
    };

    auto* row = panel.getShownRows()[1];
    REQUIRE (row != nullptr);
    REQUIRE (s.inWindow (*row).intersects (s.inWindow (playlist)));
    startCounting();

    // A row's wash.
    row->repaint();
    auto area = paintRepainted (s.screen.window);
    MESSAGE ("a row repainted (" << area.toString() << "): the window behind painted " << behind() << " time(s)");
    CHECK (behind() == 0);
    CHECK (over.paints == 1);

    // A header button's hover wash (as a row's is the row's repaint above): the buttons reach higher than the corners' radius.
    startCounting();
    s.modal().getMaximizeButton().repaint();
    area = paintRepainted (s.screen.window);
    MESSAGE ("Maximize's hover (" << area.toString() << "): the window behind painted " << behind() << " time(s)");
    CHECK (area == s.inWindow (s.modal().getMaximizeButton()));
    CHECK (behind() == 0);

    // The search's caret blinking.
    startCounting();
    REQUIRE (panel.getSearchField().hasKeyboardFocus (false));
    auto* caret = phazite::test::findAll<juce::CaretComponent> (panel.getSearchField()).getFirst();
    REQUIRE (caret != nullptr);
    caret->setVisible (! caret->isVisible());
    area = paintRepainted (s.screen.window);
    MESSAGE ("the caret blinked (" << area.toString() << "): the window behind painted " << behind() << " time(s)");
    CHECK_FALSE (area.isEmpty());
    CHECK (behind() == 0);

    // The list scrolled by a wheel's notch.
    startCounting();
    panel.getList().setViewPosition (0, 60);
    area = paintRepainted (s.screen.window);
    MESSAGE ("the list scrolled (" << area.toString() << "): the window behind painted " << behind() << " time(s)");
    CHECK (behind() == 0);

    // The other way: the window repainted beside the card, under the backdrop alone (the transport bar, above it, as its
    // position moves while the song plays): nothing of the settings.
    startCounting();
    auto* topBar = phazite::test::findAll<phazite::ui::TopBar> (s.screen.shell).getFirst();
    REQUIRE (topBar != nullptr);
    REQUIRE_FALSE (s.inWindow (*topBar).intersects (card));
    topBar->repaint();
    area = paintRepainted (s.screen.window);
    CHECK (area == s.inWindow (*topBar));
    CHECK (over.paints == 0);

    // And a column under the card, as the playhead repaints one while the song plays: the card over it paints again where it
    // crosses it, as the window behind is live.
    startCounting();
    const auto x = playlist.getLocalPoint (&s.screen.window, s.inWindow (*row).getCentre()).x;
    playlist.repaint (x, 0, 2, playlist.getHeight());
    area = paintRepainted (s.screen.window);
    MESSAGE ("a playhead's column under the card (" << area.toString() << "): the settings painted " << over.paints << " time(s)");
}

// The card's opaque part paints every pixel of itself, and where it meets the backdrop's rows at the card's top and foot and
// its edge down the sides, the card is the same as the backdrop paints it whole: its corners round, its edge unbroken.
TEST_CASE ("The settings' card paints every pixel of its opaque part, and is seamless where it meets the rows its corners cut into, "
           "dark and light")
{
    ModalScreen s;
    auto& panel = s.open();
    phazite::test::Mouse (s.screen.window).moveTo ({ -100, -100 });
    auto& modal = s.modal();

    for (const auto theme : { phazite::ui::ThemeName::dark, phazite::ui::ThemeName::light })
    {
        CAPTURE (theme == phazite::ui::ThemeName::dark ? "dark" : "light");
        s.lookAndFeel.setTheme (theme);
        phazite::ui::refreshTheme (s.screen.window);
        CHECK (paintsEveryPixel (modal));

        // The window with the card, and with only its backdrop, which paints the whole card where nothing covers it: the same
        // along the joins (the rows either side of them, and the edge's columns down the sides).
        juce::Component::unfocusAllComponents(); // nothing outlined in the header
        CHECK (panel.isShowing());
        const auto card = modal.getCardBounds();
        const auto whole = phazite::test::paintComponent (s.screen.window);
        modal.setVisible (false);
        const auto backdropOnly = phazite::test::paintComponent (s.screen.window);
        modal.setVisible (true);

        auto differ = 0;
        const auto compare = [&] (int x, int y)
        {
            if (whole.getPixelAt (x, y) != backdropOnly.getPixelAt (x, y))
            {
                if (++differ < 10)
                    MESSAGE ("differs at " << (x - card.getX()) << ", " << (y - card.getY())
                                           << " of the card: " << whole.getPixelAt (x, y).toDisplayString (true) << " against "
                                           << backdropOnly.getPixelAt (x, y).toDisplayString (true));
            }
        };

        for (int x = card.getX(); x < card.getRight(); ++x)
            for (const auto y : { card.getY() + cornerRows - 1, card.getY() + cornerRows, card.getY() + cornerRows + 1,
                                  card.getBottom() - footRows - 1, card.getBottom() - footRows })
                if (y != card.getBottom() - footRows - 1 || x < card.getX() + phazite::ui::SettingsTint::sideInset) // the list's foot
                    compare (x, y);

        for (int y = card.getY(); y < card.getBottom(); ++y)
            for (const auto x : { card.getX(), card.getX() + 1, card.getRight() - 1 })
                compare (x, y);

        CHECK (differ == 0);
    }
}

#if ! JUCE_MAC
// Without the card, nothing of Phazite's takes Alt+F4, and the system closes the window with it (MainWindow::closeButtonPressed(),
// which quits). JUCE keeps it from the system while a component is modal, so the card runs Quit for it: from a control, with
// nothing in the card focused, and from the shortcuts editor's key card, which lets it through on purpose.
TEST_CASE ("Alt+F4 quits while the settings show, from a control, with nothing focused and from the shortcuts' key card, as it "
           "closes the window without them")
{
    ModalScreen s;
    phazite::ui::WindowKeys windowKeys { s.screen.window, s.screen.shortcutKeys, s.screen.keyboard }; // as the main window's
    auto quits = 0;
    REQUIRE (s.screen.registry.find ("app.quit") == nullptr); // the main window's (MainWindow.cpp): here, a count
    REQUIRE (s.screen.registry.add ({ .id = "app.quit", .name = "Quit Phazite", .perform = [&quits] { ++quits; } }));
    auto* peer = s.screen.window.getPeer();
    REQUIRE (peer != nullptr);
    const juce::KeyPress altF4 (juce::KeyPress::F4Key, juce::ModifierKeys::altModifier, 0);

    phazite::test::findAll<phazite::ui::Playlist> (s.screen.shell).getFirst()->grabKeyboardFocus();
    CHECK_FALSE (peer->handleKeyPress (altF4));
    CHECK (quits == 0);

    // A row's control; nothing focused. The quit asks about unsaved changes over the card, which stays meanwhile.
    auto& panel = s.open();
    auto* control = panel.getShownRows().getFirst()->getControl();
    REQUIRE (control != nullptr);
    control->grabKeyboardFocus();
    CHECK (peer->handleKeyPress (altF4));
    CHECK (quits == 1);
    CHECK (s.isOpen());
    juce::Component::unfocusAllComponents();
    CHECK (peer->handleKeyPress (altF4));
    CHECK (quits == 2);

    // The Shortcuts page's key card, which takes every other key.
    panel.showGroup (SettingsPanel::shortcuts);
    auto& editor = panel.getShortcuts().getEditor();
    editor.getList().selectRow (0);
    editor.changeSelectedKey();
    auto* capture = editor.getKeyCapture();
    REQUIRE (capture != nullptr);
    capture->grabKeyboardFocus();
    REQUIRE (capture->hasKeyboardFocus (false));
    CHECK (peer->handleKeyPress (altF4));
    CHECK (quits == 3);
    CHECK (editor.getKeyCapture() == capture);
}
#endif

// As another of Phazite's windows is clicked while the settings show, or Phazite is started again (Main.cpp), the main window
// comes forward with the keys where they were in the card, not on its first control (where Main.cpp's toFront (true) on the
// modal component put them): also where JUCE, as the window becomes active, brings its modal components forward.
TEST_CASE ("The main window comes forward with the keys where they were in the settings' card")
{
    ModalScreen s;
    auto& panel = s.open();
    auto* control = panel.findRow ("Undo Steps")->getControl(); // a control further down the list than the search
    REQUIRE (control != nullptr);
    control->grabKeyboardFocus();
    REQUIRE (control->hasKeyboardFocus (false));
    auto& modal = s.modal();

    // The keys still there, and gone with the window to the back (its peer takes them from it as it loses the focus, and keeps
    // where they were).
    auto* peer = s.screen.window.getPeer();
    REQUIRE (peer != nullptr);

    for (const auto keysGone : { false, true })
    {
        CAPTURE (keysGone);

        for (const auto& [way, bring] :
             std::initializer_list<std::pair<const char*, std::function<void()>>> {
                 { "Phazite started again", [&modal] { modal.bringWindowForward(); } },
                 { "another window clicked", [&modal] { modal.inputAttemptWhenModal(); } },
                 { "JUCE's modal components forward", // and the window's focus back, which the system tells it of
                   [peer]
                   {
                       juce::ModalComponentManager::getInstance()->bringModalComponentsToFront();
                       peer->handleFocusGain();
                   } } })
        {
            CAPTURE (way);

            if (keysGone)
            {
                peer->handleFocusLoss();
                REQUIRE (juce::Component::getCurrentlyFocusedComponent() == nullptr);
            }

            bring();
            CHECK (control->hasKeyboardFocus (false));
            CHECK (s.isOpen());
        }
    }
}

//==============================================================================
// Pictures of the whole window with the settings open, for a person to compare with VS Code's (docs/vscode-look.md): 1280 by
// 800, 1600 by 900 and the narrow layout at 800 by 500, dark and light, at the Audio group and searching "@modified".
TEST_CASE ("Pictures of the window with the settings open" * doctest::test_suite ("settings-pictures") * doctest::skip())
{
    const juce::File folder (juce::SystemStats::getEnvironmentVariable ("PHAZITE_PICTURES", {}));
    REQUIRE_MESSAGE (folder.isDirectory(), "set PHAZITE_PICTURES to a folder for the pictures");
    const auto repository = phazite::test::getGoldenFolder().getParentDirectory().getParentDirectory();

    for (const auto theme : { phazite::ui::ThemeName::dark, phazite::ui::ThemeName::light })
    {
        for (const auto& size : { juce::Point<int> (1280, 800), juce::Point<int> (1600, 900), juce::Point<int> (800, 500) })
        {
            phazite::ui::LookAndFeel look { repository.getChildFile ("resources/fonts/Inter") };
            look.setTheme (theme);
            juce::LookAndFeel::setDefaultLookAndFeel (&look);
            const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };

            phazite::test::AppScreen screen;
            screen.window.setSize (size.x, size.y);
            phazite::ui::refreshTheme (screen.window);
            screen.session.openAtStartup();
            dispatchPendingMessages();

            // A few tracks with notes behind, and two settings changed from their defaults.
            for (int i = 0; i < 4; ++i)
                REQUIRE (screen.registry.invoke ("track.add"));

            auto& song = *phazite::findSong (screen.session.getContext());

            for (const auto track : phazite::getAllPlaylistTracks (screen.session.getContext()))
            {
                const auto clip = phazite::test::addMidiClip (song, track, { phazite::test::at (0.0), phazite::test::at (8.0) });

                for (int n = 0; n < 8; ++n)
                    phazite::test::addMidiNote (song, clip, 60 + (n * 5) % 12, phazite::test::at (n), phazite::test::lasting (0.5));
            }

            screen.services.settings.set (phazite::Settings::undoSteps, 500.0);
            screen.services.settings.set (phazite::Settings::scrollContinuously, true);
            dispatchPendingMessages();

            auto panel = std::make_unique<SettingsPanel> (screen.services.engineContext, screen.commands, screen.keyboard,
                                                          screen.session.getContext(), screen.services.settings,
                                                          SettingsPanel::Actions { {}, [] (float) {} });
            auto& shown = *panel;
            screen.shell.showModal (std::move (panel), "Settings", phazite::ui::Icon::settings);
            shown.showGroup (SettingsPanel::audio);
            phazite::test::Mouse (screen.window).moveTo ({ -100, -100 });

            for (const auto* search : { "", "@modified" })
            {
                shown.search (search);
                dispatchPendingMessages();
                const auto picture = screen.window.createComponentSnapshot (screen.window.getLocalBounds(), true, 1.0f);
                const auto name = juce::String ("settings-") + juce::String (size.x) + "x" + juce::String (size.y) + "-"
                                  + (theme == phazite::ui::ThemeName::dark ? "dark" : "light")
                                  + (juce::String (search).isEmpty() ? "-audio" : "-modified") + ".png";
                const auto file = folder.getChildFile (name);
                file.deleteFile();
                juce::FileOutputStream stream (file);
                REQUIRE (stream.openedOk());
                REQUIRE (juce::PNGImageFormat().writeImageToStream (picture, stream));
                MESSAGE (("Wrote " + file.getFullPathName()).toStdString());
            }

            screen.shell.closeModal();
        }
    }
}
