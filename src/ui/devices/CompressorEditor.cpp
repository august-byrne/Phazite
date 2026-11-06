// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CompressorEditor.h"
#include "DeviceTint.h"
#include "devices/dynamics/Compressor.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    using devices::Compressor;

    constexpr float meterRange = 24.0f; // the meter shows the sound turned down, or up, by up to this many dB
    constexpr float meterFall = 0.5f;   // dB per refresh: 15 dB a second
    constexpr float silence = -120.0f;

    const devices::ParameterInfo& getInfo (int index)
    {
        return Compressor::info.parameters[(size_t) index];
    }

    // What each character is for, as its button's tooltip says.
    const std::vector<juce::String> characterTips {
        "Clean: hears each peak and turns it as the curve says, with no colour of its own",
        "VCA: hears the average level, so short peaks pass through: punchy, for drums and buses",
        "FET: hears each peak, and adds odd harmonics the harder it turns down: aggressive, for drums and vocals",
        "Opto: hears the average and lets go fast at first, then slowly, the longer it held the slower: smooth, for vocals and bass",
        "Tube: hears the average, and adds even harmonics the harder it turns down: warm glue for a mix",
    };
} // namespace

//==============================================================================
// The curve over its tools, with the meter beside it of how far the sound goes down (from its middle down, in the warning
// colour) or up (from its middle up), and that as a number by the tools.
class CompressorEditor::CurvePart : public juce::Component
{
public:
    explicit CurvePart (CompressorEditor& e)
        : editor (e),
          curve (e.device, e.values),
          tools (curve)
    {
        curve.setCurve (Compressor::points);
        curve.onEdit = [this] { editor.refresh(); };
        addAndMakeVisible (curve);
        addAndMakeVisible (tools);
    }

    void resized() override
    {
        // The curve square, the meter right beside it, and the tools under both, the number at their right.
        auto area = getLocalBounds();
        auto toolbar = area.removeFromBottom (controlHeight);
        area.removeFromBottom (Space::xs);
        const auto side = std::min (area.getWidth() - GraphTint::meterWidth - Space::s, area.getHeight());
        curve.setBounds (area.removeFromLeft (side).withHeight (side));
        area.removeFromLeft (Space::s);
        meter = area.removeFromLeft (GraphTint::meterWidth).withHeight (side).toFloat();
        toolbar.setWidth (side + Space::s + GraphTint::meterWidth);
        tools.setBounds (toolbar.removeFromLeft (tools.getPreferredWidth()));
        readout = toolbar.toFloat();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& palette = getPalette (*this);
        g.setColour (palette.background);
        g.fillRoundedRectangle (meter, cornerRadius);
        const auto middle = meter.getCentreY();
        const auto halfHeight = meter.getHeight() / 2.0f;
        const auto reduction = juce::jlimit (0.0f, 1.0f, editor.shownReduction / meterRange) * halfHeight;
        const auto boost = juce::jlimit (0.0f, 1.0f, editor.shownBoost / meterRange) * halfHeight;
        g.setColour (palette.warning);
        g.fillRect (meter.withTop (middle).withHeight (reduction));
        g.setColour (getDeviceTint (*this));
        g.fillRect (meter.withTop (middle - boost).withHeight (boost));
        g.setColour (palette.text.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (middle), meter.getX(), meter.getRight());

        const auto gain = editor.shownBoost > editor.shownReduction ? editor.shownBoost : -editor.shownReduction;
        g.setColour (palette.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (std::abs (gain) >= 0.05f ? (gain > 0.0f ? "+" : "-") + juce::String (std::abs (gain), 1) + " dB"
                                             : juce::String ("0 dB"),
                    readout, juce::Justification::centredRight);
    }

    CompressorEditor& editor;
    DynamicsCurve curve;
    DynamicsCurveTools tools;

private:
    juce::Rectangle<float> meter, readout;
};

//==============================================================================
// The key's cuts, and Listen beside them: while it is on and the editor shows, the compressor plays its key, through its
// cuts, instead of the sound (Compressor::listenToKey(), which the refresh asks for again and again), to hear what makes it
// duck. Hidden, or with the editor gone, the sound comes back within half a second.
class CompressorEditor::KeyPart : public juce::Component
{
public:
    explicit KeyPart (ParameterControls& c)
        : controls (c)
    {
        listen.setTitle ("Listen");
        listen.setTooltip ("Plays what the compressor listens to, through its cuts, instead of the sound, to hear what makes it duck");
        listen.setClickingTogglesState (true);
        addAndMakeVisible (controls);
        addAndMakeVisible (listen);
    }

