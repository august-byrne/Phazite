// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CompressorEditor.h"
#include "DeviceTint.h"
#include "SidechainMenus.h"
#include "devices/dynamics/Compressor.h"
#include "model/project/ProjectContext.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    using devices::Compressor;

    constexpr float meterRange = 24.0f; // the meter shows the sound turned down, or up, by up to this many dB
    constexpr float meterFall = 0.5f;   // dB per refresh: 15 dB a second
    constexpr float silence = -120.0f;
    constexpr float historyLowest = -60.0f; // the history's levels, from here to 0 dB: a ducking threshold's whole range

    // Its pages' widths: the curve's column, as narrow as its characters' row takes; and the knobs', each at least a knob wide.
    constexpr int curveWidth = 300, narrowestCurve = 216;
    constexpr int historyWidth = 400;
    constexpr int narrowestKnob = 40;

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

    // What each mode does, as its tab's tooltip says.
    const std::vector<juce::String> modeTips {
        "Compress: turns its own sound down (or up) as its curve says, in its character's colour",
        "Duck: turns the sound down under another track, its key (a kick, for the pump), by its threshold, depth, attack, hold "
        "and release",
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
        // The curve square, the meter right beside it, and the tools under both, the number at their right, with room for it.
        auto area = getLocalBounds();
        auto toolbar = area.removeFromBottom (controlHeight);
        area.removeFromBottom (Space::xs);
        const auto side = std::min (area.getWidth() - GraphTint::meterWidth - Space::s, area.getHeight());
        curve.setBounds (area.removeFromLeft (side).withHeight (side));
        area.removeFromLeft (Space::s);
        meter = area.removeFromLeft (GraphTint::meterWidth).withHeight (side).toFloat();
        toolbar.setWidth (std::max (side + Space::s + GraphTint::meterWidth, tools.getPreferredWidth() + readoutWidth));
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
    static constexpr int readoutWidth = 56;
};

//==============================================================================
// What the compressor listens to while it ducks: its key, the track picked (given the project), and under it the key's cuts,
// and Listen beside them: while it is on and the editor shows Duck's page, the compressor plays its key, through its cuts,
// instead of the sound (Compressor::listenToKey(), which the refresh asks for again and again), to hear what makes it duck.
// Hidden, or with the editor gone, the sound comes back within half a second.
class CompressorEditor::KeyPart : public juce::Component
{
public:
    KeyPart (ParameterControls& c, std::unique_ptr<SidechainButton> shownPicker)
        : controls (c),
          picker (std::move (shownPicker))
    {
        listen.setTitle ("Listen");
        listen.setTooltip ("Plays what the compressor listens to, through its cuts, instead of the sound, to hear what makes it duck");
        listen.setClickingTogglesState (true);
        addAndMakeVisible (controls);
        addAndMakeVisible (listen);

        if (picker != nullptr)
        {
            picker->setTitle ("Key");
            label.setFont (juce::FontOptions (TextSize::small));
            label.setAccessible (false); // the picker's title says it
            label.setInterceptsMouseClicks (false, false);
            setThemeColour (label, juce::Label::textColourId, &Palette::textMuted);
            addAndMakeVisible (label);
            addAndMakeVisible (*picker);
        }
    }

    int getPreferredWidth() const
    {
        const auto cuts = controls.getPreferredWidth() + Space::xs + listenWidth;
        return picker != nullptr ? std::max (cuts, labelWidth + SidechainButton::minimumWidth) : cuts;
    }

    int getPreferredHeight() const { return (picker != nullptr ? controlHeight + Space::xs : 0) + controls.getPreferredHeight(); }

    void resized() override
    {
        // The key along the top, then its cuts and Listen.
        auto area = getLocalBounds();

        if (picker != nullptr)
        {
            auto row = area.removeFromTop (controlHeight);
            area.removeFromTop (Space::xs);
            label.setBounds (row.removeFromLeft (labelWidth));
            picker->setBounds (row);
        }

        controls.setBounds (area.removeFromLeft (controls.getPreferredWidth()));
        area.removeFromLeft (Space::xs);
        listen.setBounds (area.withSizeKeepingCentre (std::min (area.getWidth(), listenWidth), controlHeight));
    }

    ParameterControls& controls;
    const std::unique_ptr<SidechainButton> picker; // given the project
    juce::TextButton listen { "Listen" };

private:
    juce::Label label { {}, "Key" };
    static constexpr int listenWidth = 64, labelWidth = 32;
};

