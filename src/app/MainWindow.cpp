// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MainWindow.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/internal/AudioWork.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/engine/StartupSound.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/project/ExampleSongs.h"
#include "model/project/internal/Export.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "ui/components/ParameterText.h"
#include "ui/input/KeymapStore.h"
#include "ui/shell/AboutPanel.h"
#include "ui/shell/CleanUpPanel.h"
#include "ui/shell/ExportPanel.h"
#include "ui/shell/ProgressionsPanel.h"
#include "ui/shell/RenderPanel.h"
#include "ui/shell/UpdatePanel.h"
#include "ui/theme/Icons.h"

#include <juce_audio_utils/juce_audio_utils.h> // PluginListComponent

#include <array>
#include <utility>

namespace phazite
{
namespace
{
    // Interface sizes, on top of the operating system's display scaling (ui/shell/SettingsPanel.h, whose Interface Size picks one).
    using ui::interfaceScales;
    constexpr int firstRecentItemID = 100;

    juce::String toString (ui::ThemeName theme)
    {
        switch (theme)
        {
            case ui::ThemeName::light:
                return "light";
            case ui::ThemeName::highContrast:
                return "highContrast";
            case ui::ThemeName::dark:
            default:
                return "dark";
        }
    }

    // What the settings keep while the theme follows the system's (MainWindow::followSystemTheme()).
    constexpr const char* systemThemeName = "system";

    ui::ThemeName themeFromString (const juce::String& name)
    {
        if (name == "light")
            return ui::ThemeName::light;

        if (name == "highContrast")
            return ui::ThemeName::highContrast;

        return ui::ThemeName::dark;
    }

