// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StatusBar.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/TimelineModel.h"
#include "model/mixer/MixerModel.h"
#include "model/transport/TransportModel.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

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

    // "1 clip", "3 clips".
    juce::String count (size_t number, const juce::String& one, const juce::String& many)
    {
        return juce::String ((juce::int64) number) + " " + (number == 1 ? one : many);
    }

    // What is selected, in a few words: a time range by its bars ("Bars 5 to 8"); else notes, automation points, devices,
    // clips, sections or tracks, counted, one track by its name; nothing for nothing.
    juce::String describeSelection (const ProjectContext& context)
    {
        if (! context.hasProject())
            return {};

        if (const auto& range = context.getSongRange(); ! range.isEmpty())
        {
            const auto beats = range.getRange();
            const auto first = getBarAt (context, beats.getStart()) + 1;
            const auto last = std::max (first, getBarAt (context, beats.getEnd() - BeatDuration::fromBeats (1.0e-6)) + 1);
            return first == last ? "Bar " + juce::String (first) : "Bars " + juce::String (first) + " to " + juce::String (last);
        }

        const auto& selection = context.getSongSelection();

        if (const auto notes = selection.getNotes(); ! notes.empty())
            return count (notes.size(), "note", "notes");

        if (const auto points = selection.getSelectedAutomationPoints(); ! points.indices.empty())
            return count (points.indices.size(), "point", "points");

        if (const auto devices = selection.getDevices(); ! devices.empty())
            return count (devices.size(), "device", "devices");

        if (const auto clips = selection.getClips(); ! clips.empty())
            return count (clips.size(), "clip", "clips");

        if (const auto sections = selection.getSections(); ! sections.empty())
            return count (sections.size(), "section", "sections");

        if (const auto tracks = selection.getTracks(); ! tracks.empty())
        {
            if (tracks.size() == 1)
                if (const auto channel = getChannel (context, tracks.front()))
                    return channel->name;

            return count (tracks.size(), "track", "tracks");
        }

        return {};
    }

    // "12 ms", to a millisecond, or a tenth under ten.
    juce::String formatMilliseconds (double seconds)
    {
        const auto milliseconds = seconds * 1000.0;
        return (milliseconds < 10.0 ? juce::String (milliseconds, 1) : juce::String (juce::roundToInt (milliseconds))) + " ms";
    }

    const juce::String separator = juce::String::fromUTF8 (" \xc2\xb7 "); // " · "
    const juce::String arrow = juce::String::fromUTF8 (" \xe2\x86\x92 "); // " → "
} // namespace

//==============================================================================
// One of the bar's items: a short text, lit under the pointer where a click on it does something, and for something that
// takes a while, a bar of how far it has come after the text. What it says in full is its tooltip, which the hint bar
// shows too.
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

    // How far something has come, from 0 to 1, as a bar after the text; or no bar.
    void showProgress (std::optional<double> shown)
    {
        if (shown.has_value() == progress.has_value() && (! shown.has_value() || juce::exactlyEqual (*shown, *progress)))
            return;

        progress = shown;
        repaint();
    }

    juce::Font getFont() const { return LookAndFeel::getNumberFont (TextSize::small); }

    int getWidthFor (const juce::String& shown) const
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (getFont(), shown)) + 2 * Space::s
               + (progress.has_value() ? Shell::statusProgressWidth + Space::s : 0);
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

        auto area = getLocalBounds().reduced (Space::s, 0);

        if (progress.has_value())
        {
            const auto bar = area.removeFromRight (Shell::statusProgressWidth)
                                 .toFloat()
                                 .withSizeKeepingCentre ((float) Shell::statusProgressWidth, (float) Shell::statusProgressHeight);
            area.removeFromRight (Space::s);
            const auto corner = bar.getHeight() / 2.0f;
            g.setColour (p.background);
            g.fillRoundedRectangle (bar, corner);
            g.setColour (p.accent);
            g.fillRoundedRectangle (bar.withWidth (std::max (bar.getHeight(), bar.getWidth() * (float) *progress)), corner);
        }

        g.setColour (emphasis == Emphasis::warning ? p.warning : emphasis == Emphasis::normal ? p.text : p.textMuted);
        g.setFont (getFont());
        g.drawText (getButtonText(), area, juce::Justification::centred, false);
    }

    Emphasis emphasis = Emphasis::quiet;
    std::optional<double> progress;
};

