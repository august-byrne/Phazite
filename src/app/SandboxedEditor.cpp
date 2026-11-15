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
    constexpr int largestEditor = 16384;    // the most pixels a side of a plug-in's editor is taken to have

    // A size the helper told, as a side can be.
    int keepSize (int pixels)
    {
        return juce::jlimit (1, largestEditor, pixels);
    }
} // namespace

SandboxedEditor::SandboxedEditor (std::shared_ptr<SandboxedInstance> sandboxed, int width, int height, bool canResize)
    : juce::ComponentMovementWatcher (this),
      instance (std::move (sandboxed)),
      resizable (canResize),
      pluginSize (keepSize (width), keepSize (height))
{
    setOpaque (true);
    fitPlugin();

    // As the plug-in resizes its own, and a key Phazite takes, pressed over it.
    instance->onEditorResized = [this] (int newWidth, int newHeight)
    {
        pluginSize = { keepSize (newWidth), keepSize (newHeight) };
        fitPlugin();
    };

    instance->onEditorKey = [this] (bool isDown, int keyCode, int modifiers) { runKey (isDown, keyCode, modifiers); };
    startTimerHz (keyChecksPerSecond);
}

float SandboxedEditor::getScale() const
{
    // Its units in the screen's (as the platform counts them, before its own scale): the Interface Size (JUCE's global scale,
    // which is its window's) and any transform, as JUCE works it out; the helper's window has none of them. Before it is in a
    // window, as the window it goes into will scale it.
    const auto global = juce::Desktop::getInstance().getGlobalScaleFactor();
    const auto scale = juce::Component::getApproximateScaleFactorForComponent (this) * global;
    return getPeer() != nullptr ? scale : scale * global;
}

void SandboxedEditor::fitPlugin()
{
    // As large as the plug-in's editor, which is in the screen's units (known bug 68: its size in pixels taken for this one's
    // cropped it below 100 % and left margins above): unless this is that size already, so that a size told back from a
    // resize made here doesn't move it by a rounding.
    fittedScale = getScale();
    const auto toFit = [this] (int pixels, int now)
    {
        return juce::roundToInt ((float) now * fittedScale) == pixels ? now : std::max (1, juce::roundToInt ((float) pixels / fittedScale));
    };

    setSize (toFit (pluginSize.x, getWidth()), toFit (pluginSize.y, getHeight()));
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
    // The Interface Size changed, or it went into a window that scales otherwise: as large as the plug-in again, which places
    // it once more.
    if (! juce::approximatelyEqual (getScale(), fittedScale))
        fitPlugin();

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
    if (! juce::approximatelyEqual (getScale(), fittedScale))
        place();

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