    // The plug-ins scanning found, in the plug-in list's window (JUCE's), which keeps the folders it scans in Phazite's
    // settings file (none for services with their settings in memory); the services keep the list there (AppServices.h).
    // A scan under way stops as the window closes.
    class PluginList final : public juce::PluginListComponent,
                             private juce::Timer
    {
    public:
        PluginList (PluginHost& h, juce::PropertiesFile* settingsFile)
            : juce::PluginListComponent (h.getFormats(), h.getKnownPlugins(),
                                         Settings::getFolder().getChildFile ("PluginScanDeadMansPedal"), settingsFile,
                                         true), // allow plug-ins that load asynchronously (AUv3)
              host (h)
        {
            host.takeScanProblems(); // those of a scan before this window
            startTimerHz (problemChecksPerSecond);
        }

        // Before JUCE's list waits for its scanning thread to finish: for up to a minute, and then its thread pool ends a
        // thread that hasn't stopped within half a second by force. JUCE workaround W1 (docs/juce-workarounds.md).
        ~PluginList() override
        {
            if (isScanning())
                host.cancelScan();
        }

    private:
        static constexpr int problemChecksPerSecond = 4;
        PluginHost& host;
        juce::StringArray problems; // the scan's, shown as it ends

        // What the scan left out of the files it scanned (a shell file's list cut short), said as JUCE's own window says
        // which files failed, once the scan has ended, rather than left out silently (the review of known bug 181).
        void timerCallback() override
        {
            problems.addArray (host.takeScanProblems());

            if (problems.isEmpty() || isScanning())
                return;

            juce::AlertWindow::showAsync (
                juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                    .withTitle ("Scan complete")
                    .withMessage ("Some plug-ins in these files were left out:\n\n" + std::exchange (problems, {}).joinIntoString ("\n"))
                    .withButton ("OK", 0)
                    .withAssociatedComponent (this),
                nullptr);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginList)
    };
} // namespace

MainWindow::MainWindow (ui::CommandBridge& bridge, ProjectSession& s, ui::LookAndFeel& lf, AppServices& appServices,
                        Surroundings surroundingsToUse)
    : juce::DocumentWindow ("Phazite", lf.getPalette().surface, juce::DocumentWindow::allButtons, surroundingsToUse.addToDesktop),
      surroundings (std::move (surroundingsToUse)),
      commands (bridge),
      session (s),
      lookAndFeel (lf),
      services (appServices),
      computerKeyboard ([&audio = getAudioIO (appServices.engineContext)] (const juce::MidiMessage& m) { audio.playLive (m); },
                        ui::getSystemLayout()),
      shortcutKeys (bridge, ui::getSystemLayout()),
      pluginWindowServices { s.getContext(), appServices.library, appServices.settings, {} }
{
    // Phazite's own title bar on Windows (ui/shell/TitleBar.h), in the bar Windows would draw: the content fills the window,
    // and findControlAtPoint() tells Windows which of its parts are the caption and the buttons. Linux and macOS keep the
    // system's (docs/design.md, section 4). setResizable(), below, makes the window again with getDesktopWindowStyleFlags().
#if JUCE_WINDOWS
    setTitleBarButtonsRequired (0, false);
    setTitleBarHeight (0);
    setUsingNativeTitleBar (false);
#else
    setUsingNativeTitleBar (true);
#endif

    // Phazite's logo as the window's icon, where the system takes it from the window; Windows and macOS take the app's
    // own, which is made from the same drawing (CMakeLists.txt).
#if JUCE_LINUX || JUCE_BSD
    setIcon (ui::createLogoImage (ui::Shell::windowIconSize));
#endif
    setInterfaceScale ((float) services.settings.get (Settings::interfaceScale));

    auto content = std::make_unique<ui::AppShell> (services.engineContext, commands, shortcutKeys, computerKeyboard, s.getContext(),
                                                   services.settings, services.soundPack, services.library, *services.previewer);
    shell = content.get();
    setContentOwned (content.release(), false);

    setResizable (true, false);
    setResizeLimits (800, 500, 16384, 16384);
    restorePlace();

    // Menus, tooltips and the pickers open inside this window, on frosted glass, unless the system reduces transparency
    // (ui/theme/Popups.h).
    ui::setPopupsInside (*this);
    lookAndFeel.setTransparencyReduced (ui::isTransparencyReducedBySystem());

    if (const auto theme = services.settings.get (Settings::theme); theme == systemThemeName)
        followSystemTheme();
    else
        setTheme (themeFromString (theme));

    juce::Desktop::getInstance().addDarkModeSettingListener (this);
    setHoverHelp (services.settings.get (Settings::hoverHelp));

    computerKeyboard.setEnabled (services.settings.get (Settings::computerKeyboard));
    computerKeyboard.onChange = [this]
    {
        services.settings.set (Settings::computerKeyboard, computerKeyboard.isEnabled());
        menuItemsChanged();
    };

    // The menus, in the title bar, or on macOS in the menu bar at the top of the screen.
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (this);
#else
    shell->getTitleBar().setMenus (this);
#endif
    setApplicationCommandManagerToWatch (&commands.getCommandManager());

#if JUCE_WINDOWS
    shell->getTitleBar().setWindowActions (getWindowActions());
#endif

    // Shortcuts work anywhere in the window, and the computer keyboard gets its letters before they do.
    computerKeyboard.leaveKeysOf (&commands); // the key that turns it off stays that command's
    windowKeys = std::make_unique<ui::WindowKeys> (*this, shortcutKeys, computerKeyboard);
    commands.getCommandManager().getKeyMappings()->addChangeListener (this);

    // Play and stop, and the computer keyboard, work over plug-in windows too.
    pluginKeys = std::make_unique<ui::PluginKeys> (shortcutKeys, computerKeyboard);
    ui::setPluginKeys (pluginKeys.get());

    // A value typed into a plug-in's control holds the window while the plug-in reads it: the project's hold for work, the
    // window's one hold (InputHold.h).
    ui::setTypedValueHold ([&context = session.getContext()] (const juce::String& title, ProjectContext::Cancel cancel)
                           { return context.holdForWork (title, std::move (cancel)); });

    // Plug-in windows' menus save and load presets, and keep their key switches in the settings; a drum sampler's pads in its
    // own window swap their sounds from the browser. The devices' windows open over this one, for the project open.
    pluginWindowServices.swap = [this] (DeviceID device, int pad) { shell->startSwap (device, pad); };
    setPluginWindowServices (&pluginWindowServices);
    deviceWindows = std::make_unique<DeviceWindows> (s.getContext(), this);

    // The work that reads much of an audio file (an import, Normalize, Quantize Audio) shows its progress here after a second.
    getAudioWork (s.getContext()).setHost (this);
    // And the window takes no input while it reads, but the transport's keys and the few others that change nothing.
    s.getInputHold().setWindow (
        shell,
        [this] (const juce::KeyPress& key)
        {
            // Escape before a progress window shows is the work's Cancel, as it is once it shows (ProjectSession.h).
            if (key == juce::KeyPress::escapeKey && session.cancelHeldWork())
                return true;

            return shortcutKeys.keyPressedWhileHeld (key);
        },
        [this] (bool isKeyDown) { shortcutKeys.keyStateChanged (isKeyDown, nullptr); });
    s.getContext().setInputHolder (&s.getInputHold()); // which the audio files' work and the versions' holds take

    // The playlist's clips draw their waveforms from Phazite's own cache of their files' levels (ClipWaveforms.h).
    ui::setClipWaveformMaker (getClipWaveformMaker());

    session.dialogParent = this;
    session.setAutoSave (services.settings.get (Settings::autoSave)); // File > Auto Save, as the settings keep it
    session.onAutoSaveChanged = [this] (bool on) { services.settings.set (Settings::autoSave, on); };
    session.onExport = [this] { showExport(); };
    session.onCleanUpAudio = [this] { showCleanUp(); };
    session.onProjectClosing = [this]
    {
        delete exportWindow.getComponent();       // its export holds the song
        delete renderWindow.getComponent();       // its freeze or bounce too
        rendersWaiting.clear();                   // and those waiting their turn
        delete progressionsWindow.getComponent(); // it plays and changes that project's chords
        delete cleanUpWindow.getComponent();      // it lists that project's files
        deviceWindows->closeAll();                // storing what their plug-ins changed into the song
    };
    session.onProjectChanged = [this]
    {
        // The views follow the project context by themselves.
        updateTitle();
        menuItemsChanged(); // the recent projects
    };

    // What the session tells without asking anything, news or a problem, in a card over the window's corner rather than a
    // dialog that stops everything (the owner's UX audit, 2026-11-12).
    session.onNotice = [this] (const juce::String& title, const juce::String& message) { tell (title, message, false); };
    session.onError = [this] (const juce::String& title, const juce::String& message) { tell (title, message, true); };

    // A newer release found marks the settings' icon, and names the update command after it.
    services.updates.addChangeListener (this);
    showUpdateState();

    updateTitle();
    startTimerHz (1); // the title shows unsaved changes

    // Maximized as it was closed, before it shows: it opens so, on the screen its place is on.
    maximizeWhenOnDesktop = services.settings.get (Settings::windowMaximized);
    maximizeIfWaiting();
    placeRestored = true;
    setVisible (true);
}

MainWindow::~MainWindow()
{
    keepPlace(); // as it closes, maximized or not
    stopTimer();
    juce::Desktop::getInstance().removeDarkModeSettingListener (this);
    services.updates.removeChangeListener (this);
    session.onProjectClosing = nullptr;
    session.onProjectChanged = nullptr;
    session.onNotice = nullptr;
    session.onError = nullptr;
    session.onExport = nullptr;
    session.onCleanUpAudio = nullptr;
    session.dialogParent = nullptr;
    getAudioWork (session.getContext()).setHost (nullptr);
    session.getContext().setInputHolder (nullptr);
    session.getInputHold().setWindow (nullptr, nullptr);
    rendersWaiting.clear();

    // Close the dialogs: they point into the engine context and the commands, which are destroyed after this window, and
    // an export, a freeze or a bounce into the project's song. And the devices' windows, which use the keys and services
    // that go next. An export that stops as Phazite quits isn't told.
    if (auto* window = exportWindow.getComponent())
        if (auto* panel = dynamic_cast<ui::ExportPanel*> (window->getContentComponent()))
            panel->onClosedWhileExporting = nullptr;

    delete exportWindow.getComponent();
    delete renderWindow.getComponent();
    delete progressionsWindow.getComponent();
    delete cleanUpWindow.getComponent();
    delete settingsWindow.getComponent();
    delete pluginListWindow.getComponent();
    delete welcomeWindow.getComponent();
    delete aboutWindow.getComponent();
    delete updatesWindow.getComponent(); // it listens to the update check, the services'
    deviceWindows = nullptr;

    setPluginWindowServices (nullptr);
    ui::setPluginKeys (nullptr);
    ui::setTypedValueHold ({});
    session.letGoOfRenderHost(); // the work holding the window cancelled, and no wait's window ended on this after
    ui::setClipWaveformMaker ({});
    pluginKeys = nullptr;
    commands.getCommandManager().getKeyMappings()->removeChangeListener (this);
    windowKeys = nullptr;
    computerKeyboard.onChange = nullptr;
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (nullptr);
#endif
    shell->getTitleBar().setMenus (nullptr);
    shell->getTitleBar().setWindowActions (std::nullopt);
    clearContentComponent();
}

void MainWindow::closeButtonPressed()
{
    requestQuit();
}

void MainWindow::requestQuit()
{
    if (surroundings.quit != nullptr)
        surroundings.quit();
    else
        juce::JUCEApplication::getInstance()->systemRequestedQuit(); // asks to save unsaved changes first
}

juce::DialogWindow* MainWindow::launch (juce::DialogWindow::LaunchOptions& options)
{
    return surroundings.launchDialog != nullptr ? surroundings.launchDialog (options) : options.launchAsync();
}

void MainWindow::activeWindowStatusChanged()
{
    juce::DocumentWindow::activeWindowStatusChanged();

    // Keys released in another window would never reach this one.
    if (! isActiveWindow() && windowKeys != nullptr)
        windowKeys->releaseKeys();

    if (shell != nullptr)
        shell->getTitleBar().setWindowActive (isActiveWindow());
}

auto MainWindow::findControlAtPoint (juce::Point<float> point) const -> WindowControlKind
{
    using Kind = WindowControlKind;

    // Filling the screen, it neither moves nor resizes.
    if (isKioskMode())
        return Kind::client;

    // A band along the top resizes the window, as Windows 11 expects it inside the window, and along the title bar's
    // sides; below the title bar, JUCE gives the content's edges and corners a band of their own. Not while it is maximized.
    if (! isFullScreen())
    {
        const auto band = (float) ui::Shell::resizeBand;
        const auto left = point.x < band, right = point.x >= (float) getWidth() - band;

        if (point.y < band)
            return left ? Kind::sizeTopLeft : right ? Kind::sizeTopRight : Kind::sizeTop;

        if (point.y < (float) ui::Shell::titleBarHeight && (left || right))
            return left ? Kind::sizeLeft : Kind::sizeRight;
    }

    // What pops up inside the window (a menu, the command search, a tooltip: ui/theme/Popups.h) takes the mouse where it is,
    // over the title bar too. By its bounds alone: contains() could ask Windows, which would ask this again.
    for (const auto* child : getChildren())
    {
        auto clicks = false, clicksChildren = false;
        child->getInterceptsMouseClicks (clicks, clicksChildren);

        if (child != shell && clicks && child->isVisible() && dynamic_cast<const juce::ResizableBorderComponent*> (child) == nullptr
            && dynamic_cast<const juce::ResizableCornerComponent*> (child) == nullptr && child->getBounds().toFloat().contains (point))
            return Kind::client;
    }

    // The title bar's empty parts are the caption, which moves the window, and its window buttons are Windows' own.
    return shell != nullptr ? shell->getWindowControlAt (shell->getLocalPoint (this, point.roundToInt())) : Kind::client;
}

void MainWindow::resized()
{
    juce::DocumentWindow::resized();

    // On Windows the band along a restored window's sides that resizes it is inside the window: the panels keep out of it,
    // so that their scroll bars there can be dragged.
#if JUCE_WINDOWS
    if (shell != nullptr)
        shell->setEdgeInset (isFullScreen() || isKioskMode() ? 0 : ui::Shell::edgeInset);
#endif

    keepPlace();
}

void MainWindow::moved()
{
    juce::DocumentWindow::moved();
    keepPlace();
}

juce::Array<juce::Rectangle<int>> MainWindow::getScreens() const
{
    if (surroundings.screens != nullptr)
        return surroundings.screens();

    juce::Array<juce::Rectangle<int>> screens;

    for (const auto& display : juce::Desktop::getInstance().getDisplays().displays)
        screens.add (display.userBounds.getSmallestIntegerContainer()); // neighbours at a fractional scale meet, not a pixel apart

    return screens;
}

void MainWindow::restorePlace()
{
    const auto screens = getScreens();

    if (const auto place = placeWindow (juce::Rectangle<int>::fromString (services.settings.get (Settings::windowPlace)), screens))
    {
        setBounds (*place);
        return;
    }

    // The main screen (JUCE lists it first), with its default size, fitted to a small one.
    if (! screens.isEmpty())
    {
        if (const auto place =
                placeWindow (juce::Rectangle<int> (defaultWidth, defaultHeight).withCentre (screens.getFirst().getCentre()), screens))
        {
            setBounds (*place);
            return;
        }
    }

    centreWithSize (defaultWidth, defaultHeight);
}

void MainWindow::maximizeIfWaiting()
{
    auto* peer = isOnDesktop() ? getPeer() : nullptr;

    if (! maximizeWhenOnDesktop || peer == nullptr)
        return;

    // As JUCE puts a maximized window on the desktop again: maximized, then where it goes back to.
    maximizeWhenOnDesktop = false;
    const auto restored = getBounds();
    setFullScreen (true);
    peer->setNonFullScreenBounds (restored);
}

void MainWindow::parentHierarchyChanged()
{
    juce::DocumentWindow::parentHierarchyChanged();
    maximizeIfWaiting(); // put on the desktop by whoever made it (Surroundings::addToDesktop)
}

void MainWindow::keepPlace()
{
    // Not while it is made, minimized, or fills the screen (View > Full Screen, which isn't kept).
    if (! placeRestored || isMinimised() || isKioskMode())
        return;

    if (isFullScreen())
    {
        services.settings.set (Settings::windowMaximized, true);
        return;
    }

    services.settings.set (Settings::windowMaximized, false);
    services.settings.set (Settings::windowPlace, getBounds().toString());
}

std::optional<juce::Rectangle<int>> placeWindow (juce::Rectangle<int> saved, const juce::Array<juce::Rectangle<int>>& screens)
{
    if (saved.isEmpty())
        return std::nullopt;

    // The screen that holds most of it, and what of it they all hold (neighbours may share a pixel, rounded up from a
    // fractional scale).
    const juce::Rectangle<int>* best = nullptr;
    juce::int64 bestArea = 0;
    juce::RectangleList<int> onScreens;

    for (const auto& screen : screens)
    {
        const auto shared = screen.getIntersection (saved);
        const auto area = (juce::int64) shared.getWidth() * shared.getHeight();
        onScreens.add (shared);

        if (area > bestArea && shared.getWidth() >= windowShownAtLeast && shared.getHeight() >= windowShownAtLeast)
        {
            best = &screen;
            bestArea = area;
        }
    }

    if (best == nullptr)
        return std::nullopt;

    // All on the screens: where it was.
    if (onScreens.containsRectangle (saved))
        return saved;

    // Partly off them: no larger than that screen, and on it.
    return saved.withSize (juce::jmin (saved.getWidth(), best->getWidth()), juce::jmin (saved.getHeight(), best->getHeight()))
        .constrainedWithin (*best);
}

int MainWindow::getDesktopWindowStyleFlags() const
{
    // Phazite draws the window's buttons, but Windows needs to know it has them, for their clicks and its snap layouts.
    auto styleFlags = juce::DocumentWindow::getDesktopWindowStyleFlags();
#if JUCE_WINDOWS
    styleFlags |= juce::ComponentPeer::windowHasMinimiseButton | juce::ComponentPeer::windowHasMaximiseButton
                  | juce::ComponentPeer::windowHasCloseButton;
#endif
    return styleFlags;
}

ui::TitleBar::WindowActions MainWindow::getWindowActions()
{
    return { .minimise = [this] { minimiseButtonPressed(); },
             .toggleMaximised = [this] { maximiseButtonPressed(); },
             .close = [this] { closeButtonPressed(); },
             .isMaximised = [this] { return isFullScreen(); } };
}

bool MainWindow::keyPressed (const juce::KeyPress& key)
{
    if (windowKeys != nullptr && windowKeys->keyPressedAfterListeners (key))
        return true;

    return juce::DocumentWindow::keyPressed (key);
}

void MainWindow::addCommands()
{
    auto& registry = commands.getRegistry();
    shell->addCommands (registry);

    registry.add ({ .id = "app.quit",
                    .name = "Quit Phazite",
                    .category = "App",
                    .description = "Closes Phazite. It asks first if the project has unsaved changes.",
                    .synonyms = { "exit", "close", "leave" },
                    .perform = [this] { requestQuit(); } });

    registry.add ({ .id = "options.settings",
                    .name = "Settings",
                    .category = "Options",
                    .description =
                        "Every preference Phazite keeps: the audio device, MIDI inputs, recording, the look, saving, and the keyboard "
                        "shortcuts.",
                    .synonyms = { "preferences", "options", "setup" },
                    .perform = [this] { showSettings (std::nullopt); } });

    registry.add ({ .id = "options.audioSettings",
                    .name = "Audio Settings",
                    .category = "Options",
                    .description = "Chooses the audio device (ASIO on Windows, for an audio interface), sample rate and buffer size.",
                    .synonyms = { "device", "asio", "latency", "buffer", "sound card", "interface", "output" },
                    .perform = [this] { showSettings (juce::String (ui::SettingsPanel::audio)); } });

    registry.add ({ .id = "options.midiSettings",
                    .name = "MIDI Settings",
                    .category = "Options",
                    .description = "Turns MIDI inputs and the computer keyboard on or off.",
                    .synonyms = { "midi keyboard", "controller", "inputs", "devices" },
                    .perform = [this] { showSettings (juce::String (ui::SettingsPanel::midi)); } });

    registry.add ({ .id = "chords.progressions",
                    .name = "Chord Progressions",
                    .category = "Chords",
                    .description = "Common chord progressions, by mood and genre: play one in the song's key and tempo, then put "
                                   "it on the chord lane.",
                    .synonyms = { "chords", "progression", "harmony", "chord track", "four chords", "pop chords", "ideas" },
                    .perform = [this] { showProgressions(); },
                    .isEnabled = [this] { return session.getContext().hasProject(); } });

    registry.add ({ .id = "input.computerKeyboard",
                    .name = "Play Notes with the Computer Keyboard",
                    .category = "Input",
                    .description = "Plays the selected track's instrument from the typing keyboard: the middle row of letters "
                                   "plays the white keys and the row above the black ones, by where the keys are, and the row "
                                   "below changes the octave and the velocity. Hold Shift for single-key shortcuts.",
                    .synonyms = { "typing keyboard", "qwerty", "musical typing", "piano", "midi" },
                    .perform = [this] { computerKeyboard.setEnabled (! computerKeyboard.isEnabled()); },
                    .isTicked = [this] { return computerKeyboard.isEnabled(); } });

    registry.add ({ .id = "options.scanPlugins",
                    .name = "Scan for Plug-ins",
                    .category = "Options",
                    .description = "Finds your VST3 plug-ins. Scanning runs in a helper process, so a plug-in that crashes or hangs can't "
                                   "take Phazite down.",
                    .synonyms = { "vst", "vst3", "plugins", "instruments", "effects" },
                    .perform = [this] { showPluginList(); } });

    registry.add ({ .id = "options.keyboardShortcuts",
                    .name = "Keyboard Shortcuts",
                    .category = "Options",
                    .description = "Changes the keys of any command. Each keymap keeps its own changes.",
                    .synonyms = { "keys", "hotkeys", "keymap", "bindings", "rebind" },
                    .perform = [this] { showSettings (juce::String (ui::SettingsPanel::shortcuts)); } });

    for (const auto& keymap : ui::Keymap::getBuiltIn())
    {
        const auto name = keymap.name;
        registry.add ({ .id = "options.keymap." + name.removeCharacters (" ").toLowerCase(),
                        .name = "Keymap: " + name,
                        .category = "Options",
                        .description = keymap.description,
                        .synonyms = { "shortcuts", "keys", "keyboard", "hotkeys" },
                        .perform = [this, name] { setKeymap (name); },
                        .isTicked = [this, name] { return commands.getKeymap().name == name; } });
    }

    struct ThemeChoice
    {
        ui::ThemeName theme;
        const char* name;
        const char* description;
    };

    for (const auto& choice :
         { ThemeChoice { ui::ThemeName::dark, "Dark", "Light text on dark panels." },
           ThemeChoice { ui::ThemeName::light, "Light", "Dark text on light panels." },
           ThemeChoice { ui::ThemeName::highContrast, "High Contrast", "White and yellow on black, for the most contrast." } })
    {
        const auto theme = choice.theme;
        registry.add ({ .id = "options.theme." + juce::String (choice.name).removeCharacters (" ").toLowerCase(),
                        .name = "Theme: " + juce::String (choice.name),
                        .category = "Options",
                        .description = choice.description,
                        .synonyms = { "colours", "colors", "appearance", "dark mode", "accessibility" },
                        .perform = [this, theme] { setTheme (theme); },
                        .isTicked = [this, theme] { return ! followsSystemTheme && lookAndFeel.getTheme() == theme; } });
    }

    registry.add ({ .id = "options.theme.followsystem",
                    .name = "Theme: Follow System",
                    .category = "Options",
                    .description = "Dark or light as the operating system is set, changing when it does.",
                    .synonyms = { "colours", "colors", "appearance", "dark mode", "light mode", "automatic", "os theme" },
                    .perform = [this] { followSystemTheme(); },
                    .isTicked = [this] { return followsSystemTheme; } });

    registry.add ({ .id = "options.startupSound",
                    .name = "Startup Sound",
                    .category = "Options",
                    .description = "Plays Phazite's sound once it has opened: a quick blip with echoes, at the browser's preview "
                                   "volume.",
                    .synonyms = { "chime", "jingle", "launch sound", "opening sound", "intro", "sound at start" },
                    .perform = [this] { setStartupSound (! services.settings.get (Settings::startupSound)); },
                    .isTicked = [this] { return services.settings.get (Settings::startupSound); } });

    registry.add ({ .id = "view.interfaceLarger",
                    .name = "Larger Interface",
                    .category = "View",
                    .description = "Makes everything in Phazite's windows bigger, up to twice the normal size.",
                    .synonyms = { "zoom in", "scale", "bigger", "text size", "accessibility" },
                    .perform = [this] { stepInterfaceScale (1); },
                    .isEnabled = [] { return juce::Desktop::getInstance().getGlobalScaleFactor() < interfaceScales.back() - 0.01f; } });

    registry.add ({ .id = "view.interfaceSmaller",
                    .name = "Smaller Interface",
                    .category = "View",
                    .description = "Makes everything in Phazite's windows smaller, down to three quarters of the normal size.",
                    .synonyms = { "zoom out", "scale", "text size" },
                    .perform = [this] { stepInterfaceScale (-1); },
                    .isEnabled = [] { return juce::Desktop::getInstance().getGlobalScaleFactor() > interfaceScales.front() + 0.01f; } });

    registry.add ({ .id = "view.interfaceNormal",
                    .name = "Normal Interface Size",
                    .category = "View",
                    .description = "Shows Phazite at its normal size again.",
                    .synonyms = { "zoom", "reset", "100%", "scale" },
                    .perform = [this] { setInterfaceScale (1.0f); } });

    registry.add ({ .id = "view.fullScreen",
                    .name = "Full Screen",
                    .category = "View",
                    .description = "Fills the whole screen with Phazite's window, over the taskbar, or puts it back.",
                    .synonyms = { "fullscreen", "kiosk", "whole screen", "maximize window" },
                    .perform = [this] { setFullScreenMode (! isFullScreenMode()); },
                    .isTicked = [this] { return isFullScreenMode(); } });

    registry.add ({ .id = "view.hoverHelp",
                    .name = "Hover Help",
                    .category = "View",
                    .description = "Shows what a control does when the pointer rests on it. The hint bar always shows it.",
                    .synonyms = { "tooltips", "help" },
                    .perform = [this] { setHoverHelp (tipsInside == nullptr); },
                    .isTicked = [this] { return tipsInside != nullptr; } });

    registry.add ({ .id = "help.welcome",
                    .name = "Welcome",
                    .category = "Help",
                    .description = "Opens the welcome window: the shortcuts of the app you know, the audio output with a test sound, "
                                   "and Phazite Sounds.",
                    .synonyms = { "getting started", "onboarding", "setup", "first run", "start", "coming from", "test sound" },
                    .perform = [this] { showWelcome (ui::WelcomePanel::comingFrom); } });

    registry.add (
        { .id = "help.phaziteSounds",
          .name = "Phazite Sounds",
          .category = "Help",
          .description = "Downloads Phazite Sounds, 85 free drums, loops and effects to start with, or installs them from a file.",
          .synonyms = { "samples", "sample pack", "sound pack", "library", "download", "drums", "loops", "content" },
          .perform = [this] { showWelcome (ui::WelcomePanel::sounds); } });

    // Updates, asked of Phazite's releases on GitHub only when the user asks, or turned on (services/Updates.h).
    registry.add ({ .id = "help.checkForUpdates",
                    .name = "Check for Updates...",
                    .category = "Help",
                    .description = "Asks Phazite's releases on GitHub whether a newer version is out, and downloads and installs it if you "
                                   "want, once its SHA-256 is the release's.",
                    .synonyms = { "update", "upgrade", "new version", "latest version", "release", "download", "install" },
                    .perform = [this] { showUpdates(); },
                    .currentName = [this] { return ui::getUpdateCommandName (services.updates); } });

    registry.add ({ .id = "options.updateCheck",
                    .name = "Check for Updates When Phazite Opens",
                    .category = "Options",
                    .description =
                        "Off until you turn it on. Each time Phazite opens, it then asks its releases on GitHub for one small file, "
                        "to see whether a newer version is out.",
                    .synonyms = { "update", "automatic updates", "auto update", "new version", "internet", "privacy" },
                    .perform = [this] { services.updates.setChecksAtStart (! services.updates.checksAtStart()); },
                    .isTicked = [this] { return services.updates.checksAtStart(); } });

    registry.add (
        { .id = "help.about",
          .name = "About Phazite",
          .category = "Help",
          .description = "Phazite's version and license, and the licenses and notices of the code and fonts it is built with.",
          .synonyms = { "version", "license", "licence", "credits", "copyright", "notices", "third-party", "open source", "trademarks" },
          .perform = [this] { showAbout(); } });
}

//==============================================================================
void MainWindow::showSettings (std::optional<juce::String> group)
{
    if (settingsWindow != nullptr)
    {
        if (group.has_value() && settingsPanel != nullptr)
            settingsPanel->showGroup (*group);

        settingsWindow->toFront (true);
        return;
    }

    ui::SettingsPanel::Actions actions;
    actions.chooseKeymap = [this] (const juce::String& name) { setKeymap (name); };
    actions.setInterfaceScale = [this] (float scale) { setInterfaceScale (scale); };
    auto* panel = new ui::SettingsPanel (services.engineContext, commands, computerKeyboard, session.getContext(), services.settings,
                                         std::move (actions));
    panel->setSize (ui::SettingsTint::windowWidth, ui::SettingsTint::windowHeight);
    panel->showGroup (group.value_or (ui::SettingsPanel::audio));
    settingsPanel = panel;

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Settings";
    o.dialogBackgroundColour = lookAndFeel.getPalette().surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = this;
    o.content.setOwned (panel);

    settingsWindow = launch (o);
}

void MainWindow::showWelcome (ui::WelcomePanel::Page page)
{
    if (welcomeWindow != nullptr)
    {
        if (welcomePanel != nullptr)
            welcomePanel->showPage (page);

        welcomeWindow->toFront (true);
        return;
    }

    auto* panel = new ui::WelcomePanel (
        services.soundPack, services.engineContext.getAudioDeviceManager(), commands.getKeymap().name,
        [this] (const juce::String& name) { setKeymap (name); },
        [safeThis = juce::Component::SafePointer<MainWindow> (this)] (const juce::String& commandID)
        {
            if (safeThis != nullptr)
                safeThis->commands.invoke (commandID);
        },
        [this] { return services.engineContext.getAudioOutput().isBluetooth; });
    panel->setSize (600, 560);
    panel->showPage (page);
    welcomePanel = panel;

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Welcome to Phazite";
    o.dialogBackgroundColour = lookAndFeel.getPalette().surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = this;
    o.content.setOwned (panel);

    welcomeWindow = launch (o);
}

void MainWindow::showAbout()
{
    if (aboutWindow != nullptr)
    {
        aboutWindow->toFront (true);
        return;
    }

    auto* panel = new ui::AboutPanel();

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = panel->getTitle();
    o.dialogBackgroundColour = lookAndFeel.getPalette().surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = this;
    o.content.setOwned (panel);

    aboutWindow = launch (o);
}

void MainWindow::showUpdates()
{
    // Asked: look again, unless a newer release was found already.
    if (! ui::isUpdateWaiting (services.updates))
        services.updates.check();

    if (updatesWindow != nullptr)
    {
        updatesWindow->toFront (true);
        return;
    }

    auto* panel = new ui::UpdatePanel (services.updates, [this] { installUpdate(); });

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = panel->getTitle();
    o.dialogBackgroundColour = lookAndFeel.getPalette().surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = this;
    o.content.setOwned (panel);

    updatesWindow = launch (o);
}

void MainWindow::installUpdate()
{
    // As quitting does: the project's changes saved first, if the user wants. Then the installer, which waits for Phazite to
    // close before it installs, and opens it again (packaging/windows/Phazite.iss); then Phazite quits.
    session.closeThen (
        [safeThis = juce::Component::SafePointer<MainWindow> (this)] (bool closed)
        {
            if (safeThis == nullptr || ! closed)
                return;

            if (const auto started = safeThis->services.updates.startInstaller(); started.failed())
            {
                safeThis->tell ("Phazite couldn't install the update", started.getErrorMessage(), true);
                return;
            }

            juce::JUCEApplication::quit();
        });
}

void MainWindow::showUpdateState()
{
    shell->getActivityBar().showUpdateBadge (ui::isUpdateWaiting (services.updates));

    // The menus and the command search name the update found, once it is found (not at every step of its download).
    if (const auto name = ui::getUpdateCommandName (services.updates); name != shownUpdateName)
    {
        shownUpdateName = name;
        commands.getCommandManager().commandStatusChanged();
    }
}

void MainWindow::showExport()
{
    if (exportWindow != nullptr)
    {
        exportWindow->toFront (true);
        return;
    }

    auto* project = session.getProject();

    if (project == nullptr)
        return;

    // The song goes into the project's Renders folder (or, while it is untitled, Documents/Phazite/Exports), named after
    // the project, unless the user chooses another file; its stems into a folder next to it.
    auto& song = project->getSong();
    const auto defaultFile = session.getExportFolder().getChildFile (project->getFile().getFileNameWithoutExtension() + ".wav");
    juce::StringArray stemNames;

    for (const auto channel : getStemChannels (song))
        stemNames.add (getTrackName (song, channel));

    const auto range = getExportRange (song);
    const auto seconds = toSeconds (song, range.getEnd()) - toSeconds (song, range.getStart());

    auto* panel = new ui::ExportPanel (
        services.settings, defaultFile, seconds, stemNames,
        [&song] (const ExportSettings& settings, const juce::File& file, std::function<void (const ExportReport&)> onFinished)
        { return startExport (song, file, settings, std::move (onFinished)); });
    panel->setSize (560, 660);

    // Its window closed while it exports (its close button: Escape asks first): the export stops, which a notice says, as the
    // window that would have said so has gone.
    panel->onClosedWhileExporting = [this]
    {
        tell ("Export cancelled",
              "The export stopped as its window closed, before it finished: nothing was written, and a file "
              "that was there is as it was.",
              false);
    };

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Export";
    o.dialogBackgroundColour = lookAndFeel.getPalette().surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = this;
    o.content.setOwned (panel);

    exportWindow = launch (o);
}

void MainWindow::showCleanUp()
{
    if (cleanUpWindow != nullptr)
    {
        cleanUpWindow->toFront (true);
        return;
    }

    auto* panel = new ui::CleanUpPanel (session.getContext());

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = panel->getTitle();
    o.dialogBackgroundColour = lookAndFeel.getPalette().surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = this;
    o.content.setOwned (panel);

    cleanUpWindow = launch (o);
}

void MainWindow::runRender (std::unique_ptr<TrackRender> render)
{
    // One at a time (the window is modal): another waits its turn, never replacing the one that shows, whose work would go
    // on unseen or, cancelled by its going, be lost (an import's copy, say).
    if (render == nullptr)
        return;

    if (renderWindow != nullptr)
    {
        rendersWaiting.push_back (std::move (render));
        return;
    }

    auto* panel = new ui::RenderPanel (std::move (render));
    panel->onCancelled = [safeThis = juce::Component::SafePointer<MainWindow> (this)]
    {
        juce::MessageManager::callAsync (
            [safeThis]
            {
                if (safeThis != nullptr)
                    safeThis->showNextRender();
            });
    };
    panel->onHeldKey = [this] (const juce::KeyPress& key) { return shortcutKeys.keyPressedWhileHeld (key); };
    panel->onHeldKeyState = [this] (bool isKeyDown) { shortcutKeys.keyStateChanged (isKeyDown, nullptr); };

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = panel->getTitle();
    o.dialogBackgroundColour = lookAndFeel.getPalette().surface;
    o.escapeKeyTriggersCloseButton = false; // Cancel stops it, on purpose
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = this;
    o.content.setOwned (panel);

    renderWindow = launch (o);
}

void MainWindow::showNextRender()
{
    if (renderWindow != nullptr || rendersWaiting.empty())
        return;

    auto next = std::move (rendersWaiting.front());
    rendersWaiting.pop_front();
    runRender (std::move (next));
}

void MainWindow::renderFinished (const juce::String& title, const juce::Result& result)
{
    // What it says, taken first: the title and the result may be the render's, which goes with its window.
    const auto shownTitle = title;
    const auto failed = result.failed();
    const auto error = result.getErrorMessage();

    // One waiting its turn ends where it waits; otherwise the one that shows, whose window goes, and the next shows.
    const auto waiting = std::find_if (rendersWaiting.begin(), rendersWaiting.end(),
                                       [&shownTitle] (const std::unique_ptr<TrackRender>& r) { return r->getTitle() == shownTitle; });

    if (waiting != rendersWaiting.end())
    {
        auto ended = std::move (*waiting); // which is calling back: it lets go of itself last
        rendersWaiting.erase (waiting);
        ended = nullptr;
    }
    else
    {
        delete renderWindow.getComponent(); // and the render, which is calling back: it lets go of itself last
        juce::MessageManager::callAsync (
            [safeThis = juce::Component::SafePointer<MainWindow> (this)]
            {
                if (safeThis != nullptr)
                    safeThis->showNextRender();
            });
    }

    if (failed)
        tell (shownTitle, error, true);
}

void MainWindow::renderWrote (const juce::String& title, const juce::File& file)
{
    // Where it went, and the folder shown on asking.
    tell (title, "Phazite wrote " + file.getFileName() + " into " + file.getParentDirectory().getFullPathName() + ".", false,
          "Show in Folder", [file] { file.revealToUser(); });
}

void MainWindow::tell (const juce::String& title, const juce::String& message, bool isProblem, const juce::String& actionName,
                       std::function<void()> action)
{
    if (shell != nullptr)
    {
        shell->getNotices().show (
            { .title = title, .text = message, .isProblem = isProblem, .actionName = actionName, .action = std::move (action) });
        return;
    }

    auto options = juce::MessageBoxOptions()
                       .withIconType (isProblem ? juce::MessageBoxIconType::WarningIcon : juce::MessageBoxIconType::InfoIcon)
                       .withTitle (title)
                       .withMessage (message)
                       .withAssociatedComponent (this);

    if (actionName.isNotEmpty())
        options = options.withButton (actionName, 1);

    juce::AlertWindow::showAsync (options.withButton ("OK", 0),
                                  [then = std::move (action)] (int button)
                                  {
                                      if (button == 1 && then != nullptr)
                                          then();
                                  });
}

void MainWindow::showProgressions()
{
    if (progressionsWindow != nullptr)
    {
        progressionsWindow->toFront (true);
        return;
    }

    if (! session.getContext().hasProject())
        return;

    auto* panel = new ui::ProgressionsPanel (session.getContext(), *services.previewer);
    panel->setSize (640, 560);

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Chord Progressions";
    o.dialogBackgroundColour = lookAndFeel.getPalette().surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = this;
    o.content.setOwned (panel);

    progressionsWindow = launch (o);
}

void MainWindow::showWelcomeOnFirstRun()
{
    if (services.settings.get (Settings::welcomeShown))
        return;

    services.settings.set (Settings::welcomeShown, true);

    // Once the window is on the screen.
    juce::MessageManager::callAsync (
        [safeThis = juce::Component::SafePointer<MainWindow> (this)]
        {
            if (safeThis != nullptr)
                safeThis->showWelcome (ui::WelcomePanel::comingFrom);
        });
}

void MainWindow::showPluginList()
{
    if (pluginListWindow != nullptr)
    {
        pluginListWindow->toFront (true);
        return;
    }

    auto& host = getPluginHost (services.engineContext);
    auto* list = new PluginList (host, services.settingsFile.get());

    // One scanning thread (JUCE's default here): the scanner has one helper, which scans a file at a time.
    list->setNumberOfThreadsForScanning (1);
    list->setSize (800, 600);

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Plug-ins";
    o.dialogBackgroundColour = lookAndFeel.getPalette().surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.useBottomRightCornerResizer = true;
    o.componentToCentreAround = this;
    o.content.setOwned (list);

    pluginListWindow = launch (o);

    // Start a VST3 scan, as the list's "Options... > Scan for new or updated VST3 plug-ins" would.
    for (auto* format : host.getFormats().getFormats())
        if (format->getName() == "VST3")
            list->scanFor (*format);
}

void MainWindow::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &services.updates)
    {
        showUpdateState();
        return;
    }

    // Keys changed, in the shortcuts editor or with another keymap: keep them, and update the
    // tooltips and hints that show them.
    ui::saveKeySettings (commands, services.settings);
    shell->refreshShortcutTexts();
}

