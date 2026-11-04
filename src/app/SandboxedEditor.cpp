// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SandboxedEditor.h"
#include "model/plugins/internal/SandboxedInstance.h"
#include "ui/input/PluginKeys.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite
{
namespace
{
    constexpr int openMilliseconds = 10000; // how long a plug-in may take to make its editor
    constexpr int closeMilliseconds = 2000; // and to take it out of Phazite's window, before that goes
    constexpr int keyChecksPerSecond = 4;   // how often it looks whether the keys Phazite takes changed
} // namespace

SandboxedEditor::SandboxedEditor (std::shared_ptr<SandboxedInstance> sandboxed, int width, int height, bool canResize)
    : juce::ComponentMovementWatcher (this),
      instance (std::move (sandboxed)),
      resizable (canResize)
{
    setOpaque (true);
    setSize (width, height);

    // As the plug-in resizes its own, and a key Phazite takes, pressed over it.
    instance->onEditorResized = [this] (int newWidth, int newHeight) { setSize (newWidth, newHeight); };
    instance->onEditorKey = [this] (bool isDown, int keyCode, int modifiers) { runKey (isDown, keyCode, modifiers); };
    startTimerHz (keyChecksPerSecond);
}

SandboxedEditor::~SandboxedEditor()
{
    instance->onEditorResized = nullptr;
    instance->onEditorKey = nullptr;

    if (! instance->hasCrashed())
        instance->getRemote().closeEditor (closeMilliseconds);
}

// Under the plug-in's window, before it shows.
void SandboxedEditor::paint (juce::Graphics& g)
{
    g.fillAll (ui::getPalette (*this).surface);
}

// Where it is in its window, in the window's pixels as the screen counts them; nowhere while it doesn't show.
void SandboxedEditor::place()
{
    auto* peer = getPeer();
    const auto showing = peer != nullptr && isShowing() && ! instance->hasCrashed();
    const auto window = showing ? (std::int64_t) (juce::pointer_sized_int) peer->getNativeHandle() : 0;
    const auto area = showing ? (peer->getAreaCoveredBy (*this).toFloat() * peer->getPlatformScaleFactor()).getSmallestIntegerContainer()
                              : juce::Rectangle<int>();

    if (window == placedWindow && area == placedArea)
        return;

    placedWindow = window;
    placedArea = area;
    instance->getRemote().placeEditor (window, area.getX(), area.getY(), area.getWidth(), area.getHeight());
    timerCallback(); // the keys, now that its window is known
}

// The keys Phazite takes over its window, told again as they change.
void SandboxedEditor::timerCallback()
{
    auto* keys = ui::getPluginKeys();
    const auto everyKey = keys != nullptr && keys->sendsAllKeys (*getTopLevelComponent());
    const auto wanted = keys != nullptr ? keys->getWantedKeys() : std::vector<juce::KeyPress>();

    if (toldKeys && everyKey == toldEveryKey && wanted == toldWanted)
        return;

    std::vector<std::pair<int, int>> codes;

    for (const auto& key : wanted)
        codes.emplace_back (key.getKeyCode(), key.getModifiers().getRawFlags());

    instance->getRemote().setEditorKeys (everyKey, codes);
    toldKeys = true;
    toldEveryKey = everyKey;
    toldWanted = wanted;
}

// One the helper handed back: run as Phazite's own hook runs a key over a plug-in's window.
void SandboxedEditor::runKey (bool isDown, int keyCode, int modifiers)
{
    auto* keys = ui::getPluginKeys();

    if (keys == nullptr)
        return;

    keys->keyStateChanged (isDown, this);

    if (isDown)
        keys->keyPressed (juce::KeyPress (keyCode, juce::ModifierKeys (modifiers), 0), this);
}

std::unique_ptr<SandboxedEditor> createSandboxedEditor (std::shared_ptr<SandboxedInstance> instance)
{
    if (instance == nullptr || instance->hasCrashed() || ! instance->getRemote().getInfo().hasEditor)
        return nullptr;

    if (const auto size = instance->getRemote().openEditor (openMilliseconds))
        return std::unique_ptr<SandboxedEditor> (new SandboxedEditor (std::move (instance), size->width, size->height, size->isResizable));

    return nullptr;
}
} // namespace phazite
