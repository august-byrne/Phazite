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
        "Compress: turns the sound down (or up) as its curve says, in its character's colour, by its own level or by the track "
        "its sidechain listens to",
        "Duck: turns the sound down while another track plays, the one its sidechain listens to (a kick, for sidechain "
        "pumping), by its threshold, depth, attack, hold and release",
        "Transient: turns each hit's attack and what follows it up or down, whatever the level of the sound",
    };

    // How far the history shows Transient's gain up and down from its middle, in decibels: as far as it turns.
    constexpr float transientRange = 24.0f;
} // namespace

//==============================================================================
// The curve over its tools and Auto Makeup's switch, with the meter beside it of how far the sound goes down (from its middle
// down, in the warning colour) or up (from its middle up), and that as a number by the tools.
class CompressorEditor::CurvePart : public juce::Component
{
public:
    explicit CurvePart (CompressorEditor& e)
        : editor (e),
          curve (e.device, e.values),
          tools (curve),
          autoMakeup (e.device, getInfo (Compressor::autoMakeup).id, "Auto",
                      "Auto Makeup: turns the sound up after the curve by half what the curve takes off a 0 dB level, so that "
                      "a curve compresses without getting quieter; Out adds to it")
    {
        curve.setCurve (Compressor::points);
        curve.onEdit = [this] { editor.refresh(); };
        addAndMakeVisible (curve);
        addAndMakeVisible (tools);
        addAndMakeVisible (autoMakeup);
    }

    void resized() override
    {
        // The curve square, the meter right beside it, and the tools and the switch under both, the number at their right,
        // with room for it.
        auto area = getLocalBounds();
        auto toolbar = area.removeFromBottom (controlHeight);
        area.removeFromBottom (Space::xs);
        const auto side = std::min (area.getWidth() - GraphTint::meterWidth - Space::s, area.getHeight());
        curve.setBounds (area.removeFromLeft (side).withHeight (side));
        area.removeFromLeft (Space::s);
        meter = area.removeFromLeft (GraphTint::meterWidth).withHeight (side).toFloat();
        const auto switchWidth = autoMakeup.getPreferredWidth();
        toolbar.setWidth (
            std::max (side + Space::s + GraphTint::meterWidth, tools.getPreferredWidth() + Space::xs + switchWidth + readoutWidth));
        tools.setBounds (toolbar.removeFromLeft (tools.getPreferredWidth()));
        toolbar.removeFromLeft (Space::xs);
        autoMakeup.setBounds (toolbar.removeFromLeft (switchWidth));
        readout = toolbar.toFloat();
    }

    // The least width its toolbar takes.
    int getNarrowestWidth() { return tools.getPreferredWidth() + Space::xs + autoMakeup.getPreferredWidth() + readoutWidth; }

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
    ParameterSwitch autoMakeup;

private:
    juce::Rectangle<float> meter, readout;
    static constexpr int readoutWidth = 56;
};

//==============================================================================
// What the compressor listens to while it compresses or ducks, under its part's title, Sidechain: the track picked (given the
// project), as the multiband's Sidechain part shows its own, and under it the sidechain's cuts, and Listen beside them: while
// it is on and shows, the compressor plays what its detector hears, through its cuts, instead of the sound
// (Compressor::listenToKey(), which the refresh asks for again and again), to hear what makes it duck or compress. Hidden,
// or with the editor gone, the sound comes back within half a second.
class CompressorEditor::KeyPart : public juce::Component
{
public:
    KeyPart (ParameterControls& c, std::unique_ptr<SidechainButton> shownPicker)
        : controls (c),
          picker (std::move (shownPicker))
    {
        listen.setTitle ("Listen");
        listen.setTooltip ("Plays what the compressor listens to, through its cuts, instead of the sound, to hear what makes it "
                           "compress or duck");
        listen.setClickingTogglesState (true);
        addAndMakeVisible (controls);
        addAndMakeVisible (listen);

        if (picker != nullptr)
            addAndMakeVisible (*picker);
    }

    int getPreferredWidth() const
    {
        const auto cuts = controls.getPreferredWidth() + Space::xs + listenWidth;
        return picker != nullptr ? std::max (cuts, SidechainButton::minimumWidth) : cuts;
    }

    int getPreferredHeight() const { return (picker != nullptr ? controlHeight + Space::xs : 0) + controls.getPreferredHeight(); }

    void resized() override
    {
        // The track picked along the top, then the cuts and Listen.
        auto area = getLocalBounds();

        if (picker != nullptr)
        {
            picker->setBounds (area.removeFromTop (controlHeight));
            area.removeFromTop (Space::xs);
        }

        controls.setBounds (area.removeFromLeft (controls.getPreferredWidth()));
        area.removeFromLeft (Space::xs);
        listen.setBounds (area.withSizeKeepingCentre (std::min (area.getWidth(), listenWidth), controlHeight));
    }

