// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginWindow.h"
#include "SandboxedEditor.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/presets/PresetModel.h"
#include "services/Library.h"
#include "services/Settings.h"
#include "ui/browser/Browser.h"
#include "ui/devices/DeviceWindowContent.h"
#include "ui/input/PluginKeys.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite
{
namespace
{
    const PluginWindowServices* services = nullptr; // the main window's, while it is open

    constexpr int checksPerSecond = 2; // how often a window looks whether what it shows is still there
    constexpr int cascadeOffset = 30;  // how far right and down of the last a window opens, when it has no place kept

    // How the settings know a plug-in in every project: by its format, maker and name ("VST3/Xfer Records/Serum").
    juce::String getIdentity (const juce::PluginDescription& plugin)
    {
        return plugin.pluginFormatName + "/" + plugin.manufacturerName + "/" + plugin.name;
    }

    void updateWindowsKeys(); // every open window's, after a plug-in's switch changed

    // The menu bar's one menu (docs/design.md, section 9; roadmap step 2.12).
    class PluginMenu : public juce::MenuBarModel
    {
    public:
        PluginMenu (ProjectContext& c, DeviceID d, juce::Component& w)
            : context (c),
              id (d),
              window (w)
        {
            if (const auto* song = findSong (context))
            {
                if (const auto device = findDeviceState (*song, id); isPluginState (device))
                {
                    name = getDeviceName (*song, id);
                    identity = getIdentity (getPluginIdentity (device));
                }
            }
        }

        juce::StringArray getMenuBarNames() override { return { "Plug-in" }; }

        juce::PopupMenu getMenuForIndex (int, const juce::String&) override
        {
            juce::PopupMenu menu;
            const auto inProject = isInOpenProject();
            menu.addItem ("Save Preset...", inProject, false, whileOpen ([this] { savePreset(); }));

            const auto presets = inProject ? makePresetMenu (getPresetFolder()) : juce::PopupMenu();
            menu.addSubMenu ("Load Preset", presets, presets.getNumItems() > 0);

            menu.addSeparator();
            menu.addItem ("Send All Keys to the Plug-in", services != nullptr && identity.isNotEmpty(), getsAllKeys(),
                          whileOpen ([this] { toggleAllKeys(); }));
            return menu;
        }

        void menuItemSelected (int, int) override {} // each item runs its own action

        // Tells the window's keys whether the plug-in gets them all, as the settings say.
        void updateKeys()
        {
            if (auto* keys = ui::getPluginKeys())
                keys->setSendsAllKeys (window, getsAllKeys());
        }

    private:
        ProjectContext& context;
        const DeviceID id;
        juce::Component& window;
        juce::String name, identity; // the plug-in's, as its window opened
        std::unique_ptr<juce::FileChooser> chooser;

        // An item's action, which JUCE runs once the menu has closed: nothing if the window, and this menu with it, has
        // gone meanwhile.
        std::function<void()> whileOpen (std::function<void()> action)
        {
            return [open = juce::Component::SafePointer<juce::Component> (&window), run = std::move (action)]
            {
                if (open != nullptr)
                    run();
            };
        }

        // Presets go to and from the devices of the project the services have open only, while its song has the device.
        bool isInOpenProject() const
        {
            if (services == nullptr || &services->context != &context)
                return false;

            const auto* song = findSong (context);
            return song != nullptr && findDeviceState (*song, id).isValid();
        }

        // Its folder among the user's presets, named after it.
        juce::File getPresetFolder() const
        {
            return services->library.getPresetsFolder().getChildFile (getPresetFolderName (services->context, id));
        }

        // The presets saved for it, in its folder and the folders in it.
        juce::PopupMenu makePresetMenu (const juce::File& folder)
        {
            juce::PopupMenu menu;

            for (const auto& item : services->library.listFolder (folder))
            {
                if (item.kind == LibraryItem::Kind::folder)
                {
                    if (const auto inside = makePresetMenu (item.file); inside.getNumItems() > 0)
                        menu.addSubMenu (item.name, inside);
                }
                else if (item.preset.has_value() && item.preset->kind == PresetKind::device && item.preset->device == name)
                {
                    menu.addItem (item.name, whileOpen ([this, file = item.file] { loadPreset (file); }));
                }
            }

            return menu;
        }

        void savePreset()
        {
            if (! isInOpenProject())
                return;

            ui::askWhereToSavePreset (chooser, getPresetFolder(), name,
                                      [this] (const juce::File& file)
                                      {
                                          if (! isInOpenProject())
                                              return;

                                          if (saveDevicePreset (services->context, id, file))
                                              services->library.notePresetSaved(); // the browser shows it
                                          else
                                              showProblem ("Phazite couldn't save the preset as " + file.getFullPathName() + ".");
                                      });
        }

        void loadPreset (const juce::File& file)
        {
            if (isInOpenProject() && ! loadPresetInto (services->context, id, file))
                showProblem ("Phazite couldn't load " + file.getFileNameWithoutExtension() + " into " + name
                             + ": the preset is another device's, or Phazite can't read it.");
        }

        void showProblem (const juce::String& message)
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                              .withIconType (juce::MessageBoxIconType::WarningIcon)
                                              .withTitle (name)
                                              .withMessage (message)
                                              .withButton ("OK")
                                              .withAssociatedComponent (&window),
                                          nullptr);
        }

        bool getsAllKeys() const
        {
            return services != nullptr && identity.isNotEmpty()
                   && services->settings.get (Settings::pluginsGettingAllKeys).contains (identity);
        }

        // For every project, and every window of the plug-in.
        void toggleAllKeys()
        {
            if (services == nullptr || identity.isEmpty())
                return;

            auto plugins = services->settings.get (Settings::pluginsGettingAllKeys);

            if (plugins.contains (identity))
                plugins.removeString (identity);
            else
                plugins.add (identity);

            services->settings.set (Settings::pluginsGettingAllKeys, plugins);
            updateKeys();
            updateWindowsKeys();
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginMenu)
    };

    //==============================================================================
    // What a device's window does, a plug-in's or one of Phazite's own devices': it opens where it was last (kept as view
    // state in the song: model/mixer/internal/Mixer.h), on a screen that is there; it stays above the main window while
    // Phazite is in front; and it closes through its owner, as its close button is pressed and as what it shows goes.
    class DeviceWindowFrame : public juce::DocumentWindow,
                              private juce::Timer
    {
    public:
        DeviceWindowFrame (DeviceWindows& windows, Song& s, DeviceID d)
            : juce::DocumentWindow ({}, {}, juce::DocumentWindow::closeButton),
              owner (windows),
              song (&s),
              id (d)
        {
            setBackgroundColour (ui::getPalette (*this).surface);
            setUsingNativeTitleBar (true);
            getConstrainer()->setMinimumOnscreenAmounts (0x10000, 50, 30, 50);
        }

        ~DeviceWindowFrame() override
        {
            if (auto* keys = ui::getPluginKeys())
                keys->unwatch (*this);
        }

        // Whether it has something to show (an editor).
        virtual bool hasEditor() const = 0;

        // Shows it there, on a screen that is there; then its moves are kept, and it stays above the main window.
        void place (juce::Point<int> topLeft)
        {
            setBounds (onScreen (getBounds().withPosition (topLeft)));
            placed = keepMoves = true;
            keepOnTop();
            startTimerHz (checksPerSecond);
        }

        void closeButtonPressed() override { owner.close (id); } // deletes this

        void lookAndFeelChanged() override // the theme changed
        {
            juce::DocumentWindow::lookAndFeelChanged();
            setBackgroundColour (ui::getPalette (*this).surface);
        }

        void moved() override
        {
            // As view state, for the next time it opens (model/mixer/internal/Mixer.h).
            if (auto* shown = song.get(); keepMoves && shown != nullptr)
                rememberWindowPosition (*shown, id, getPosition());
        }

    protected:
        DeviceWindows& owner;
        const juce::WeakReference<Song> song;
        const DeviceID id;

        // Whether it was shown; and, as it closes, that what it does then is no move to keep.
        bool wasPlaced() const noexcept { return placed; }
        void stopKeepingMoves() noexcept { keepMoves = false; }

        // Twice a second, while the song has its device: whether it still has something to show (else it goes), and what
        // it shows of the device that changes.
        virtual bool refresh (Song&) = 0;

    private:
        bool placed = false, keepMoves = false;

        // A window kept where it was on a screen that isn't there any more (a project saved with a second screen beside
        // it) comes back to the nearest one.
        static juce::Rectangle<int> onScreen (juce::Rectangle<int> bounds)
        {
            const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (bounds);

            if (display == nullptr)
                return bounds;

            const auto area = display->userBounds.toNearestInt();
            return bounds.withPosition (
                juce::jlimit (area.getX(), std::max (area.getX(), area.getRight() - bounds.getWidth()), bounds.getX()),
                juce::jlimit (area.getY(), std::max (area.getY(), area.getBottom() - bounds.getHeight()), bounds.getY()));
        }

        void keepOnTop()
        {
            if (const auto inFront = juce::Process::isForegroundProcess(); inFront != isAlwaysOnTop())
                setAlwaysOnTop (inFront);
        }

        void timerCallback() override
        {
            // A device the song no longer has (deleted, its track deleted, or the step that made it undone), or whose track
            // is frozen, shows nothing: its window goes. (This deletes the window, which a timer's callback may do.)
            auto* shown = song.get();

            if (shown == nullptr || ! findDeviceState (*shown, id).isValid() || isFrozenDevice (*shown, id) || ! refresh (*shown))
            {
                owner.close (id);
                return;
            }

            keepOnTop();
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceWindowFrame)
    };

    //==============================================================================
    // A plug-in's window: its own editor, under the Plug-in menu. It shows its instance in the song's playback session
    // (model/plugins/internal/PluginInstances.h), and that instance's editor again after the plug-in was reloaded.
    class PluginWindow final : public DeviceWindowFrame
    {
    public:
        PluginWindow (DeviceWindows& windows, ProjectContext& context, Song& s, DeviceID d)
            : DeviceWindowFrame (windows, s, d),
              menu (context, d, *this)
        {
            setName (getDeviceName (s, d));
            setMenuBar (&menu);

            if (auto* session = findSession (s))
            {
                auto& plugins = session->getPlugins();
                auto found = plugins.find (d);
                showEditorOf (std::move (found), plugins.getGeneration (d));
            }
        }

        ~PluginWindow() override
        {
            stopKeepingMoves();

            // What the plug-in changed in the window, into the song, outside the undo history.
            if (auto* shown = song.get(); shown != nullptr && wasPlaced())
                if (auto* session = findSession (*shown))
                    session->getPlugins().store (id);

            setEditor (nullptr);
            setMenuBar (nullptr); // before the menu goes
        }

        bool hasEditor() const override { return editor != nullptr; }
        void updateKeys() { menu.updateKeys(); }

    private:
        PluginMenu menu;
        std::shared_ptr<SandboxedInstance> instance; // what the editor shows
        int generation = 0;                          // the instance's reloads, as the editor was made
        std::unique_ptr<SandboxedEditor> editor;

        void showEditorOf (std::shared_ptr<SandboxedInstance> newInstance, int newGeneration)
        {
            setEditor (nullptr); // first: the editor going lets go of the instance's callbacks, which the next one takes
            instance = std::move (newInstance);
            generation = newGeneration;
            setEditor (createSandboxedEditor (instance));
        }

        void setEditor (std::unique_ptr<SandboxedEditor> newEditor)
        {
            setConstrainer (nullptr);
            clearContentComponent();
            editor = std::move (newEditor);

            if (editor == nullptr)
                return;

            setContentNonOwned (editor.get(), true);
            setResizable (editor->isResizable(), false);

            // Play and stop, and the computer keyboard, work while the plug-in is being tweaked (ui/input/PluginKeys.h),
            // unless it gets every key.
            if (auto* keys = ui::getPluginKeys())
            {
                keys->watch (*this);
                menu.updateKeys();
            }
        }

        // Its plug-in gone (the window goes), or loaded again: another instance (the song got the device back), or the same
        // one opened again after its helper went, whose editor it then shows.
        bool refresh (Song& shown) override
        {
            auto* session = findSession (shown);
            auto current = session != nullptr ? session->getPlugins().find (id) : nullptr;

            if (current == nullptr)
                return false;

            if (const auto now = session->getPlugins().getGeneration (id); current != instance || now != generation)
            {
                showEditorOf (std::move (current), now);
                return hasEditor();
            }

            return true;
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
    };

    // One of Phazite's own devices out of the Devices view, when the user wants it: its editor, in a window named after it
    // and its channel ("EQ - Bass"), as large as its editor at first and then as the user makes it. Its card in the Devices
    // view stays folded meanwhile.
    class DeviceWindow final : public DeviceWindowFrame
    {
    public:
        DeviceWindow (DeviceWindows& windows, Song& s, DeviceID d)
            : DeviceWindowFrame (windows, s, d),
              content (makeDeviceHandle (s, d))
        {
            setName (getTitleOf (s, d));
            setContentNonOwned (&content, true);
            setResizable (true, false);
            setResizeLimits (content.getMinimumWidth(), std::min (content.getHeight(), content.getMinimumHeight()), 8192, 8192);

            // It is Phazite's own: every shortcut works in it, and the computer keyboard, as in the main window, while the
            // app's keys are there (until the main window goes).
            if (auto* keys = ui::getPluginKeys())
                keys->watchAsOwnWindow (*this);
        }

        ~DeviceWindow() override
        {
            stopKeepingMoves();
            clearContentComponent();
        }

        bool hasEditor() const override { return content.hasEditor(); }

        bool keyPressed (const juce::KeyPress& key) override
        {
            auto* keys = ui::getPluginKeys();
            return (keys != nullptr && keys->keyPressedAfterListeners (*this, key)) || DeviceWindowFrame::keyPressed (key);
        }

        void activeWindowStatusChanged() override
        {
            DeviceWindowFrame::activeWindowStatusChanged();

            // Keys released in another window would never reach this one.
            if (auto* keys = ui::getPluginKeys(); keys != nullptr && ! isActiveWindow())
                keys->releaseKeys (*this);
        }

    private:
        ui::DeviceWindowContent content;

        // "EQ - Bass": the device and its channel (the track it is on, or whose container it is in), or the master.
        static juce::String getTitleOf (const Song& song, DeviceID id)
        {
            const auto channel = getDeviceChannel (song, id);
            const auto onTrack = channel.isValid() && ! isMaster (song, channel);
            return getDeviceName (song, id) + " - " + (onTrack ? getTrackName (song, channel) : juce::String ("Master"));
        }

        bool refresh (Song& shown) override // the device or its track renamed
        {
            if (const auto title = getTitleOf (shown, id); title != getName())
                setName (title);

            return true;
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceWindow)
    };

    void updateWindowsKeys()
    {
        auto& desktop = juce::Desktop::getInstance();

        for (int i = 0; i < desktop.getNumComponents(); ++i)
            if (auto* window = dynamic_cast<PluginWindow*> (desktop.getComponent (i)))
                window->updateKeys();
    }
} // namespace