void MainWindow::setTheme (ui::ThemeName theme)
{
    followsSystemTheme = false;
    applyTheme (theme);
    services.settings.set (Settings::theme, toString (theme));
    menuItemsChanged();
}

void MainWindow::followSystemTheme()
{
    followsSystemTheme = true;
    applyTheme (juce::Desktop::getInstance().isDarkModeActive() ? ui::ThemeName::dark : ui::ThemeName::light);
    services.settings.set (Settings::theme, juce::String (systemThemeName));
    menuItemsChanged();
}

void MainWindow::darkModeSettingChanged()
{
    if (followsSystemTheme)
        applyTheme (juce::Desktop::getInstance().isDarkModeActive() ? ui::ThemeName::dark : ui::ThemeName::light);
}

void MainWindow::applyTheme (ui::ThemeName theme)
{
    const auto oldSurface = lookAndFeel.getPalette().surface;
    lookAndFeel.setTheme (theme);
    const auto surface = lookAndFeel.getPalette().surface;
    setBackgroundColour (surface); // the window's frame, as the shell paints it

    // This window (even before it is on the screen), and every other one: panels in their own windows, dialogs, whose
    // backgrounds were the old theme's surface as they opened, and tooltips.
    ui::refreshTheme (*this);
    auto& desktop = juce::Desktop::getInstance();

    for (int i = 0; i < desktop.getNumComponents(); ++i)
    {
        if (auto* window = desktop.getComponent (i); window != nullptr && window != this)
        {
            if (auto* dialog = dynamic_cast<juce::ResizableWindow*> (window);
                dialog != nullptr && dialog->getBackgroundColour() == oldSurface)
                dialog->setBackgroundColour (surface);

            ui::refreshTheme (*window);
        }
    }

    repaint();
}

