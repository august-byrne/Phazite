// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SweepPicture.h"
#include "DeviceTint.h"
#include "devices/dsp/NoteValues.h"
#include "devices/dsp/SyncedLfo.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    using Lfo = devices::dsp::SyncedLfo;

    constexpr float lfoDotRadius = 4.0f;
} // namespace

SweepPicture::SweepPicture (const DeviceHandle& d, const devices::DeviceInfo& deviceInfo, int strip)
    : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
      device (d),
      info (deviceInfo),
      stripHeight (strip),
      shapeIndex (devices::findParameter (deviceInfo, "shape")),
      stereoIndex (devices::findParameter (deviceInfo, "stereo")),
      syncIndex (devices::findParameter (deviceInfo, "sync")),
      divisionIndex (devices::findParameter (deviceInfo, "division")),
      rateIndex (devices::findParameter (deviceInfo, "rate"))
{
    jassert (shapeIndex >= 0 && stereoIndex >= 0 && syncIndex >= 0 && divisionIndex >= 0 && rateIndex >= 0);

    for (const auto& parameter : info.parameters)
    {
        parameters.push_back (device.getParameter (parameter.id));
        values.push_back (parameter.defaultValue);
    }

    readValues();
}

SweepPicture::~SweepPicture()
{
    if (auto* scope = device.getScope())
        scope->setListening (false);
}

juce::Rectangle<float> SweepPicture::getResponseArea() const
{
    auto area = getLocalBounds().toFloat().reduced (1.0f);
    area.removeFromBottom ((float) (lfoHeight + Space::xs));

    if (stripHeight > 0)
        area.removeFromBottom ((float) (stripHeight + Space::xs));

    return area;
}

juce::Rectangle<float> SweepPicture::getStripArea() const
{
    if (stripHeight <= 0)
        return {};

    auto area = getLocalBounds().toFloat().reduced (1.0f);
    area.removeFromBottom ((float) (lfoHeight + Space::xs));
    return area.removeFromBottom ((float) stripHeight);
}

juce::Rectangle<float> SweepPicture::getLfoArea() const
{
    return getLocalBounds().toFloat().reduced (1.0f).removeFromBottom ((float) lfoHeight);
}

float SweepPicture::getLfoValue (int channel) const
{
    return getSweepValue (getVoicePhase (channel, 0));
}

double SweepPicture::getVoicePhase (int channel, int) const
{
    return (double) phase + (channel == 1 ? 0.5 * (double) values[(size_t) stereoIndex] : 0.0);
}

float SweepPicture::getSweepValue (double at) const
{
    return Lfo::getValue (values[(size_t) shapeIndex] > 0.5f ? Lfo::Shape::triangle : Lfo::Shape::sine, at);
}

double SweepPicture::getSampleRate() const
{
    auto* scope = device.getScope();
    return scope != nullptr ? scope->getSampleRate() : 48000.0;
}

void SweepPicture::readValues()
{
    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].isValid())
            values[i] = parameters[i].getValue();
}

int SweepPicture::indexOf (std::string_view parameterID) const
{
    return devices::findParameter (info, parameterID);
}

void SweepPicture::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto tint = getDeviceTint (*this);
    const auto area = getResponseArea();
    const auto frequencies = getFrequencies();
    graph::paintBackground (g, p, area);
    graph::paintFrequencyGrid (g, p, frequencies, true);

    // What comes in, faint; what goes out, lit.
    const graph::LevelAxis spectrumLevels { area, -90.0f, 6.0f };
    input.paint (g, frequencies, spectrumLevels, p.textMuted.withAlpha (GraphTint::spectrumFill),
                 p.textMuted.withAlpha (GraphTint::spectrumLine));
    output.paint (g, frequencies, spectrumLevels, tint.withAlpha (GraphTint::spectrumFill), tint.withAlpha (GraphTint::spectrumLine));

    paintResponse (g, p, tint);
    paintLfo (g, p, tint);
}

void SweepPicture::paintLfo (juce::Graphics& g, const Palette& p, juce::Colour tint) const
{
    const auto area = getLfoArea();
    graph::paintBackground (g, p, area);

    // One cycle of its sweep, the bottom of the sweep at the bottom.
    const auto curveArea = area.reduced ((float) Space::s, lfoDotRadius + 2.0f);
    const auto yFor = [&curveArea] (float value) { return curveArea.getCentreY() - value * curveArea.getHeight() * 0.5f; };
    const auto curve = graph::makeCurve (curveArea, [&] (float x)
                                         { return yFor (getSweepValue ((double) ((x - curveArea.getX()) / curveArea.getWidth()))); });
    g.setColour (tint.withAlpha (GraphTint::marker));
    g.strokePath (curve, juce::PathStrokeType (1.5f));

    // Each side's copies where they are now: the left's filled, the right's rings.
    for (int channel = 1; channel >= 0; --channel)
    {
        for (int voice = 0; voice < getNumVoices(); ++voice)
        {
            auto at = getVoicePhase (channel, voice);
            at -= std::floor (at);
            const auto copy = juce::Rectangle<float> (2.0f * lfoDotRadius, 2.0f * lfoDotRadius)
                                  .withCentre ({ curveArea.getX() + (float) at * curveArea.getWidth(), yFor (getSweepValue (at)) });
            g.setColour (tint);

            if (channel == 0)
                g.fillEllipse (copy);
            else
                g.drawEllipse (copy, 1.5f);
        }
    }

    graph::paintReadout (g, p, area, getRateText(), juce::Justification::centredRight);
}

juce::String SweepPicture::getRateText() const
{
    if (values[(size_t) syncIndex] > 0.5f)
    {
        const auto index =
            juce::jlimit (0, (int) std::size (devices::dsp::noteValueNames) - 1, juce::roundToInt (values[(size_t) divisionIndex]));
        const auto name = devices::dsp::noteValueNames[(size_t) index];
        return juce::String (name.data(), name.size());
    }

    return juce::String (values[(size_t) rateIndex], 2) + " Hz";
}

void SweepPicture::tick()
{
    readValues();
    phase = readPhase();
    auto spectraMoved = false;

    if (auto* scope = device.getScope())
    {
        scope->setListening (true);
        input.setSampleRate (scope->getSampleRate());
        output.setSampleRate (scope->getSampleRate());

        if (auto* tap = scope->getInput())
            input.pull (*tap);

        if (auto* tap = scope->getOutput())
            output.pull (*tap);

        spectraMoved = input.update();
        spectraMoved = output.update() || spectraMoved;
    }

    // The response and the LFO, at the values and where the LFO is (it rests while the device sleeps on silence).
    auto numbers = values;
    numbers.insert (numbers.end(), { phase, (float) getSampleRate() });

    if (drawn.changed (std::move (numbers)) || spectraMoved)
        repaint();
}
} // namespace phazite::ui