//==============================================================================
DeviceWindows::DeviceWindows (ProjectContext& c, juce::Component* over)
    : context (c),
      openOver (over)
{
    context.addListener (this);
    setDeviceWindows (context, this);
}

DeviceWindows::~DeviceWindows()
{
    if (getDeviceWindows (context) == this)
        setDeviceWindows (context, nullptr);

    closeAll();
    context.removeListener (this);
}

void DeviceWindows::show (DeviceID id)
{
    if (auto* open = findWindow (id))
    {
        open->toFront (true);
        return;
    }

    auto* song = findSong (context);

    if (song == nullptr || ! findDeviceState (*song, id).isValid() || isFrozenDevice (*song, id))
        return;

    // A plug-in's own editor, or one of Phazite's own devices' out of the Devices view. Containers show there only.
    std::unique_ptr<DeviceWindowFrame> window;

    if (opensWindow (*song, id))
        window = std::make_unique<PluginWindow> (*this, context, *song, id);
    else if (popsOut (*song, id))
        window = std::make_unique<DeviceWindow> (*this, *song, id);

    if (window == nullptr || ! window->hasEditor())
        return; // a plug-in that didn't load, or makes no editor

    const auto kept = getWindowPosition (*song, id);
    window->place (kept.has_value() ? *kept : choosePosition (*window));
    window->setVisible (true);
    window->toFront (false);
    windows[id] = std::move (window);
}