    ParameterControls& controls;
    const std::unique_ptr<SidechainButton> picker; // given the project
    juce::TextButton listen { "Listen" };

private:
    static constexpr int listenWidth = 64;
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

    // What both pages show: its history, scrolling; each mode shows it its own way (modeShown()).
    auto& shownHistory = keep (std::make_unique<LevelHistoryView> ("Compressor history", historyLowest, meterRange));
    history = &shownHistory;
    const DeviceLayout::Part historyPart { "History", &shownHistory, historyWidth, 260, 96, true, GraphTint::minimumWidth };

    // Compress: the characters over the curve, beside the history over the knobs and the key it hears.
    auto& shownCurve = keep (std::make_unique<CurvePart> (*this));
    curvePart = &shownCurve;
    addControl (getInfo (Compressor::autoMakeup).id, shownCurve.autoMakeup);
    auto& shownCharacters = keep (
        std::make_unique<ChoiceButtons> (device, getInfo (Compressor::character).id, ChoiceButtons::Direction::across, characterTips));
    characters = &shownCharacters;
    addControl (getInfo (Compressor::character).id, shownCharacters);

    const std::vector<std::string_view> compressIDs { "attack", "release", "levelHold", "lookahead", "inputGain", "drive", "makeup" };
    auto compressKnobs = makeControlsPart ("Compress", compressIDs, 0, ParameterControls::Size::compact,
                                           { "Attack", "Release", "Hold", "Look-Ahead", "In", "Drive", "Out" });
    compressKnobs.minimumWidth = (int) compressIDs.size() * narrowestKnob;

    // A gate drawn on the curve closes as the gain goes down and opens as it comes back: at the Attack and the Release, the
    // reverse of a gate's own words. Their tooltips here say both.
    if (auto* knobs = dynamic_cast<ParameterControls*> (compressKnobs.content))
    {
        for (const auto& [parameterID, meaning] :
             { std::pair ("attack", "Attack: how fast the gain goes down: a compressor gripping, a gate closing"),
               std::pair ("release", "Release: how fast the gain comes back: a compressor letting go, a gate opening") })
        {
            if (auto* client = dynamic_cast<juce::SettableTooltipClient*> (knobs->findControl (parameterID)))
            {
                const auto rest = client->getTooltip().fromFirstOccurrenceOf (": ", false, false);
                client->setTooltip (juce::String (meaning) + ". " + rest.substring (0, 1).toUpperCase() + rest.substring (1));
            }
        }
    }

    const auto characterWidth = std::max (curveWidth, shownCharacters.getPreferredWidth());
    const auto narrowest = std::max (narrowestCurve, shownCurve.getNarrowestWidth());

    // Duck: the key over its cuts and Listen, beside the history over the knobs.
    auto& cuts =
        makeControls ({ "keyLowCut", "keyHighCut" }, 2, ParameterControls::Size::compact, sidechainLabel, { "Low Cut", "High Cut" });
    auto& shownKey =
        keep (std::make_unique<KeyPart> (cuts, context != nullptr ? std::make_unique<SidechainButton> (*context, id) : nullptr));
    keyPart = &shownKey;
    keyPicker = shownKey.picker.get();
    auto duckKnobs = makeControlsPart ("Duck", { "threshold", "depth", "attack", "hold", "release", "releaseCurve" }, 0,
                                       ParameterControls::Size::compact, { "Threshold", "Depth", "Attack", "Hold", "Release", "Curve" });
    duckKnobs.minimumWidth = 6 * narrowestKnob;

    // Duck takes a millisecond at least to come back, under the Release's floor, which is the gates': its tooltip here says so.
    if (auto* knobs = dynamic_cast<ParameterControls*> (duckKnobs.content))
        if (auto* client = dynamic_cast<juce::SettableTooltipClient*> (knobs->findControl ("release")))
            client->setTooltip (
                "Release: the time the sound takes to come back, 1 ms at least while it ducks (a shorter one, which a gate on "
                "Compress's page may have, comes back over 1 ms here, so that the whole depth doesn't click back). "
                + client->getTooltip().fromFirstOccurrenceOf (": ", false, false).substring (0, 1).toUpperCase()
                + client->getTooltip().fromFirstOccurrenceOf (": ", false, false).substring (1));

    // Transient: the history, beside its punch and sustain, large, over its speed, makeup and low cut.
    const auto punchAndSustain = makeControlsPart ("Transient", { "punch", "sustain" }, 0, ParameterControls::Size::large);
    auto transientKnobs = makeControlsPart ("Ear and Out", { "speed", "makeup", "keyLowCut" }, 0, ParameterControls::Size::compact,
                                            { "Speed", "Out", "Low Cut" });