//==============================================================================
StatusBar::StatusBar (EngineContext& e, CommandBridge& bridge, ProjectContext& c)
    : engine (e),
      commands (bridge),
      context (c),
      saveItem (std::make_unique<Item> ("Saved")),
      selectionItem (std::make_unique<Item> ("Selection")),
      midiItem (std::make_unique<Item> ("MIDI")),
      renderItem (std::make_unique<Item> ("Stretching")),
      delayItem (std::make_unique<Item> ("Delay compensation")),
      audioItem (std::make_unique<Item> ("Audio output")),
      cpuItem (std::make_unique<Item> ("CPU"))
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Status bar");
    saveItem->onClick = [this] { commands.invoke ("project.save"); };
    midiItem->onClick = [this] { commands.invoke ("options.midiSettings"); };
    delayItem->onClick = [this] { commands.invoke ("options.audioSettings"); };
    audioItem->onClick = [this] { commands.invoke ("options.audioSettings"); };
    selectionItem->setTooltip ("What is selected in the song: what Delete, Duplicate, the arrow keys and the other edits act on");
    renderItem->setTooltip ("Audio clips that follow the tempo, or are pitched or reversed, are being made again in the background "
                            "after a change: each one is silent until it is ready");
    cpuItem->setTooltip ("How much of the time available for each audio block the engine uses. Near 100%, the sound breaks up: "
                         "freeze tracks, or choose a larger buffer in the audio settings");

    addAndMakeVisible (hintBar);

    for (auto* item : { saveItem.get(), selectionItem.get(), midiItem.get(), renderItem.get(), delayItem.get() })
        addChildComponent (*item);

    addAndMakeVisible (*audioItem);
    addAndMakeVisible (*cpuItem);

    context.addListener (this);
    context.getSongSelection().addChangeListener (this);
    context.getSongRange().addChangeListener (this);

    showAudioOutput (engine.getAudioOutput());
    showSelection();
    timerCallback();
    startTimerHz (4);
}

StatusBar::~StatusBar()
{
    context.getSongRange().removeChangeListener (this);
    context.getSongSelection().removeChangeListener (this);
    context.removeListener (this);
}

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

void StatusBar::showSelection()
{
    const auto text = describeSelection (context);
    const auto wasVisible = selectionItem->isVisible();
    selectionItem->setVisible (text.isNotEmpty());
    const auto oldWidth = selectionItem->getPreferredWidth();
    selectionItem->show (text, Item::Emphasis::quiet);

    if (wasVisible != selectionItem->isVisible() || oldWidth != selectionItem->getPreferredWidth())
        resized();
}

// "MIDI → Bass": the track what is played on a MIDI keyboard or the computer keyboard goes to, and records onto.
void StatusBar::showMidi()
{
    const auto track = context.hasProject() ? getLiveMidiTrack (context) : TrackID();
    const auto channel = track.isValid() ? getChannel (context, track) : std::nullopt;
    const auto wasVisible = midiItem->isVisible();
    const auto oldWidth = midiItem->getPreferredWidth();
    midiItem->setVisible (context.hasProject());

    if (channel.has_value())
    {
        midiItem->show ("MIDI" + arrow + channel->name, Item::Emphasis::quiet);
        midiItem->setTooltip ("What you play on a MIDI keyboard or the computer keyboard goes to " + channel->name
                              + ", and records onto it. Select another track with an instrument to play that one. Click for the MIDI "
                                "settings");
    }
    else
    {
        midiItem->show ("MIDI" + arrow + "no track", Item::Emphasis::quiet);
        midiItem->setTooltip ("No track has an instrument, so what you play on a MIDI keyboard or the computer keyboard isn't heard: "
                              "give a track an instrument. Click for the MIDI settings");
    }

    if (wasVisible != midiItem->isVisible() || oldWidth != midiItem->getPreferredWidth())
        resized();
}

// "Stretching 2 clips" and a bar, while clips are being rendered in the background (model/arrangement/AudioClipModel.h).
void StatusBar::showRenders()
{
    const auto progress = getClipRenderProgress (context);
    const auto wasVisible = renderItem->isVisible();
    const auto oldWidth = renderItem->getPreferredWidth();
    renderItem->setVisible (progress.renders > 0);

    if (progress.renders > 0)
    {
        const auto percent = juce::roundToInt (progress.done * 100.0);
        renderItem->show ("Stretching " + count ((size_t) progress.renders, "clip", "clips"), Item::Emphasis::normal);
        renderItem->showProgress (percent / 100.0);
        renderItem->setTitle (renderItem->getButtonText() + ", " + juce::String (percent) + "%");
    }
    else
    {
        renderItem->showProgress (std::nullopt);
    }

    if (wasVisible != renderItem->isVisible() || oldWidth != renderItem->getPreferredWidth())
        resized();
}

