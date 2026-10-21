// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginWindow.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/DevicePlugin.h"
#include "model/devices/internal/DeviceStates.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/presets/PresetModel.h"
#include "model/project/internal/ProjectContext.h"
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

    // How the settings know a plug-in in every project: by its format, maker and name ("VST3/Xfer Records/Serum").
    juce::String getIdentity (const te::ExternalPlugin& plugin)
    {
        return plugin.desc.pluginFormatName + "/" + plugin.desc.manufacturerName + "/" + plugin.desc.name;
    }

    void updateWindowsKeys(); // every open window's, after a plug-in's switch changed

    // The menu bar's one menu (docs/design.md, section 9; roadmap step 2.12).
    class PluginMenu : public juce::MenuBarModel
    {
    public:
        PluginMenu (te::ExternalPlugin& p, juce::Component& w)
            : plugin (p),
              window (w)
        {
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
            menu.addItem ("Send All Keys to the Plug-in", services != nullptr, getsAllKeys(), whileOpen ([this] { toggleAllKeys(); }));
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
        te::ExternalPlugin& plugin;
        juce::Component& window;
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

        // Presets go to and from the open project's devices only: not the previewer's, which has an Edit of its own.
        bool isInOpenProject() const { return services != nullptr && getEdit (services->context) == &plugin.edit; }

        // Its folder among the user's presets, named after it.
        juce::File getPresetFolder() const
        {
            return services->library.getPresetsFolder().getChildFile (getPresetFolderName (services->context, getDeviceID (plugin)));
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
                else if (item.preset.has_value() && item.preset->kind == PresetKind::device && item.preset->device == plugin.getName())
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

            ui::askWhereToSavePreset (chooser, getPresetFolder(), plugin.getName(),
                                      [this] (const juce::File& file)
                                      {
                                          if (! isInOpenProject())
                                              return;

                                          if (saveDevicePreset (services->context, getDeviceID (plugin), file))
                                              services->library.notePresetSaved(); // the browser shows it
                                          else
                                              showProblem ("Phazite couldn't save the preset as " + file.getFullPathName() + ".");
                                      });
        }

        void loadPreset (const juce::File& file)
        {
            if (isInOpenProject() && ! loadPresetInto (services->context, getDeviceID (plugin), file))
                showProblem ("Phazite couldn't load " + file.getFileNameWithoutExtension() + " into " + plugin.getName()
                             + ": the preset is another device's, or Phazite can't read it.");
        }

        void showProblem (const juce::String& message)
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                              .withIconType (juce::MessageBoxIconType::WarningIcon)
                                              .withTitle (plugin.getName())
                                              .withMessage (message)
                                              .withButton ("OK")
                                              .withAssociatedComponent (&window),
                                          nullptr);
        }

        bool getsAllKeys() const
        {
            return services != nullptr && services->settings.get (Settings::pluginsGettingAllKeys).contains (getIdentity (plugin));
        }

        // For every project, and every window of the plug-in.
        void toggleAllKeys()
        {
            if (services == nullptr)
                return;

            auto plugins = services->settings.get (Settings::pluginsGettingAllKeys);
            const auto identity = getIdentity (plugin);

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
    // state in the project: model/mixer/internal/Mixer.h), on a screen that is there; it stays above the main window while
    // Phazite is in front; and it closes through Tracktion, which then forgets it.
    class DeviceWindowFrame : public juce::DocumentWindow,
                              private juce::Timer
    {
    public:
        DeviceWindowFrame (te::Plugin& p, const juce::String& title)
            : juce::DocumentWindow (title, {}, juce::DocumentWindow::closeButton),
              plugin (p)
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

        void closeButtonPressed() override { plugin.windowState->closeWindowExplicitly(); }

        void lookAndFeelChanged() override // the theme changed
        {
            juce::DocumentWindow::lookAndFeelChanged();
            setBackgroundColour (ui::getPalette (*this).surface);
        }

        void moved() override
        {
            if (! storeBounds)
                return;

            // For this session, and as view state for the next (model/mixer/internal/Mixer.h).
            plugin.windowState->lastWindowBounds = getBounds();
            rememberWindowPosition (plugin, getPosition());
        }

    protected:
        te::Plugin& plugin;

        // Where it was last time, or where Tracktion suggests; then its moves are kept, and it stays above the main window.
        void place()
        {
            setBounds (onScreen (getBounds().withPosition (plugin.windowState->choosePositionForPluginWindow())));
            storeBounds = true;
            timerCallback();
            startTimerHz (2);
        }

        // As it closes: what it does then is no move to keep.
        void stopKeepingMoves() noexcept { storeBounds = false; }

    private:
        bool storeBounds = false;

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
            // A device in a container whose track went (deleted, or the step that made it undone) plays nowhere, and its
            // container's rack stays for the session: its window goes, as Tracktion closes a device's on a deleted track.
            // (This deletes the window, which a timer's callback may do.)
            if (plugin.isInRack() && findContainerOf (plugin) == nullptr)
            {
                plugin.windowState->closeWindowExplicitly();
                return;
            }

            if (const auto inFront = juce::Process::isForegroundProcess(); inFront != isAlwaysOnTop())
                setAlwaysOnTop (inFront);

            refresh();
        }

        // Twice a second, what the window shows of the device that changes (its name).
        virtual void refresh() {}

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceWindowFrame)
    };

    //==============================================================================
    // A plug-in's window: its own editor, under the Plug-in menu.
    class PluginWindow : public DeviceWindowFrame
    {
    public:
        explicit PluginWindow (te::ExternalPlugin& p)
            : DeviceWindowFrame (p, p.getName()),
              externalPlugin (p),
              menu (p, *this)
        {
            setMenuBar (&menu);
            recreateEditor();
            place();
        }

        ~PluginWindow() override
        {
            stopKeepingMoves();
            storeDeviceState (plugin); // the settings changed in the window, into the project, outside the undo history
            setEditor (nullptr);
            setMenuBar (nullptr); // before the menu goes
        }

        bool hasEditor() const noexcept { return editor != nullptr; }

        void recreateEditor()
        {
            setEditor (nullptr);
            setEditor (externalPlugin.createEditor());
        }

        void updateKeys() { menu.updateKeys(); }

    private:
        te::ExternalPlugin& externalPlugin;
        PluginMenu menu;
        std::unique_ptr<te::Plugin::EditorComponent> editor;

        void setEditor (std::unique_ptr<te::Plugin::EditorComponent> newEditor)
        {
            setConstrainer (nullptr);
            clearContentComponent();
            editor = std::move (newEditor);

            if (editor == nullptr)
                return;

            setContentNonOwned (editor.get(), true);
            setResizable (editor->allowWindowResizing(), false);

            if (editor->allowWindowResizing())
                setConstrainer (editor->getBoundsConstrainer());

            // Play and stop, and the computer keyboard, work while the plug-in is being tweaked (ui/input/PluginKeys.h),
            // unless it gets every key.
            if (auto* keys = ui::getPluginKeys())
            {
                keys->watch (*this);
                menu.updateKeys();
            }
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
    };

    // One of Phazite's own devices out of the Devices view, when the user wants it: its editor, in a window named after it
    // and its channel ("EQ - Bass"), as large as its editor at first and then as the user makes it. Its card in the Devices
    // view stays folded meanwhile.
    class DeviceWindow final : public DeviceWindowFrame
    {
    public:
        explicit DeviceWindow (DevicePlugin& device)
            : DeviceWindowFrame (device, getTitleOf (device)),
              content (makeDeviceHandle (device))
        {
            setContentNonOwned (&content, true);
            setResizable (true, false);
            setResizeLimits (content.getMinimumWidth(), std::min (content.getHeight(), content.getMinimumHeight()), 8192, 8192);

            // It is Phazite's own: every shortcut works in it, and the computer keyboard, as in the main window, while the
            // app's keys are there (until the main window goes).
            if (auto* keys = ui::getPluginKeys())
                keys->watchAsOwnWindow (*this);

            place();
        }

        ~DeviceWindow() override
        {
            stopKeepingMoves();
            clearContentComponent();
        }

        bool hasEditor() const noexcept { return content.hasEditor(); }

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

        // "EQ - Bass": the device and its channel, the track of the container it is in, or the master.
        static juce::String getTitleOf (const te::Plugin& device)
        {
            auto* track = device.getOwnerTrack();

            if (track == nullptr)
                track = getTrackOfContainerDevice (device);

            return device.getName() + " - " + (track != nullptr ? track->getName() : juce::String ("Master"));
        }

        void refresh() override // the device or its track renamed
        {
            if (const auto title = getTitleOf (plugin); title != getName())
                setName (title);
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

std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState& state)
{
    auto* windowState = dynamic_cast<te::Plugin::WindowState*> (&state);

    if (windowState == nullptr)
        return {};

    // One of Phazite's own devices, out of the Devices view when the user wants: its editor. Containers show there only.
    if (auto* device = dynamic_cast<DevicePlugin*> (&windowState->plugin))
    {
        auto window = std::make_unique<DeviceWindow> (*device);

        if (! window->hasEditor())
            return {};

        window->setVisible (true);
        window->toFront (false);
        return window;
    }

    auto* plugin = dynamic_cast<te::ExternalPlugin*> (&windowState->plugin);

    if (plugin == nullptr || plugin->getAudioPluginInstance() == nullptr)
        return {}; // not a plug-in, or one that didn't load

    auto window = std::make_unique<PluginWindow> (*plugin);

    if (! window->hasEditor())
        return {};

    window->setVisible (true);
    window->toFront (false);
    return window;
}

void recreatePluginWindowContent (te::Plugin& plugin)
{
    if (plugin.windowState == nullptr)
        return;

    if (auto* window = dynamic_cast<PluginWindow*> (plugin.windowState->pluginWindow.get()))
        window->recreateEditor();
}

void setPluginWindowServices (const PluginWindowServices* newServices)
{
    services = newServices;
}

std::unique_ptr<juce::MenuBarModel> createPluginMenu (te::ExternalPlugin& plugin, juce::Component& window)
{
    return std::make_unique<PluginMenu> (plugin, window);
}
} // namespace phazite