void MainWindow::setHoverHelp (bool on)
{
    tipsInside = on ? std::make_unique<ui::Tooltips> (*this, true) : nullptr;
    tipsElsewhere = on ? std::make_unique<ui::Tooltips> (*this, false) : nullptr;

    services.settings.set (Settings::hoverHelp, on);
    menuItemsChanged();
}

void MainWindow::setInterfaceScale (float scale)
{
    scale = juce::jlimit (interfaceScales.front(), interfaceScales.back(), scale);
    juce::Desktop::getInstance().setGlobalScaleFactor (scale);
    services.settings.set (Settings::interfaceScale, scale);
}

void MainWindow::stepInterfaceScale (int direction)
{
    const auto current = juce::Desktop::getInstance().getGlobalScaleFactor();

    if (direction > 0)
    {
        for (const auto scale : interfaceScales)
            if (scale > current + 0.01f)
                return setInterfaceScale (scale);
    }
    else
    {
        for (auto it = interfaceScales.rbegin(); it != interfaceScales.rend(); ++it)
            if (*it < current - 0.01f)
                return setInterfaceScale (*it);
    }
}

void MainWindow::setKeymap (const juce::String& name)
{
    ui::switchKeymap (commands, name, services.settings);

    shell->refreshShortcutTexts();
    menuItemsChanged();
}

