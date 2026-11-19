// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginWindow.h"
#include "SandboxedEditor.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/presets/PresetModel.h"
#include "services/Library.h"
#include "services/Settings.h"
#include "ui/browser/Browser.h"
#include "ui/devices/DeviceWindowContent.h"
#include "ui/devices/DrumSamplerEditor.h"
#include "ui/devices/SidechainMenus.h"
#include "ui/input/PluginKeys.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite
{
namespace
{
    const PluginWindowServices* services = nullptr; // the main window's, while it is open

    constexpr int checksPerSecond = 2; // how often a window looks whether what it shows is still there
    constexpr int cascadeOffset = 30;  // how far right and down of the last a window opens, when it has no place kept

    constexpr int keyPickerWidth = 160; // the header's sidechain, at its right end, after its label

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

        // Its folder among the user's presets, named after it in the plug-ins' folder (PresetModel.h: getPresetFolderName()).
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

                                          // Once the plug-in's state is in (known bug 166); a save that failed says why itself
                                          // (PresetModel.h: saveDevicePreset()).
                                          services->context.withPluginStates (
                                              [context = &services->context, library = &services->library, device = id, file] (bool go)
                                              {
                                                  if (go && saveDevicePreset (*context, device, file))
                                                      library->notePresetSaved(); // the browser shows it
                                              });
                                      });
        }

        void loadPreset (const juce::File& file)
        {
            if (! isInOpenProject())
                return;

            // Once the plug-in's state is in, so that an undo brings back what it has now (known bug 166).
            loadPresetWithPluginStates (services->context, id, file,
                                        [file, plugin = name] (bool loaded)
                                        {
                                            if (! loaded)
                                                showProblem (plugin, "Phazite couldn't load " + file.getFileNameWithoutExtension()
                                                                         + " into " + plugin
                                                                         + ": the preset is another device's, or Phazite can't read it.");
                                        });
        }

        // One that comes after its window may have gone (a load that waited for the plug-in's state).
        static void showProblem (const juce::String& title, const juce::String& message)
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                              .withIconType (juce::MessageBoxIconType::WarningIcon)
                                              .withTitle (title)
                                              .withMessage (message)
                                              .withButton ("OK", 0),
                                          nullptr);
        }

        void showProblem (const juce::String& message)
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                              .withIconType (juce::MessageBoxIconType::WarningIcon)
                                              .withTitle (name)
                                              .withMessage (message)
                                              .withButton ("OK", 0)
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
    // state in the song: model/mixer/internal/Mixer.h), on a screen that is there; it floats over the main window; and it
    // closes through its owner, as its close button is pressed and as what it shows goes.
    class DeviceWindowFrame : public juce::DocumentWindow,
                              private juce::Timer
    {
    public:
        DeviceWindowFrame (DeviceWindows& windows, Song& s, DeviceID d)
            : juce::DocumentWindow ({}, {}, juce::DocumentWindow::closeButton, false), // on the desktop once placed
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

        // Puts it on the desktop there, on a screen that is there, floating over a window (the main window, if there is one):
        // above it but not above other apps, minimised and moved with it, and off the taskbar (JUCE's associated component).
        // Then its moves are kept.
        void place (juce::Component* over, juce::Point<int> topLeft)
        {
            setBounds (onScreen (getBounds().withPosition (topLeft)));
            setAssociatedComponent (over);
            addToDesktop();
            placed = keepMoves = true;
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

        void timerCallback() override
        {
            // A device the song no longer has (deleted, its track deleted, or the step that made it undone), or whose track
            // is frozen, shows nothing: its window goes. (This deletes the window, which a timer's callback may do.)
            auto* shown = song.get();

            if (shown == nullptr || ! findDeviceState (*shown, id).isValid() || isFrozenDevice (*shown, id) || ! refresh (*shown))
                owner.close (id);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceWindowFrame)
    };

    //==============================================================================
    // A plug-in's window: its own editor, under the header with the Plug-in menu and, for a plug-in with a sidechain, its key.
    // It shows its instance in the song's playback session (model/plugins/internal/PluginInstances.h), and that instance's
    // editor again after the plug-in was reloaded.
    class PluginWindow final : public DeviceWindowFrame
    {
    public:
        // With its editor, made already (DeviceWindows::show()), of its instance as reloaded so many times.
        PluginWindow (DeviceWindows& windows, ProjectContext& context, Song& s, DeviceID d, std::unique_ptr<SandboxedEditor> made,
                      int madeGeneration)
            : DeviceWindowFrame (windows, s, d),
              menu (context, d, *this)
        {
            setName (getDeviceName (s, d));
            setMenuBar (&menu); // the bar's height, then the header in its place, which the window owns
            header = new PluginWindowHeader (context, d, menu);
            setMenuBarComponent (header);

            if (made != nullptr)
            {
                instance = made->getInstance();
                generation = madeGeneration;
                setEditor (std::move (made));
            }
        }

        ~PluginWindow() override
        {
            stopKeepingMoves();

            // What the plug-in changed in the window, into the song, outside the undo history.
            if (auto* shown = song.get(); shown != nullptr && wasPlaced())
                if (auto* session = findSession (*shown))
                    session->getPlugins().store (id);

            remaking = nullptr;
            setEditor (nullptr);
            header = nullptr;
            setMenuBar (nullptr); // before the menu goes, the header with it
        }

        bool hasEditor() const override { return editor != nullptr || remaking != nullptr || getContentComponent() == &loadingNote; }
        void updateKeys() { menu.updateKeys(); }

    private:
        PluginMenu menu;
        PluginWindowHeader* header = nullptr;        // the window's, along its top
        std::shared_ptr<SandboxedInstance> instance; // what the editor shows
        int generation = 0;                          // the instance's reloads, as the editor was made
        std::unique_ptr<SandboxedEditor> editor;
        std::unique_ptr<SandboxedEditorRequest> remaking; // the editor of another instance, or of one reloaded, to come
        juce::Label loadingNote;                          // in its place while another instance loads

        // Another instance's editor (the song got the device back), or the same one's after it was reloaded, asked of its
        // helper without waiting (known bug 166): the window shows nothing of the plug-in meanwhile, and goes if none comes.
        void showEditorOf (std::shared_ptr<SandboxedInstance> newInstance, int newGeneration)
        {
            remaking = nullptr;
            setEditor (nullptr); // first: the editor going lets go of the instance's callbacks, which the next one takes
            instance = std::move (newInstance);
            generation = newGeneration;
            remaking = requestSandboxedEditor (instance,
                                               [this] (std::unique_ptr<SandboxedEditor> made)
                                               {
                                                   remaking = nullptr;

                                                   if (made == nullptr)
                                                       owner.close (id); // deletes this
                                                   else
                                                       setEditor (std::move (made));
                                               });
        }

        // While another instance of it loads: no editor, a word that says so in its place, at the window's size.
        void showLoading()
        {
            if (getContentComponent() == &loadingNote)
                return;

            remaking = nullptr;
            setEditor (nullptr);
            instance = nullptr;
            generation = -1;
            loadingNote.setText (getName() + " is loading. Its window shows once it has.", juce::dontSendNotification);
            loadingNote.setTitle ("Loading");
            loadingNote.setJustificationType (juce::Justification::centred);
            ui::setThemeColour (loadingNote, juce::Label::textColourId, &ui::Palette::textMuted);
            setContentNonOwned (&loadingNote, false);
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

            // Another instance of it still loading, in the background, as the song got the device back (a version brought
            // back, Discard, a branch switched to in place): the window stays, saying so, and shows its editor once it is
            // in (the review of known bug 166: it closed).
            if (current == nullptr && session != nullptr && getPluginStatus (shown, id) == PluginInstances::Status::loading)
            {
                showLoading();
                return true;
            }

            if (current == nullptr)
                return false;

            if (const auto now = session->getPlugins().getGeneration (id); current != instance || now != generation)
            {
                showEditorOf (std::move (current), now);
                return hasEditor();
            }

            if (header != nullptr)
                header->refresh();

            return true;
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
    };

    // One of Phazite's own devices out of the Devices view, when the user wants it: its editor, in a window named after it
    // and its channel ("EQ - Bass"), as large as its editor at first and then as the user makes it, its key's picker in it
    // (a compressor's, a vocoder's). The Devices view says meanwhile where it is, and its row there keeps its switch and mix.
    class DeviceWindow final : public DeviceWindowFrame
    {
    public:
        DeviceWindow (DeviceWindows& windows, ProjectContext& context, Song& s, DeviceID d)
            : DeviceWindowFrame (windows, s, d),
              content (context, d)
        {
            setName (getTitleOf (s, d));
            setContentNonOwned (&content, true);
            setResizable (true, false);
            setResizeLimits (content.getMinimumWidth(), std::min (content.getHeight(), content.getMinimumHeight()), 8192, 8192);

            // A drum sampler's pads swap their sounds from the main window's browser.
            if (auto* drums = dynamic_cast<ui::DrumSamplerEditor*> (content.getEditor());
                drums != nullptr && services != nullptr && services->swap != nullptr)
                drums->onSwap = [d] (int pad)
                {
                    if (services != nullptr && services->swap != nullptr)
                        services->swap (d, pad);
                };

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
        static juce::String getTitleOf (const Song& itsSong, DeviceID device)
        {
            const auto channel = getDeviceChannel (itsSong, device);
            const auto onTrack = channel.isValid() && ! isMaster (itsSong, channel);
            return getDeviceName (itsSong, device) + " - " + (onTrack ? getTrackName (itsSong, channel) : juce::String ("Master"));
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

    // Asked again while it opens: the wait given up (a helper alive that never makes the editor kept it for ever: the second
    // review of known bug 166), its editor dropped if it comes; the next ask asks again.
    if (opening.contains (id))
    {
        close (id);
        return;
    }

    // A plug-in's own editor, once the plug-in has loaded, if it still loads, and its helper has made the editor, which it
    // is asked for without waiting (known bug 166); or one of Phazite's own devices' out of the Devices view. Containers show
    // there only.
    if (opensWindow (*song, id))
    {
        opening[id] = {};
        lookAtOpening();
        return;
    }

    if (popsOut (*song, id))
        place (id, std::make_unique<DeviceWindow> (*this, context, *song, id));
}

void DeviceWindows::place (DeviceID id, std::unique_ptr<juce::DocumentWindow> made)
{
    auto* window = dynamic_cast<DeviceWindowFrame*> (made.get());
    auto* song = findSong (context);

    if (window == nullptr || song == nullptr || ! window->hasEditor())
        return; // a plug-in that makes no editor

    const auto kept = getWindowPosition (*song, id);
    window->place (openOver, kept.has_value() ? *kept : choosePosition (*window));
    window->setVisible (true);
    window->toFront (false);
    windows[id] = std::move (made);
}

bool DeviceWindows::isOpening (DeviceID id) const
{
    return opening.contains (id);
}

void DeviceWindows::lookAtOpening()
{
    // Each plug-in's window to open: dropped once its device has gone or is frozen, or once its plug-in didn't load or makes
    // no editor; its editor asked for once its plug-in has loaded; and the window shown once the editor has come.
    auto* song = findSong (context);
    auto* session = song != nullptr ? findSession (*song) : nullptr;

    for (auto it = opening.begin(); it != opening.end();)
    {
        const auto id = it->first;
        auto& waiting = it->second;

        if (song == nullptr || session == nullptr || ! findDeviceState (*song, id).isValid() || isFrozenDevice (*song, id))
        {
            it = opening.erase (it);
            continue;
        }

        // The instance asked for no longer the song's (an undo or a redo replaced it, or it was reloaded): its editor dropped
        // as it comes (the helper closes it), and the instance there is now asked once it has loaded (the review of known bug
        // 166: a window opened on the old one, whose turns went nowhere).
        auto& plugins = session->getPlugins();

        if (waiting.request != nullptr && (plugins.find (id) != waiting.instance || plugins.getGeneration (id) != waiting.generation))
            waiting = {};

        if (waiting.request == nullptr)
        {
            if (getPluginStatus (*song, id) == PluginInstances::Status::loading)
            {
                ++it;
                continue;
            }

            waiting.instance = plugins.find (id);
            waiting.generation = plugins.getGeneration (id);
            waiting.request = requestSandboxedEditor (waiting.instance, [this, id] (std::unique_ptr<SandboxedEditor> made)
                                                      { editorMade (id, std::move (made)); });

            if (waiting.request == nullptr)
            {
                it = opening.erase (it); // one that didn't load, crashed, or has no editor
                continue;
            }
        }

        ++it;
    }

    if (opening.empty())
        stopTimer();
    else if (! isTimerRunning())
        startTimer (lookMilliseconds);
}

void DeviceWindows::editorMade (DeviceID id, std::unique_ptr<SandboxedEditor> made)
{
    // Its request goes first (this is its call): what it made is the window's from now on, if its instance is still the
    // song's, as reloaded as often; else it is dropped, and the one there is now asked for its own.
    const auto found = opening.find (id);
    const auto generation = found != opening.end() ? found->second.generation : 0;
    const auto asked = found != opening.end() ? found->second.instance : nullptr;
    opening.erase (id);
    auto* song = findSong (context);
    auto* session = song != nullptr ? findSession (*song) : nullptr;

    if (session == nullptr || ! findDeviceState (*song, id).isValid())
        return;

    // Whatever came, editor or none, of an instance the song no longer has (an undo or a redo replaced it before the timer
    // looked, or its helper refused one it was closing): the instance there now is asked once it has loaded.
    if (session->getPlugins().find (id) != asked || session->getPlugins().getGeneration (id) != generation)
    {
        made = nullptr;
        opening[id] = {};
        lookAtOpening();
        return;
    }

    if (made == nullptr)
        return;

    place (id, std::make_unique<PluginWindow> (*this, context, *song, id, std::move (made), generation));
}

void DeviceWindows::timerCallback()
{
    lookAtOpening();
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
    opening.erase (id); // one still to open doesn't: the editor its helper makes is closed there

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
    opening.clear();
    stopTimer();

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

//==============================================================================
struct PluginWindowHeader::Parts
{
    Parts (ProjectContext& c, DeviceID id, juce::MenuBarModel& model)
        : context (c),
          bar (&model)
    {
        if (hasSidechain (context, id))
            key = std::make_unique<ui::SidechainButton> (context, id);
    }

    ProjectContext& context;
    juce::MenuBarComponent bar;
    juce::Label keyLabel { {}, ui::sidechainLabel };
    std::unique_ptr<ui::SidechainButton> key; // a plug-in's with a sidechain
    int shownSongVersion = -1;
};

PluginWindowHeader::PluginWindowHeader (ProjectContext& context, DeviceID id, juce::MenuBarModel& model)
    : parts (std::make_unique<Parts> (context, id, model))
{
    setTitle ("Plug-in window's header");
    parts->bar.getProperties().set (ui::LookAndFeel::onBarProperty, true); // on the header's background
    addAndMakeVisible (parts->bar);

    if (auto* key = parts->key.get())
    {
        parts->keyLabel.setFont (juce::FontOptions (ui::TextSize::small));
        parts->keyLabel.setJustificationType (juce::Justification::centredRight);
        parts->keyLabel.setAccessible (false); // the picker's title says it
        parts->keyLabel.setInterceptsMouseClicks (false, false);
        ui::setThemeColour (parts->keyLabel, juce::Label::textColourId, &ui::Palette::textMuted);
        addAndMakeVisible (parts->keyLabel);
        addAndMakeVisible (*key);
    }
}

PluginWindowHeader::~PluginWindowHeader() = default;

juce::Component* PluginWindowHeader::getKeyPicker() const noexcept
{
    return parts->key.get();
}

void PluginWindowHeader::refresh()
{
    if (parts->key != nullptr && parts->context.getSongVersion() != parts->shownSongVersion)
    {
        parts->shownSongVersion = parts->context.getSongVersion();
        parts->key->refresh();
    }
}

void PluginWindowHeader::paint (juce::Graphics& g)
{
    g.fillAll (ui::getPalette (*this).surfaceRaised); // as a plug-in window's menu bar is
}

void PluginWindowHeader::resized()
{
    // The menu at the left; the key at the right end, as wide as it has room for, beside half of the row.
    auto area = getLocalBounds();

    if (auto* key = parts->key.get())
    {
        const auto labelWidth = ui::getSidechainLabelWidth();
        auto right = area.removeFromRight (std::min (labelWidth + ui::Space::xs + keyPickerWidth, area.getWidth() / 2))
                         .withTrimmedRight (ui::Space::xs)
                         .reduced (0, 2);
        parts->keyLabel.setBounds (right.removeFromLeft (labelWidth));
        right.removeFromLeft (ui::Space::xs);
        key->setBounds (right);
    }

    parts->bar.setBounds (area);
}
} // namespace phazite
