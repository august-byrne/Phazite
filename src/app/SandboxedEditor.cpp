// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SandboxedEditor.h"
#include "model/plugins/internal/PluginHost.h"
#include "ui/input/PluginKeys.h"
#include "ui/theme/LookAndFeel.h"

#include <vector>

namespace phazite
{
namespace
{
    constexpr int openMilliseconds = 10000; // how long a plug-in may take to make its editor
    constexpr int closeMilliseconds = 2000; // and to take it out of Phazite's window, before that goes
    constexpr int keyChecksPerSecond = 4;   // how often it looks whether the keys Phazite takes changed

    class SandboxedEditor final : public juce::AudioProcessorEditor,
                                  private juce::ComponentMovementWatcher,
                                  private juce::Timer
    {
    public:
        SandboxedEditor (SandboxedInstance& sandboxed, const sandbox::RemoteInstance::EditorSize& size)
            : juce::AudioProcessorEditor (sandboxed),
              juce::ComponentMovementWatcher (this),
              instance (sandboxed)
        {
            setOpaque (true);
            setResizable (size.isResizable, false);
            setSize (size.width, size.height);

            // As the plug-in resizes its own, and a key Phazite takes, pressed over it.
            instance.onEditorResized = [this] (int width, int height) { setSize (width, height); };
            instance.onEditorKey = [this] (bool isDown, int keyCode, int modifiers) { runKey (isDown, keyCode, modifiers); };
            startTimerHz (keyChecksPerSecond);
        }

        ~SandboxedEditor() override
        {
            instance.onEditorResized = nullptr;
            instance.onEditorKey = nullptr;

            if (! instance.hasCrashed())
                instance.getRemote().closeEditor (closeMilliseconds);
        }

    private:
        SandboxedInstance& instance;
        std::int64_t placedWindow = 0; // where the helper was told to show it
        juce::Rectangle<int> placedArea;
        bool toldKeys = false, toldEveryKey = false;
        std::vector<juce::KeyPress> toldWanted;

        // Under the plug-in's window, before it shows.
        void paint (juce::Graphics& g) override { g.fillAll (ui::getPalette (*this).surface); }

        // Where it is in its window, in the window's pixels as the screen counts them; nowhere while it doesn't show.
        void place()
        {
            auto* peer = getPeer();
            const auto showing = peer != nullptr && isShowing() && ! instance.hasCrashed();
            const auto window = showing ? (std::int64_t) (juce::pointer_sized_int) peer->getNativeHandle() : 0;
            const auto area =
                showing ? (peer->getAreaCoveredBy (*this).toFloat() * peer->getPlatformScaleFactor()).getSmallestIntegerContainer()
                        : juce::Rectangle<int>();

            if (window == placedWindow && area == placedArea)
                return;

            placedWindow = window;
            placedArea = area;
            instance.getRemote().placeEditor (window, area.getX(), area.getY(), area.getWidth(), area.getHeight());
            timerCallback(); // the keys, now that its window is known
        }

        void componentMovedOrResized (bool, bool) override { place(); }
        void componentPeerChanged() override { place(); }
        void componentVisibilityChanged() override { place(); }
        using juce::ComponentMovementWatcher::componentMovedOrResized;
        using juce::ComponentMovementWatcher::componentVisibilityChanged;

        // The keys Phazite takes over its window, told again as they change.
        void timerCallback() override
        {
            auto* keys = ui::getPluginKeys();
            const auto everyKey = keys != nullptr && keys->sendsAllKeys (*getTopLevelComponent());
            const auto wanted = keys != nullptr ? keys->getWantedKeys() : std::vector<juce::KeyPress>();

            if (toldKeys && everyKey == toldEveryKey && wanted == toldWanted)
                return;

            std::vector<std::pair<int, int>> codes;

            for (const auto& key : wanted)
                codes.emplace_back (key.getKeyCode(), key.getModifiers().getRawFlags());

            instance.getRemote().setEditorKeys (everyKey, codes);
            toldKeys = true;
            toldEveryKey = everyKey;
            toldWanted = wanted;
        }

        // One the helper handed back: run as Phazite's own hook runs a key over a plug-in's window.
        void runKey (bool isDown, int keyCode, int modifiers)
        {
            auto* keys = ui::getPluginKeys();

            if (keys == nullptr)
                return;

            keys->keyStateChanged (isDown, this);

            if (isDown)
                keys->keyPressed (juce::KeyPress (keyCode, juce::ModifierKeys (modifiers), 0), this);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SandboxedEditor)
    };
} // namespace

juce::AudioProcessorEditor* createSandboxedEditor (SandboxedInstance& instance)
{
    if (const auto size = instance.getRemote().openEditor (openMilliseconds))
        return new SandboxedEditor (instance, *size);

    return nullptr;
}

void useSandboxedEditors (PluginHost& host)
{
    host.setEditorMaker (&createSandboxedEditor);
}
} // namespace phazite