bool MainWindow::isFullScreenMode() const
{
    return juce::Desktop::getInstance().getKioskModeComponent() == this;
}

void MainWindow::setFullScreenMode (bool on)
{
    // Over the whole screen, the taskbar too, or back where it was. The title bar stays, its menus and search with it, but
    // not the window's buttons.
    juce::Desktop::getInstance().setKioskModeComponent (on ? this : nullptr, false);

#if JUCE_WINDOWS
    shell->getTitleBar().setWindowActions (on ? std::nullopt : std::optional (getWindowActions()));
#endif

    // Nor the logo, as VS Code hides its own there (none on macOS: ui/shell/TitleBar.h).
#if ! JUCE_MAC
    shell->getTitleBar().setLogoShown (! on);
#endif
}

void MainWindow::setStartupSound (bool on)
{
    services.settings.set (Settings::startupSound, on);

    // Turned on, it plays, so that what it turns on is heard.
    if (on)
        playStartupSound (*services.previewer);
}

void MainWindow::updateTitle()
{
    // The taskbar's name for the window; and in the title bar and the status bar, the project's name and whether it is saved.
    juce::String title ("Phazite"), name;
    auto unsaved = false;
    std::optional<juce::Time> lastAutosave;

    if (auto* project = session.getProject())
    {
        name = project->getFile().getFileNameWithoutExtension();
        unsaved = project->hasUnsavedChanges();
        lastAutosave = project->getLastAutosave();
        title = name + (unsaved ? "*" : "") + " - Phazite";
    }

    if (getName() != title)
        setName (title);

    if (shell != nullptr)
        shell->showProject (name, unsaved, session.getProject() != nullptr && session.isUntitled(), lastAutosave,
                            session.hasAutosaveFailed());
}