    const DeviceLayout::Part charactersPart { .title = "Character",
                                              .content = &shownCharacters,
                                              .width = characterWidth,
                                              .height = controlHeight,
                                              .minimumHeight = controlHeight,
                                              .minimumWidth = narrowest };
    const DeviceLayout::Part curveGraph { "Curve", &shownCurve, characterWidth, 300, 150, true, narrowest };
    const auto keyHeight = shownKey.getPreferredHeight();
    const DeviceLayout::Part sidechainPart { "Sidechain", &shownKey, shownKey.getPreferredWidth(), keyHeight, keyHeight };
    setModes (getInfo (Compressor::mode).id,
              { { { charactersPart, curveGraph }, { historyPart, compressKnobs, sidechainPart } },
                { { sidechainPart }, { historyPart, duckKnobs } },
                { { historyPart }, { punchAndSustain, transientKnobs } } },
              modeTips);

    // The meters' first readings would show what played before the editor opened.
    shownAgain();
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

bool CompressorEditor::isShapingTransients() const noexcept
{
    return getShownMode() == (int) Compressor::Mode::transient;
}

void CompressorEditor::modeShown (int)
{
    // Compressing, what comes in and what goes out, how far it turns down from the top over half the height, or with a key,
    // the key; ducking, the sound it ducks as it comes in and goes out, with the key (without one, how far it turns down, on
    // the levels' own scale) and the threshold's line; shaping transients, how far it turns up and down about the middle.
    const auto ducking = isDucking();
    const auto lineName = getInfo (Compressor::threshold).name;
    history->setLine (ducking ? &threshold : nullptr, juce::String (lineName.data(), lineName.size()));
    history->setReductionScale (ducking ? -historyLowest : meterRange, ducking ? 1.0f : 0.5f);
    history->setGainAroundMiddle (isShapingTransients() ? transientRange : 0.0f);
    showKey();

    // Where Compress's page is too short for its history, its knobs and its key one over another (the default dock), its
    // knobs show first, under their tab beside the history's and the key's.
    if (getShownMode() == (int) Compressor::Mode::compress && ! knobsChosen)
    {
        knobsChosen = true;
        getLayout().showPart (1, 1);
    }
}

void CompressorEditor::showKey()
{
    const auto key = device.getSidechainSource();
    history->setKey (isShapingTransients() ? juce::String() : key.name, key.colour);

    if (keyPicker != nullptr)
        keyPicker->show (key);
}

void CompressorEditor::shownAgain()
{
    // What played while nobody looked (or before the editor opened) isn't shown: the meters and the peaks start from now, and
    // the history from its next reading.
    if (auto* compressor = device.getProcessor<Compressor>())
    {
        compressor->getLevelMeter().take();
        compressor->getReductionMeter().take();
        compressor->getBoostMeter().take();
    }

    if (auto* scope = device.getScope())
        scope->drain();

    shownLevel = silence;
    shownReduction = 0.0f;
    shownBoost = 0.0f;
    history->clear();
}

void CompressorEditor::refresh()
{
    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].isValid())
            values[i] = parameters[i].getValue();

    characters->refresh();

    // The meters jump up and fall back smoothly; the history takes each reading as it comes, with what comes in and goes out.
    auto level = silence, reduction = 0.0f, boost = 0.0f, input = silence, output = silence;

    if (auto* compressor = device.getProcessor<Compressor>())
    {
        level = compressor->getLevelMeter().take();
        reduction = compressor->getReductionMeter().take();
        boost = compressor->getBoostMeter().take();

        // Listen: what the detector hears, for as long as the editor shows Listen, on Compress's page or Duck's, and asks.
        if (! isShapingTransients() && keyPart->listen.getToggleState() && keyPart->listen.isShowing())
            compressor->listenToKey();
    }

    if (auto* scope = device.getScope())
    {
        scope->setListening (true);
        input = scope->getInputPeak().take();
        output = scope->getOutputPeak().take();
    }

    shownLevel = std::max (level, shownLevel - meterFall);
    shownReduction = std::max (reduction, shownReduction - meterFall);
    shownBoost = std::max (boost, shownBoost - meterFall);

    // The sound's peak as it came in and as it went out, as each mode's page shows them; with a key, also the key as the
    // detector heard it (ducking, against the threshold). Compressing without one, the detector's level is the curve's dot
    // and the meter's alone: after the input gain and the key's low cut, and for some characters an average, it isn't what
    // came in. Shaping transients, how far it turned up (below 0) or down, whichever went further.
    const auto now = juce::Time::getMillisecondCounterHiRes();

    if (isShapingTransients())
        history->push (now, device.getSongTime(), input, output, boost > reduction ? -boost : reduction);
    else if (isDucking() || history->getKeyName().isNotEmpty())
        history->push (now, device.getSongTime(), input, output, reduction, level);
    else
        history->push (now, device.getSongTime(), input, output, reduction);

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