juce::Point<int> DeviceWindows::choosePosition (const juce::Component& window) const
{
    // Over the middle of the main window (or the window in front), each one opened then a little right and down of the
    // last, so that none hides another.
    const juce::Component* over = openOver != nullptr ? openOver : juce::TopLevelWindow::getActiveTopLevelWindow();
    auto centre = over != nullptr ? over->getScreenBounds().getCentre() : juce::Point<int>();

    if (over == nullptr)
        if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            centre = display->userBounds.getCentre().toInt();

    const auto cascade = cascadeOffset * (int) windows.size();
    return centre - juce::Point<int> (window.getWidth() / 2, window.getHeight() / 2) + juce::Point<int> (cascade, cascade);
}

bool DeviceWindows::isOpen (DeviceID id) const
{
    return windows.contains (id);
}

void DeviceWindows::close (DeviceID id)
{
    const auto found = windows.find (id);

    if (found == windows.end())
        return;

    // Out of the list first: as it goes, it shows no more.
    auto window = std::move (found->second);
    windows.erase (found);
    window = nullptr;
}

void DeviceWindows::closeAll()
{
    while (! windows.empty())
        close (windows.begin()->first);
}

juce::DocumentWindow* DeviceWindows::findWindow (DeviceID id) const
{
    const auto found = windows.find (id);
    return found != windows.end() ? found->second.get() : nullptr;
}

void setPluginWindowServices (const PluginWindowServices* newServices)
{
    services = newServices;
}

std::unique_ptr<juce::MenuBarModel> createPluginMenu (ProjectContext& context, DeviceID id, juce::Component& window)
{
    return std::make_unique<PluginMenu> (context, id, window);
}
} // namespace phazite