//==============================================================================
juce::StringArray getMainMenuNames()
{
    // As in FL Studio, Live and Bitwig: no Track, Song or Transport menu. Their commands are in Add, Edit and
    // View, as well as on the top bar, the keys, the right-click menus and the command palette. And, as in VS Code, no
    // Options menu: the settings are in the menu of the gear at the activity bar's foot (ui/shell/ActivityBar.h).
    return { "File", "Edit", "Add", "View", "Help" };
}

juce::StringArray MainWindow::getMenuBarNames()
{
    return getMainMenuNames();
}

juce::PopupMenu MainWindow::getMenuForIndex (int menuIndex, const juce::String&)
{
    juce::PopupMenu menu;
    auto& commandManager = commands.getCommandManager();

    const auto addOne = [this, &commandManager] (juce::PopupMenu& m, const juce::String& id)
    {
        const auto commandID = commands.getCommandID (id);
        jassert (commandID != 0); // a command the menu names that the app doesn't have: renamed, or gone

        if (commandID != 0)
            m.addCommandItem (&commandManager, commandID);
    };

    const auto add = [addOne] (juce::PopupMenu& m, std::initializer_list<const char*> ids)
    {
        for (const auto* id : ids)
            addOne (m, id);
    };

    switch (menuIndex)
    {
        case 0: // File
        {
            add (menu, { "project.new", "project.open", "project.importDawProject" });

            juce::PopupMenu recentMenu;
            const auto recent = session.getRecentProjects();

            for (int i = 0; i < recent.size(); ++i)
                recentMenu.addItem (firstRecentItemID + i, recent[i].getFileNameWithoutExtension());

            menu.addSubMenu ("Open Recent", recentMenu, ! recent.isEmpty());
            menu.addSeparator();
            add (menu, { "project.save", "project.saveAs", "project.autoSave", "version.save" });
            menu.addSeparator();
            add (menu, { "project.export", "version.exportDawProject" });
            menu.addSeparator();
            add (menu, { "project.cleanUpAudio" });
            menu.addSeparator();
            add (menu, { "app.quit" });
            break;
        }

        case 1: // Edit
        {
            add (menu, { "edit.undo", "edit.redo", "edit.undoOrRedo" });
            menu.addSeparator();
            add (menu, { "edit.cut", "edit.copy", "edit.paste" });
            menu.addSeparator();
            add (menu, { "edit.delete", "edit.duplicate", "edit.duplicateAsAlias", "edit.rename" });
            menu.addSeparator();
            add (menu, { "edit.selectAll", "edit.deselectAll" });
            menu.addSeparator();
            add (menu, { "edit.quantize", "clip.consolidate" });

            // The piano roll's edits, which act on the selected notes.
            juce::PopupMenu notes;
            add (notes, { "note.transposeUp", "note.transposeDown", "note.octaveUp", "note.octaveDown" });
            notes.addSeparator();
            add (notes, { "note.lengthen", "note.shorten", "note.legato", "note.chop" });
            notes.addSeparator();
            add (notes, { "note.quantizeSettings", "note.legatoSettings", "note.chopSettings", "note.transposeSettings" });
            menu.addSubMenu ("Notes", notes);

            // The selected tracks' edits, which their right-click menu has too.
            juce::PopupMenu tracks;
            add (tracks, { "track.color", "track.mute", "track.solo" });
            tracks.addSeparator();
            add (tracks, { "track.groupIntoFolder", "track.ungroup" });
            tracks.addSeparator();
            add (tracks, { "track.moveUp", "track.moveDown", "track.selectPrevious", "track.selectNext" });
            menu.addSubMenu ("Tracks", tracks);

            menu.addSeparator();
            add (menu, { "edit.insertTime", "edit.removeTime" });
            break;
        }

        case 2: // Add
        {
            add (menu, { "track.add", "track.addFolder", "track.addReturn" });
            menu.addSeparator();
            add (menu, { "section.add" });

            juce::PopupMenu outlines;

            for (const auto& outline : getBuiltInOutlines())
                addOne (outlines, getOutlineCommandID (outline.id));

            menu.addSubMenu ("Song Outline", outlines);

            // The chord lane's progressions, which put chords on it: the playlist shows it once the song has some. And a tempo
            // change where the playhead is, which shows the tempo lane.
            menu.addSeparator();
            add (menu, { "chords.progressions", "transport.addTempoChange" });
            break;
        }

        case 3: // View
        {
            // The panels, each shown or hidden, and the two views used most; the rest in a submenu each, every command as
            // it was (and in the command palette, on its keys and in the right-click menus).
            add (menu, { "view.toggleLeftPanel", "view.toggleSong", "view.toggleEditor", "view.toggleInspector" });
            add (menu, { "view.browser", "view.project", "view.versions" });

            // How the panels sit: one filling the window until the others are restored, one in a window of its own, or
            // a layout of them all.
            juce::PopupMenu panelsMenu;
            add (panelsMenu, { "view.maximizePanel" });
            panelsMenu.addSeparator();
            add (panelsMenu, { "view.maximizeLeftPanel", "view.maximizeSong", "view.maximizeEditor", "view.maximizeInspector" });
            panelsMenu.addSeparator();
            add (panelsMenu, { "view.detachLeftPanel", "view.detachInspector", "view.detachEditor" });
            panelsMenu.addSeparator();
            add (panelsMenu, { "view.layoutBeat", "view.layoutMix", "view.layoutFull", "view.focusMode" });
            menu.addSubMenu ("Panels", panelsMenu);
            add (menu, { "view.fullScreen" });
            menu.addSeparator();
            add (menu, { "view.pianoRoll", "view.mixer" });
            menu.addSeparator();

            juce::PopupMenu zoomMenu;
            add (zoomMenu, { "view.zoomIn", "view.zoomOut", "view.zoomToFit", "view.zoomToSelection", "view.zoomBack" });
            menu.addSubMenu ("Zoom", zoomMenu);

            // The playhead, how the view follows it and where playback loops: the song, unless something else is looped
            // on purpose.
            juce::PopupMenu playbackMenu;
            add (playbackMenu, { "transport.goToStart", "transport.goToEnd", "view.centreOnPlayhead" });
            playbackMenu.addSeparator();
            add (playbackMenu, { "view.followPlayback", "view.scrollContinuously" });
            playbackMenu.addSeparator();
            add (playbackMenu, { "transport.loop", "transport.loopSong", "transport.loopSelection", "section.loop" });
            menu.addSubMenu ("Playback", playbackMenu);

            juce::PopupMenu gridMenu;
            add (gridMenu, { "grid.snap" });
            gridMenu.addSeparator();
            add (gridMenu, { "grid.adaptive", "grid.finer", "grid.coarser", "grid.triplets" });
            menu.addSubMenu ("Grid", gridMenu);

            // The selected tracks' rows in the playlist.
            juce::PopupMenu tracksMenu;
            add (tracksMenu, { "track.taller", "track.shorter" });
            tracksMenu.addSeparator();
            add (tracksMenu, { "track.collapse", "track.collapseAll", "track.pin" });
            menu.addSubMenu ("Tracks", tracksMenu);

            juce::PopupMenu sizeMenu;
            add (sizeMenu, { "view.interfaceLarger", "view.interfaceSmaller", "view.interfaceNormal" });
            menu.addSubMenu ("Interface Size", sizeMenu);
            break;
        }

        case 4: // Help: where to find things
            add (menu, { "view.commandPalette", "options.keyboardShortcuts", "view.hoverHelp", "help.guide" });

            {
                // The lessons, which the hint bar shows step by step (ui/shell/Lessons.h).
                juce::PopupMenu lessonsMenu;

                for (const auto& lesson : ui::Lessons::getAll())
                    addOne (lessonsMenu, "help.lesson." + juce::String (lesson.id));

                lessonsMenu.addSeparator();
                addOne (lessonsMenu, "help.stopLesson");
                menu.addSubMenu ("Lessons", lessonsMenu);

                // Songs that show what Phazite does, each opening as a new project (model/project/ExampleSongs.h).
                juce::PopupMenu examplesMenu;

                for (const auto& song : getExampleSongs())
                    addOne (examplesMenu, "help.example." + juce::String (song.id));

                menu.addSubMenu ("Example Songs", examplesMenu);
            }

            menu.addSeparator();
            add (menu, { "help.phaziteSounds", "help.welcome" });

            // As in VS Code's Help menu: the updates, then About.
            menu.addSeparator();
            add (menu, { "help.checkForUpdates", "help.about" });
            break;

        default:
            break;
    }

    return menu;
}

void MainWindow::menuItemSelected (int menuItemID, int)
{
    // The Open Recent submenu's items (the palette has "Open Recent Project" for them).
    if (const auto recent = session.getRecentProjects(); juce::isPositiveAndBelow (menuItemID - firstRecentItemID, recent.size()))
        session.openProject (recent[menuItemID - firstRecentItemID]);
}
} // namespace phazite