// "Delay comp. 12 ms": how late the song plays so that every track lines up with the devices that look ahead, when it
// does (MixerModel.h); what is played live comes out as late.
void StatusBar::showDelay()
{
    const auto compensation = context.hasProject() ? getDelayCompensation (context) : DelayCompensation();
    const auto shown = compensation.seconds >= 0.0005;
    const auto wasVisible = delayItem->isVisible();
    const auto oldWidth = delayItem->getPreferredWidth();
    delayItem->setVisible (shown);

    if (shown)
    {
        const auto delay = formatMilliseconds (compensation.seconds);
        juce::StringArray devices;

        for (const auto& [name, seconds] : compensation.devices)
            devices.add (name + " (" + formatMilliseconds (seconds) + ")");

        delayItem->show ("Delay comp. " + delay, Item::Emphasis::quiet);
        delayItem->setTooltip ("The song plays " + delay + " late, so that every track lines up with the devices that look ahead"
                               + (devices.isEmpty() ? juce::String() : ": " + devices.joinIntoString (", "))
                               + ". What you play live comes out as late: turn on Low Latency in the transport bar while you play. "
                                 "Click for the audio settings");
    }

    if (wasVisible != delayItem->isVisible() || oldWidth != delayItem->getPreferredWidth())
        resized();
}

juce::String StatusBar::getSaveText() const
{
    return saveItem->isVisible() ? saveItem->getButtonText() : juce::String();
}

juce::String StatusBar::getSelectionText() const
{
    return selectionItem->isVisible() ? selectionItem->getButtonText() : juce::String();
}

juce::String StatusBar::getMidiText() const
{
    return midiItem->isVisible() ? midiItem->getButtonText() : juce::String();
}

juce::String StatusBar::getRenderText() const
{
    return renderItem->isVisible() ? renderItem->getButtonText() : juce::String();
}

double StatusBar::getRenderProgress() const
{
    return renderItem->progress.value_or (0.0);
}

juce::String StatusBar::getDelayText() const
{
    return delayItem->isVisible() ? delayItem->getButtonText() : juce::String();
}

juce::String StatusBar::getAudioText() const
{
    return audioItem->getButtonText();
}

juce::String StatusBar::getCpuText() const
{
    return cpuItem->getButtonText();
}

void StatusBar::projectChanged()
{
    showSelection();
    timerCallback();
}

void StatusBar::songChanged (const SongChanges& changes)
{
    if (changes.tracks)
        showSelection(); // a selected track renamed

    if (changes.tracks || changes.devices)
    {
        showMidi();
        showDelay();
    }
}

void StatusBar::changeListenerCallback (juce::ChangeBroadcaster*)
{
    showSelection();
    showMidi(); // live MIDI follows the selected track
}

void StatusBar::timerCallback()
{
    showRenders();
    showMidi();
    showDelay();

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

    // The others as they fit, leaving the hints room: what is selected goes first, then where MIDI goes, the delay
    // compensation and the renders; whether the project is saved stays.
    Item* const optional[] = { renderItem.get(), delayItem.get(), midiItem.get(), selectionItem.get() }; // most needed first
    auto room = area.getWidth() - Shell::statusHintsMinimumWidth - (saveItem->isVisible() ? saveItem->getPreferredWidth() : 0);
    std::vector<Item*> fitting;

    for (auto* item : optional)
    {
        if (! item->isVisible())
            continue;

        if (item->getPreferredWidth() <= room)
        {
            room -= item->getPreferredWidth();
            fitting.push_back (item);
        }
    }

    const auto fits = [&fitting] (Item* item) { return std::find (fitting.begin(), fitting.end(), item) != fitting.end(); };

    for (auto* item : { delayItem.get(), renderItem.get() })
        item->setBounds (fits (item) ? area.removeFromRight (item->getPreferredWidth()) : juce::Rectangle<int>());

    for (auto* item : { saveItem.get(), selectionItem.get(), midiItem.get() })
        if (item == saveItem.get() ? item->isVisible() : fits (item))
            item->setBounds (area.removeFromLeft (item->getPreferredWidth()));
        else
            item->setBounds ({});

    hintBar.setBounds (area);
}
} // namespace phazite::ui
