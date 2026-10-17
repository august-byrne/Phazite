// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginWindow.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/ParameterHandle.h"
#include "model/mixer/internal/Containers.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/components/Controls.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/input/PluginKeys.h"
#include "ui/mixer/ContainerEditor.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite
{
namespace
{
    constexpr int knobSize = 64, labelHeight = 18, columns = 4;

    // A knob for each parameter, for Tracktion's built-in devices, which have no editor of their own.
    class GenericEditor : public te::Plugin::EditorComponent,
                          private juce::Timer
    {
    public:
        explicit GenericEditor (te::Plugin& plugin)
        {
            for (auto* parameter : plugin.getAutomatableParameters())
            {
                auto* knob = knobs.add (new ui::Knob (parameter->getParameterName(), parameter->getDefaultValue().value_or (0.0f)));
                auto* attachment = attachments.add (new ui::ParameterAttachment (*knob));
                attachment->setParameter (makeParameterHandle (&plugin, parameter->paramID));
                attachment->showDeviceText();

                auto* label = labels.add (new juce::Label ({}, parameter->getParameterName()));
                label->setJustificationType (juce::Justification::centred);
                label->setFont (juce::FontOptions (ui::TextSize::small));

                addAndMakeVisible (knob);
                addAndMakeVisible (label);
            }

            const auto rows = std::max (1, (knobs.size() + columns - 1) / columns);
            setSize (columns * (knobSize + ui::Space::l) + ui::Space::l, rows * (knobSize + labelHeight + ui::Space::s) + ui::Space::l);
            startTimerHz (10);
        }

        bool allowWindowResizing() override { return false; }
        juce::ComponentBoundsConstrainer* getBoundsConstrainer() override { return nullptr; }

        void paint (juce::Graphics& g) override { g.fillAll (ui::getPalette (*this).surface); }

        void resized() override
        {
            for (int i = 0; i < knobs.size(); ++i)
            {
                const auto x = ui::Space::l + (i % columns) * (knobSize + ui::Space::l);
                const auto y = ui::Space::l + (i / columns) * (knobSize + labelHeight + ui::Space::s);
                knobs[i]->setBounds (x, y, knobSize, knobSize);
                labels[i]->setBounds (x - ui::Space::s, y + knobSize, knobSize + 2 * ui::Space::s, labelHeight);
            }
        }

    private:
        juce::OwnedArray<ui::Knob> knobs;
        juce::OwnedArray<ui::ParameterAttachment> attachments;
        juce::OwnedArray<juce::Label> labels;

        void timerCallback() override // shows changes from automation, undo and the device's own knobs
        {
            for (auto* attachment : attachments)
                attachment->update();
        }
    };

    //==============================================================================
    // One of Phazite's devices' editors (ui/devices/DeviceEditors.h), or a container's (ui/mixer/ContainerEditor.h), as
    // Tracktion's windows take editors: as big as the editor makes itself, as a container's grows with its devices.
    class DeviceEditor : public te::Plugin::EditorComponent,
                         private juce::ComponentListener
    {
    public:
        explicit DeviceEditor (std::unique_ptr<juce::Component> editor)
            : content (std::move (editor))
        {
            addAndMakeVisible (*content);
            setSize (content->getWidth(), content->getHeight());
            content->addComponentListener (this);
        }

        ~DeviceEditor() override { content->removeComponentListener (this); }

        bool allowWindowResizing() override { return false; }
        juce::ComponentBoundsConstrainer* getBoundsConstrainer() override { return nullptr; }
        void resized() override { content->setBounds (getLocalBounds()); }

    private:
        std::unique_ptr<juce::Component> content;

        void componentMovedOrResized (juce::Component&, bool, bool wasResized) override
        {
            if (wasResized)
                setSize (content->getWidth(), content->getHeight());
        }
    };

    //==============================================================================
    class PluginWindow : public juce::DocumentWindow,
                         private juce::Timer
    {
    public:
        PluginWindow (te::Plugin& p, const PluginWindowServices& s)
            : juce::DocumentWindow (p.getName(), {}, juce::DocumentWindow::closeButton),
              plugin (p),
              services (s)
        {
            setBackgroundColour (ui::getPalette (*this).surface);
            setUsingNativeTitleBar (true);
            getConstrainer()->setMinimumOnscreenAmounts (0x10000, 50, 30, 50);
            recreateEditor();

            // Where it was last time, or where Tracktion suggests.
            setTopLeftPosition (plugin.windowState->choosePositionForPluginWindow());
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
        }

        bool hasEditor() const noexcept { return editor != nullptr; }

        void recreateEditor()
        {
            setEditor (nullptr);
            auto newEditor = plugin.createEditor();

            if (newEditor == nullptr)
            {
                if (auto deviceEditor = ui::createDeviceEditor (makeDeviceHandle (plugin))) // one of Phazite's own devices
                    newEditor = std::make_unique<DeviceEditor> (std::move (deviceEditor));
                else if (isContainer (plugin))
                    newEditor = createContainerEditor();
                else if (dynamic_cast<te::ExternalPlugin*> (&plugin) == nullptr)
                    newEditor = std::make_unique<GenericEditor> (plugin);
            }

            setEditor (std::move (newEditor));
        }

        void closeButtonPressed() override { plugin.windowState->closeWindowExplicitly(); }

        void lookAndFeelChanged() override // the theme changed
        {
            juce::DocumentWindow::lookAndFeelChanged();
            setBackgroundColour (ui::getPalette (*this).surface);
        }

        void moved() override
        {
            if (storeBounds)
                plugin.windowState->lastWindowBounds = getBounds();
        }

    private:
        te::Plugin& plugin;
        const PluginWindowServices services;
        std::unique_ptr<te::Plugin::EditorComponent> editor;
        bool storeBounds = false;

        void timerCallback() override
        {
            if (const auto inFront = juce::Process::isForegroundProcess(); inFront != isAlwaysOnTop())
                setAlwaysOnTop (inFront);
        }

        // A container's devices and macros, in the open project; nothing before the app has given what it needs.
        std::unique_ptr<te::Plugin::EditorComponent> createContainerEditor() const
        {
            if (services.context == nullptr || services.commands == nullptr || getEdit (*services.context) != &plugin.edit)
                return {};

            return std::make_unique<DeviceEditor> (
                std::make_unique<ui::ContainerEditor> (*services.context, *services.commands, getDeviceID (plugin)));
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

            // Play and stop, and the computer keyboard, work while the plug-in is being tweaked (ui/input/PluginKeys.h).
            if (auto* keys = ui::getPluginKeys())
                keys->watch (*this);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
    };
} // namespace

std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState& state, const PluginWindowServices& services)
{
    auto* windowState = dynamic_cast<te::Plugin::WindowState*> (&state);

    if (windowState == nullptr)
        return {};

    auto& plugin = windowState->plugin;

    if (auto* external = dynamic_cast<te::ExternalPlugin*> (&plugin); external != nullptr && external->getAudioPluginInstance() == nullptr)
        return {}; // it didn't load

    auto window = std::make_unique<PluginWindow> (plugin, services);

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
} // namespace phazite
