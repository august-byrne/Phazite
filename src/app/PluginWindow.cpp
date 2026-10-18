// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginWindow.h"
#include "model/mixer/internal/Mixer.h"
#include "model/presets/PresetModel.h"
#include "model/project/internal/ProjectContext.h"
#include "services/Library.h"
#include "services/Settings.h"
#include "ui/browser/Browser.h"
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
    class PluginWindow : public juce::DocumentWindow,
                         private juce::Timer
    {
    public:
        explicit PluginWindow (te::ExternalPlugin& p)
            : juce::DocumentWindow (p.getName(), {}, juce::DocumentWindow::closeButton),
              plugin (p),
              menu (p, *this)
        {
            setBackgroundColour (ui::getPalette (*this).surface);
            setUsingNativeTitleBar (true);
            getConstrainer()->setMinimumOnscreenAmounts (0x10000, 50, 30, 50);
            setMenuBar (&menu);
            recreateEditor();

            // Where it was last time, or where Tracktion suggests.
            setBounds (onScreen (getBounds().withPosition (plugin.windowState->choosePositionForPluginWindow())));
            storeBounds = true;

            // Above the main window, as the design asks, but not above other apps while Phazite is in the background.
            timerCallback();
            startTimerHz (2);
        }

        ~PluginWindow() override
        {
            if (auto* keys = ui::getPluginKeys())
                keys->unwatch (*this);

            storeBounds = false;
            plugin.edit.flushPluginStateIfNeeded (plugin);
            setEditor (nullptr);
            setMenuBar (nullptr); // before the menu goes
        }

        bool hasEditor() const noexcept { return editor != nullptr; }

        void recreateEditor()
        {
            setEditor (nullptr);
            setEditor (plugin.createEditor());
        }

        void updateKeys() { menu.updateKeys(); }

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

    private:
        te::ExternalPlugin& plugin;
        PluginMenu menu;
        std::unique_ptr<te::Plugin::EditorComponent> editor;
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
            if (const auto inFront = juce::Process::isForegroundProcess(); inFront != isAlwaysOnTop())
                setAlwaysOnTop (inFront);
        }

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

    // Phazite's own devices and containers show in the Devices view instead.
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
