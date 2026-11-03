// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SoundView.h"
#include "ui/components/ValueText.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    constexpr int detailsHeight = 18;

    const juce::String separator { juce::CharPointer_UTF8 (" \xc2\xb7 ") };

    // A second or less to the hundredth, as a one-shot's length matters to it; minutes and seconds past a minute.
    juce::String describeSeconds (double seconds)
    {
        return seconds < 60.0 ? juce::String (seconds, seconds < 10.0 ? 2 : 1) + " s" : formatDuration (seconds);
    }

    juce::String describeFormat (double sampleRate, int bits, int channels)
    {
        const auto layout = channels == 1   ? juce::String ("mono")
                            : channels == 2 ? juce::String ("stereo")
                                            : juce::String (channels) + " channels";
        return formatSampleRate (sampleRate) + ", " + (bits > 0 ? juce::String (bits) + "-bit " : juce::String()) + layout;
    }

    // A loop's tempo from its tags ("120 BPM"), as the library found it in its name.
    std::optional<double> getTaggedTempo (const LibraryItem& item)
    {
        for (const auto& tag : item.tags)
            if (tag.endsWith (" BPM"))
                return tag.upToFirstOccurrenceOf (" ", false, false).getDoubleValue();

        return {};
    }

    // How long a loop lasts at its tempo: whole bars of four beats, or else beats.
    juce::String describeBeats (double beats)
    {
        const auto bars = beats / 4.0;

        if (std::abs (bars - std::round (bars)) < 0.05 && bars >= 0.95)
        {
            const auto whole = (int) std::lround (bars);
            return juce::String (whole) + (whole == 1 ? " bar" : " bars");
        }

        return juce::String (beats, 1) + " beats";
    }
} // namespace

SoundView::SoundView()
{
    setTitle ("Sound");
    setDescription ("The selected sound's waveform, and what it is: a loop or a one-shot, how long it lasts and its format");
    formats.registerBasicFormats();
    thumbnail.addChangeListener (this);
}

SoundView::~SoundView()
{
    thumbnail.removeChangeListener (this);
}

void SoundView::show (const std::optional<LibraryItem>& item)
{
    const auto file = item.has_value() && item->kind == LibraryItem::Kind::sound ? item->file : juce::File();

    if (file == shown)
        return;

    shown = file;
    details = {};
    thumbnail.setSource (file.existsAsFile() ? new juce::FileInputSource (file) : nullptr);

    if (const std::unique_ptr<juce::AudioFormatReader> reader (file.existsAsFile() ? formats.createReaderFor (file) : nullptr);
        reader != nullptr && reader->sampleRate > 0.0)
    {
        const auto seconds = (double) reader->lengthInSamples / reader->sampleRate;
        juce::StringArray parts;

        if (item->tags.contains ("Loop"))
        {
            parts.add ("Loop");

            if (const auto tempo = getTaggedTempo (*item); tempo.has_value() && *tempo > 0.0)
            {
                parts.add (juce::String (juce::roundToInt (*tempo)) + " BPM");
                parts.add (describeBeats (seconds * *tempo / 60.0));
            }
        }
        else
        {
            parts.add ("One-shot");
        }

        parts.add (describeSeconds (seconds));
        parts.add (describeFormat (reader->sampleRate, (int) reader->bitsPerSample, (int) reader->numChannels));
        details = parts.joinIntoString (separator);
    }

    setDescription (details.isNotEmpty() ? details : juce::String ("No sound selected"));
    repaint();
}

void SoundView::paint (juce::Graphics& g)
{
    if (! showsSound())
        return;

    const auto& p = getPalette (*this);
    auto area = getLocalBounds();
    const auto text = area.removeFromBottom (detailsHeight);
    area.removeFromBottom (Space::xs);

    g.setColour (p.surfaceRaised);
    g.fillRoundedRectangle (area.toFloat(), cornerRadius);

    if (thumbnail.getTotalLength() > 0.0)
    {
        g.setColour (p.accent);
        thumbnail.drawChannels (g, area.reduced (Space::xs), 0.0, thumbnail.getTotalLength(), 1.0f);
    }

    g.setColour (p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText (details, text, juce::Justification::centredLeft, true);
}
} // namespace phazite::ui
