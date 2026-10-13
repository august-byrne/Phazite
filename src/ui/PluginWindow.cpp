// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginWindow.h"
#include "Controls.h"
#include "LookAndFeel.h"
#include "PluginKeys.h"

namespace phazite::ui
{
namespace
{
    constexpr int knobSize = 64, labelHeight = 18, columns = 4;

    // A knob for each parameter, for Tracktion's built-in devices, which have no editor of their own.
    class GenericEditor : public te::Plugin::EditorComponent,
                          private juce::Timer
    {
    public:
        explicit GenericEditor (te::Plugin& p)
            : plugin (p)
        {
            for (auto* parameter : plugin.getAutomatableParameters())
            {
                auto* knob = knobs.add (new Knob (parameter->getParameterName(), parameter->getDefaultValue().value_or (0.0f)));
                const te::AutomatableParameter::Ptr shown (parameter);
                knob->textFromValueFunction = [shown] (double value) { return shown->valueToString ((float) value); };

                auto* attachment = attachments.add (new ParameterAttachment (*knob, "Change " + parameter->getParameterName()));
                attachment->setParameter (&plugin, parameter->paramID);

                auto* label = labels.add (new juce::Label ({}, parameter->getParameterName()));
                label->setJustificationType (juce::Justification::centred);
                label->setFont (juce::FontOptions (TextSize::small));

                addAndMakeVisible (knob);
                addAndMakeVisible (label);
            }

            const auto rows = std::max (1, (knobs.size() + columns - 1) / columns);
            setSize (columns * (knobSize + Space::l) + Space::l, rows * (knobSize + labelHeight + Space::s) + Space::l);
            startTimerHz (10);
        }

        bool allowWindowResizing() override { return false; }
        juce::ComponentBoundsConstrainer* getBoundsConstrainer() override { return nullptr; }

        void paint (juce::Graphics& g) override { g.fillAll (getPalette (*this).surface); }

        void resized() override
        {
            for (int i = 0; i < knobs.size(); ++i)
            {
                const auto x = Space::l + (i % columns) * (knobSize + Space::l);
                const auto y = Space::l + (i / columns) * (knobSize + labelHeight + Space::s);
                knobs[i]->setBounds (x, y, knobSize, knobSize);
                labels[i]->setBounds (x - Space::s, y + knobSize, knobSize + 2 * Space::s, labelHeight);
            }
        }

    private:
        te::Plugin& plugin;
        juce::OwnedArray<Knob> knobs;
        juce::OwnedArray<ParameterAttachment> attachments;
        juce::OwnedArray<juce::Label> labels;

        void timerCallback() override // shows changes from automation, undo and the device's own knobs
        {
            for (auto* attachment : attachments)
                attachment->update();
        }
    };

    //==============================================================================
    class PluginWindow : public juce::DocumentWindow,
                         private juce::Timer
    {
    public:
        explicit PluginWindow (te::Plugin& p)
            : juce::DocumentWindow (p.getName(), {}, juce::DocumentWindow::closeButton),
              plugin (p)
        {
            setBackgroundColour (getPalette (*this).surface);
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
            if (auto* keys = getPluginKeys())
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

            if (newEditor == nullptr && dynamic_cast<te::ExternalPlugin*> (&plugin) == nullptr)
                newEditor = std::make_unique<GenericEditor> (plugin);

            setEditor (std::move (newEditor));
        }

        void closeButtonPressed() override { plugin.windowState->closeWindowExplicitly(); }

        void lookAndFeelChanged() override // the theme changed
        {
            juce::DocumentWindow::lookAndFeelChanged();
            setBackgroundColour (getPalette (*this).surface);
        }

        void moved() override
        {
            if (storeBounds)
                plugin.windowState->lastWindowBounds = getBounds();
        }

    private:
        te::Plugin& plugin;
        std::unique_ptr<te::Plugin::EditorComponent> editor;
        bool storeBounds = false;

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

            // Play and stop, and the computer keyboard, work while the plug-in is being tweaked (PluginKeys.h).
            if (auto* keys = getPluginKeys())
                keys->watch (*this);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
    };
} // namespace

std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState& state)
{
    auto* windowState = dynamic_cast<te::Plugin::WindowState*> (&state);

    if (windowState == nullptr)
        return {};

    auto& plugin = windowState->plugin;

    if (auto* external = dynamic_cast<te::ExternalPlugin*> (&plugin); external != nullptr && external->getAudioPluginInstance() == nullptr)
        return {}; // it didn't load

    auto window = std::make_unique<PluginWindow> (plugin);

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
} // namespace phazite::ui