    int getPreferredWidth() const { return controls.getPreferredWidth() + Space::xs + listenWidth; }

    void resized() override
    {
        auto area = getLocalBounds();
        controls.setBounds (area.removeFromLeft (controls.getPreferredWidth()));
        area.removeFromLeft (Space::xs);
        listen.setBounds (area.withSizeKeepingCentre (area.getWidth(), controlHeight));
    }

    ParameterControls& controls;
    juce::TextButton listen { "Listen" };

private:
    static constexpr int listenWidth = 64;
};

//==============================================================================
CompressorEditor::CompressorEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle),
      DisplayTicker::Display (static_cast<juce::Component&> (*this)),
      shownLevel (silence)
{
    for (const auto& parameter : Compressor::info.parameters)
    {
        parameters.push_back (device.getParameter (parameter.id));
        values.push_back (parameter.defaultValue);
    }

    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].isValid())
            values[i] = parameters[i].getValue();

    auto& shownCurve = keep (std::make_unique<CurvePart> (*this));
    curvePart = &shownCurve;
    auto& shownCharacters = keep (
        std::make_unique<ChoiceButtons> (device, getInfo (Compressor::character).id, ChoiceButtons::Direction::across, characterTips));
    characters = &shownCharacters;
    addControl (getInfo (Compressor::character).id, shownCharacters);

    // The meters' first readings would show what played before the editor opened.
    if (auto* compressor = device.getProcessor<Compressor>())
    {
        compressor->getLevelMeter().take();
        compressor->getReductionMeter().take();
        compressor->getBoostMeter().take();
    }

    const auto timing = makeControlsPart ("Timing", { "attack", "release" }, 2, ParameterControls::Size::large);
    const auto gains =
        makeControlsPart ("Gain", { "inputGain", "drive", "makeup" }, 3, ParameterControls::Size::compact, { "In", "Drive", "Out" });
    auto& cuts = makeControls ({ "keyLowCut", "keyHighCut" }, 2, ParameterControls::Size::compact, "Key", { "Low Cut", "High Cut" });
    auto& shownKey = keep (std::make_unique<KeyPart> (cuts));
    keyPart = &shownKey;

    const auto width = std::max (260, shownCharacters.getPreferredWidth());
    setParts ({ { { "Character", &shownCharacters, width, controlHeight, controlHeight }, { "Curve", &shownCurve, width, 300, 150, true } },
                { timing, gains },
                { { "Sidechain", &shownKey, shownKey.getPreferredWidth(), cuts.getPreferredHeight(), cuts.getPreferredHeight() } } });
}

CompressorEditor::~CompressorEditor() = default;

DynamicsCurve& CompressorEditor::getCurve()
{
    return curvePart->curve;
}

juce::Button& CompressorEditor::getListenButton()
{
    return keyPart->listen;
}

juce::Button& CompressorEditor::getCharacterButton (int character)
{
    return characters->getButton (character);
}

void CompressorEditor::refresh()
{
    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].isValid())
            values[i] = parameters[i].getValue();

    characters->refresh();

    // The meters jump up and fall back smoothly.
    if (auto* compressor = device.getProcessor<Compressor>())
    {
        const auto level = compressor->getLevelMeter().take(), reduction = compressor->getReductionMeter().take(),
                   boost = compressor->getBoostMeter().take();
        shownLevel = std::max (level, shownLevel - meterFall);
        shownReduction = std::max (reduction, shownReduction - meterFall);
        shownBoost = std::max (boost, shownBoost - meterFall);

        // Listen: the key, for as long as the editor shows and asks.
        if (keyPart->listen.getToggleState())
            compressor->listenToKey();
    }

    // The curve, lit up to the level heard, and the meter beside it, repainted when they move.
    curvePart->curve.setHeardLevel (shownLevel);
    auto numbers = values;
    numbers.insert (numbers.end(), { shownLevel, shownReduction, shownBoost });

    if (drawnCurve.changed (std::move (numbers)))
        curvePart->repaint();
}
} // namespace phazite::ui
