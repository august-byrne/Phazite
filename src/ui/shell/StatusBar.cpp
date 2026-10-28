// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StatusBar.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    // 44100 as "44.1 kHz", 48000 as "48 kHz".
    juce::String formatSampleRate (double rate)
    {
        const auto kilohertz = rate / 1000.0;
        return juce::String (kilohertz, juce::approximatelyEqual (kilohertz, std::round (kilohertz)) ? 0 : 1) + " kHz";
    }

    const juce::String separator = juce::String::fromUTF8 (" \xc2\xb7 "); // " · "
} // namespace

//==============================================================================
// One of the bar's items: a short text, lit under the pointer where a click on it does something. What it says in full
// is its tooltip, which the hint bar shows too.
struct StatusBar::Item : public juce::Button
{
    enum class Emphasis
    {
        quiet,  // as it should be
        normal, // worth a look: changes not saved
        warning // needs the user: no audio output, the engine near its limit
    };

    explicit Item (const juce::String& title)
        : juce::Button (title)
    {
        setTitle (title);
        setMouseClickGrabsKeyboardFocus (false);
    }

    void show (const juce::String& shown, Emphasis shownEmphasis)
    {
        if (shown == getButtonText() && shownEmphasis == emphasis)
            return;

        emphasis = shownEmphasis;
        setButtonText (shown);
        setTitle (shown); // what screen readers say
        repaint();
    }

    juce::Font getFont() const { return LookAndFeel::getNumberFont (TextSize::small); }

    int getWidthFor (const juce::String& shown) const
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (getFont(), shown)) + 2 * Space::s;
    }

    int getPreferredWidth() const { return getWidthFor (getButtonText()); }

    void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override
    {
        const auto& p = getPalette (*this);

        if ((isHighlighted || isDown) && onClick != nullptr)
        {
            g.setColour (isDown ? p.background : p.surface);
            g.fillRect (getLocalBounds().withTrimmedTop (1));
        }

        g.setColour (emphasis == Emphasis::warning ? p.warning : emphasis == Emphasis::normal ? p.text : p.textMuted);
        g.setFont (getFont());
        g.drawText (getButtonText(), getLocalBounds().reduced (Space::s, 0), juce::Justification::centred, false);
    }

    Emphasis emphasis = Emphasis::quiet;
};

//==============================================================================
StatusBar::StatusBar (EngineContext& e, CommandBridge& bridge)
    : engine (e),
      commands (bridge),
      saveItem (std::make_unique<Item> ("Saved")),
      audioItem (std::make_unique<Item> ("Audio output")),
      cpuItem (std::make_unique<Item> ("CPU"))
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Status bar");
    saveItem->onClick = [this] { commands.invoke ("project.save"); };
    audioItem->onClick = [this] { commands.invoke ("options.audioSettings"); };
    cpuItem->setTooltip ("How much of the time available for each audio block the engine uses. Near 100%, the sound breaks up: "
                         "freeze tracks, or choose a larger buffer in the audio settings");

    addAndMakeVisible (hintBar);
    addChildComponent (*saveItem);
    addAndMakeVisible (*audioItem);
    addAndMakeVisible (*cpuItem);

    showAudioOutput (engine.getAudioOutput());
    timerCallback();
    startTimerHz (4);
}

StatusBar::~StatusBar() = default;

void StatusBar::showSaveState (SaveState state)
{
    using Emphasis = Item::Emphasis;
    saveItem->setVisible (state != SaveState::none);

    switch (state)
    {
        case SaveState::saved:
            saveItem->show ("Saved", Emphasis::quiet);
            saveItem->setTooltip ("Every change is saved");
            break;
        case SaveState::unsaved:
            saveItem->show ("Unsaved changes", Emphasis::normal);
            saveItem->setTooltip (withShortcut (commands, "Changes since the project was last saved: click to save them", "project.save"));
            break;
        case SaveState::neverSaved:
            saveItem->show ("Not saved yet", Emphasis::normal);
            saveItem->setTooltip (
                withShortcut (commands, "The project has never been saved: click to choose its name and where it goes", "project.save"));
            break;
        case SaveState::none:
        default:
            break;
    }

    resized();
}

void StatusBar::showAudioOutput (const EngineContext::AudioOutput& output)
{
    using Emphasis = Item::Emphasis;

    if (! output.isOpen)
    {
        audioItem->show ("No audio output", Emphasis::warning);
        audioItem->setTooltip ("Nothing can be heard: click to choose an audio output in the audio settings");
    }
    else
    {
        // "48 kHz · 10 ms": how late what is played comes out of the device, as its driver reports, Bluetooth's own delay
        // on top.
        const auto delay = juce::String (juce::roundToInt (output.latencySeconds * 1000.0)) + " ms";
        auto text = formatSampleRate (output.sampleRate) + separator + delay;

        if (output.isBluetooth)
            text << separator << "Bluetooth";

        audioItem->show (text, output.isBluetooth ? Emphasis::warning : Emphasis::quiet);
        audioItem->setTooltip (
            output.name + " (" + output.type + "), " + juce::String (output.bufferSamples) + " samples a block: what is played comes out "
            + delay + " later"
            + (output.isBluetooth ? ", and Bluetooth adds about 200 ms more. Wired headphones or speakers play in time" : juce::String())
            + ". Click for the audio settings");
    }

    resized();
}

juce::String StatusBar::getSaveText() const
{
    return saveItem->isVisible() ? saveItem->getButtonText() : juce::String();
}

juce::String StatusBar::getAudioText() const
{
    return audioItem->getButtonText();
}

juce::String StatusBar::getCpuText() const
{
    return cpuItem->getButtonText();
}

void StatusBar::timerCallback()
{
    const auto percent = juce::roundToInt (engine.getCpuUsage() * 100.0);

    if (percent == shownCpu)
        return;

    shownCpu = percent;
    cpuItem->show ("CPU " + juce::String (percent) + "%",
                   percent >= Shell::busyCpuPercent ? Item::Emphasis::warning : Item::Emphasis::quiet);
}

void StatusBar::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surfaceRaised);
    g.setColour (p.border);
    g.fillRect (getLocalBounds().removeFromTop (1));
}

void StatusBar::resized()
{
    // The CPU as wide as it is at its widest, so that the others don't move as it changes.
    auto area = getLocalBounds();
    cpuItem->setBounds (area.removeFromRight (cpuItem->getWidthFor ("CPU 100%")));
    audioItem->setBounds (area.removeFromRight (audioItem->getPreferredWidth()));

    if (saveItem->isVisible())
        saveItem->setBounds (area.removeFromLeft (saveItem->getPreferredWidth()));

    hintBar.setBounds (area);
}
} // namespace phazite::ui