//==============================================================================
CompressorEditor::CompressorEditor (const DeviceHandle& handle, ProjectContext* projectContext, DeviceID id)
    : DeviceEditorBase (handle),
      DisplayTicker::Display (static_cast<juce::Component&> (*this)),
      context (projectContext),
      threshold (handle.getParameter (getInfo (Compressor::threshold).id)),
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

    // The meters' first readings would show what played before the editor opened.
    if (auto* compressor = device.getProcessor<Compressor>())
    {
        compressor->getLevelMeter().take();
        compressor->getReductionMeter().take();
        compressor->getBoostMeter().take();
    }

    if (auto* scope = device.getScope())
        scope->getOutputPeak().take();

    // What both pages show: its history, scrolling; each mode shows it its own way (modeShown()).
    auto& shownHistory = keep (std::make_unique<LevelHistoryView> ("Compressor history", historyLowest, meterRange));
    history = &shownHistory;
    const DeviceLayout::Part historyPart { "History", &shownHistory, historyWidth, 260, 96, true, GraphTint::minimumWidth };

    // Compress: the characters over the curve, beside the history over the knobs.
    auto& shownCurve = keep (std::make_unique<CurvePart> (*this));
    curvePart = &shownCurve;
    auto& shownCharacters = keep (
        std::make_unique<ChoiceButtons> (device, getInfo (Compressor::character).id, ChoiceButtons::Direction::across, characterTips));
    characters = &shownCharacters;
    addControl (getInfo (Compressor::character).id, shownCharacters);

    auto compressKnobs = makeControlsPart ("Compress", { "attack", "release", "inputGain", "drive", "makeup", "keyLowCut" }, 0,
                                           ParameterControls::Size::compact, { "Attack", "Release", "In", "Drive", "Out", "Low Cut" });
    compressKnobs.minimumWidth = 6 * narrowestKnob;
    const auto characterWidth = std::max (curveWidth, shownCharacters.getPreferredWidth());

    // Duck: the key over its cuts and Listen, beside the history over the knobs.
    auto& cuts = makeControls ({ "keyLowCut", "keyHighCut" }, 2, ParameterControls::Size::compact, "Key", { "Low Cut", "High Cut" });
    auto& shownKey =
        keep (std::make_unique<KeyPart> (cuts, context != nullptr ? std::make_unique<SidechainButton> (*context, id) : nullptr));
    keyPart = &shownKey;
    keyPicker = shownKey.picker.get();
    auto duckKnobs = makeControlsPart ("Duck", { "threshold", "depth", "attack", "hold", "release", "releaseCurve" }, 0,
                                       ParameterControls::Size::compact, { "Threshold", "Depth", "Attack", "Hold", "Release", "Curve" });
    duckKnobs.minimumWidth = 6 * narrowestKnob;

    const DeviceLayout::Part charactersPart { .title = "Character",
                                              .content = &shownCharacters,
                                              .width = characterWidth,
                                              .height = controlHeight,
                                              .minimumHeight = controlHeight,
                                              .minimumWidth = narrowestCurve };
    const DeviceLayout::Part curveGraph { "Curve", &shownCurve, characterWidth, 300, 150, true, narrowestCurve };
    const auto keyHeight = shownKey.getPreferredHeight();
    const DeviceLayout::Part sidechainPart { "Sidechain", &shownKey, shownKey.getPreferredWidth(), keyHeight, keyHeight };
    setModes (getInfo (Compressor::mode).id,
              { { { charactersPart, curveGraph }, { historyPart, compressKnobs } }, { { sidechainPart }, { historyPart, duckKnobs } } },
              modeTips);
    refresh();
}

CompressorEditor::~CompressorEditor()
{
    history->setLine (nullptr, {}); // the threshold's handle goes before it

    if (auto* scope = device.getScope())
        scope->setListening (false);
}

juce::Button& CompressorEditor::getModeTab (int mode)
{
    return getModeTabs()->getTab (mode);
}

bool CompressorEditor::isDucking() const noexcept
{
    return getShownMode() == (int) Compressor::Mode::duck;
}

DynamicsCurve& CompressorEditor::getCurve()
{
    return curvePart->curve;
}

LevelHistoryView& CompressorEditor::getHistory()
{
    return *history;
}

juce::Button& CompressorEditor::getListenButton()
{
    return keyPart->listen;
}

juce::Button& CompressorEditor::getCharacterButton (int character)
{
    return characters->getButton (character);
}

void CompressorEditor::modeShown (int)
{
    // Compressing, what comes in and goes out, how far it turns down from the top over half the height; ducking, the key and
    // its threshold's line, how far it turns down on the levels' own scale, as a gain.
    const auto ducking = isDucking();
    const auto lineName = getInfo (Compressor::threshold).name;
    history->setShowsOutput (! ducking);
    history->setLine (ducking ? &threshold : nullptr, juce::String (lineName.data(), lineName.size()));
    history->setReductionScale (ducking ? -historyLowest : meterRange, ducking ? 1.0f : 0.5f);
    showKey();
}

void CompressorEditor::showKey()
{
    const auto key = device.getSidechainSource();
    history->setKey (isDucking() ? key.name : juce::String(), key.colour);

    if (keyPicker != nullptr)
        keyPicker->show (key);
}

void CompressorEditor::refresh()
{
    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].isValid())
            values[i] = parameters[i].getValue();

    characters->refresh();

    // The meters jump up and fall back smoothly; the history takes each reading as it comes, with what goes out.
    auto level = silence, reduction = 0.0f, boost = 0.0f, output = silence;

    if (auto* compressor = device.getProcessor<Compressor>())
    {
        level = compressor->getLevelMeter().take();
        reduction = compressor->getReductionMeter().take();
        boost = compressor->getBoostMeter().take();

        // Listen: the key, for as long as the editor shows Duck's page and asks.
        if (isDucking() && keyPart->listen.getToggleState())
            compressor->listenToKey();
    }

    if (auto* scope = device.getScope())
    {
        scope->setListening (true);
        output = scope->getOutputPeak().take();
    }

    shownLevel = std::max (level, shownLevel - meterFall);
    shownReduction = std::max (reduction, shownReduction - meterFall);
    shownBoost = std::max (boost, shownBoost - meterFall);
    history->push (juce::Time::getMillisecondCounterHiRes(), device.getSongTime(), level, output, reduction);

    // What it listens to, in its colour and named in the history, and in the picker: again as the song changes (its own
    // undo, Duck Under, a track renamed or recoloured).
    if (context == nullptr || context->getSongVersion() != shownSongVersion)
    {
        shownSongVersion = context != nullptr ? context->getSongVersion() : -1;
        showKey();
    }

    // The curve, lit up to the level heard, and the meter beside it, repainted when they move.
    curvePart->curve.setHeardLevel (shownLevel);
    auto numbers = values;
    numbers.insert (numbers.end(), { shownLevel, shownReduction, shownBoost });

    if (drawnCurve.changed (std::move (numbers)))
        curvePart->repaint();
}
} // namespace phazite::ui
