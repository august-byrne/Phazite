// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SynthEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "devices/synth/Synth.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "ui/input/LibraryDrag.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace phazite::ui
{
namespace
{
    using devices::Synth;

    constexpr int pointsPerWave = 72;               // where each frame's wave is drawn through
    constexpr int playedPoints = 4 * pointsPerWave; // and the one it plays, through its warp (sync's repeats among them)

    std::string_view getParameterID (int index)
    {
        return Synth::info.parameters[(size_t) index].id;
    }

    std::string_view getParameterID (int osc, Synth::OscillatorField field)
    {
        return getParameterID (Synth::getParameter (osc, field));
    }

    juce::String getOscillatorName (int osc)
    {
        return "Osc " + juce::String (osc + 1);
    }

    // The filter's part: its response over its controls, two rows of four, the second with its slope and its Drive, then, in
    // the rest of that row after the Drive, its Character, Clean or Driven, as two buttons, as the Filter device's: no
    // taller than its controls alone.
    class FilterPart : public juce::Component
    {
    public:
        FilterPart (juce::Component& p, ParameterControls& c, ChoiceButtons& b)
            : picture (p),
              controls (c),
              buttons (b)
        {
            addAndMakeVisible (picture);
            addAndMakeVisible (controls);
            addAndMakeVisible (buttons); // over the controls' empty cells
        }

        // As wide as the controls' grid: the buttons take its two cells after the Drive, as wide as their words need.
        int getPreferredWidth() const { return controls.getPreferredWidth(); }

        int getBelowPicture() const { return Space::xs + controls.getPreferredHeight(); }

        void resized() override
        {
            auto area = getLocalBounds();
            controls.setBounds (area.removeFromBottom (controls.getPreferredHeight()));
            area.removeFromBottom (Space::xs);
            picture.setBounds (area);

            const auto rowHeight = ParameterControls::getCellHeight (ParameterControls::Size::compact);
            const auto left = getButtonsLeft();
            buttons.setBounds (controls.getX() + left, controls.getY() + rowHeight + (rowHeight - buttons.getPreferredHeight()) / 2,
                               std::max (0, controls.getWidth() - left), buttons.getPreferredHeight());
        }

    private:
        juce::Component& picture;
        ParameterControls& controls;
        ChoiceButtons& buttons;

        // Where the buttons start in the controls: after the Drive's cell.
        int getButtonsLeft() const
        {
            const auto* drive = controls.findControl ("drive");
            return drive != nullptr ? drive->getRight() + Space::xs : 0;
        }
    };
} // namespace

//==============================================================================
class SynthEditor::TableView : public juce::Component,
                               public juce::SettableTooltipClient,
                               public juce::DragAndDropTarget,
                               public juce::FileDragAndDropTarget
{
public:
    TableView (SynthEditor& e, int index, ParameterHandle positionHandle)
        : editor (e),
          osc (index),
          position (std::move (positionHandle))
    {
        setTitle (getOscillatorName (osc) + " Wavetable");
        setTooltip ("Drag right or up to move through the table, left or down to go back; drop a sound on it to make that the table");
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        drag.onCancelled = [this] { repaint(); };
    }

    void setTable (std::shared_ptr<const devices::Wavetable> newTable, const juce::String& name)
    {
        if (newTable == table)
            return;

        table = std::move (newTable);
        setDescription (name);
        rebuild();
        repaint();
    }

    // Where it plays in its table, whether it is on, and its warp at the amount it plays it at: it repaints only when one
    // moved.
    void setPlayed (float newPosition, bool isOn, Synth::Warp newWarp, float newAmount)
    {
        if (std::abs (newPosition - played) > 0.0005f || isOn != on || newWarp != warp || std::abs (newAmount - warpAmount) > 0.0005f)
        {
            played = newPosition;
            on = isOn;
            warp = newWarp;
            warpAmount = newAmount;
            repaint();
        }
    }

    float getPlayedPosition() const noexcept { return played; }
    int getNumFramesDrawn() const noexcept { return (int) frames.size(); }

    // The frame it plays as the oscillator reads it, at playedPoints + 1 places across its cycle, from its start to its end:
    // through its warp at its amount (Synth::readWarped(), the synth's own mapping), each place read from the frames drawn
    // as makeWave() reads them. FM and ring modulation, which depend on the other oscillator's sound, the frame as it is.
    std::vector<float> getPlayedWave() const
    {
        std::vector<float> wave;

        if (table == nullptr)
            return wave;

        const auto place = std::clamp (played, 0.0f, 1.0f) * (float) (table->getNumFrames() - 1);
        const auto first = std::min ((int) place, table->getNumFrames() - 1);
        const auto from = table->getFrameForDrawing (first);
        const auto to = table->getFrameForDrawing (std::min (first + 1, table->getNumFrames() - 1));
        const auto morph = place - (float) first;
        const auto read = [&] (float at) { return readDrawn (from, at) + morph * (readDrawn (to, at) - readDrawn (from, at)); };
        const auto shape = Synth::getWarpShape (warp, warpAmount);

        for (int i = 0; i <= playedPoints; ++i)
            wave.push_back (Synth::readWarped (warp, shape, (float) i / (float) playedPoints, read));

        return wave;
    }

    // What its readout says: for a warp that the other oscillator's sound makes (FM, ring modulation), which the drawing
    // can't show, its name; otherwise nothing.
    juce::String getReadout() const
    {
        if (warp != Synth::Warp::fm && warp != Synth::Warp::ringMod)
            return {};

        const auto name = Synth::info.parameters[(size_t) Synth::getParameter (osc, Synth::oscWarp)].choices[(size_t) warp];
        return { name.data(), name.size() };
    }

    void paint (juce::Graphics& g) override
    {
        const auto& palette = getPalette (*this);
        const auto area = getLocalBounds().toFloat();
        g.setColour (palette.surfaceRaised);
        g.fillRoundedRectangle (area, cornerRadius);

        if (dropping)
        {
            g.setColour (palette.accent.withAlpha (WavetableTint::dropWash));
            g.fillRoundedRectangle (area, cornerRadius);
        }

        g.setColour (dropping ? palette.accent : palette.border);
        g.drawRoundedRectangle (area.reduced (0.5f), cornerRadius, 1.0f);

        // The frames, the back ones first, so that those in front cover them.
        const auto alpha = on ? 1.0f : GraphTint::partOff;
        g.setColour (palette.textMuted.withAlpha (WavetableTint::frames * alpha));

        for (auto frame = frames.rbegin(); frame != frames.rend(); ++frame)
            g.strokePath (*frame, juce::PathStrokeType (WavetableTint::lineWidth));

        // The one it plays, as its warp reads it, which a drag moves: ringed in the text colour under the pointer and while
        // dragged.
        if (table != nullptr)
        {
            const auto wave = makePath (getPlayedWave(), getFrameBox (std::clamp (played, 0.0f, 1.0f)));

            if (hovered || drag.isTaken())
            {
                g.setColour (palette.text.withAlpha (alpha));
                g.strokePath (wave, juce::PathStrokeType (WavetableTint::playedWidth + 2.0f * GraphTint::litGap,
                                                          juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            g.setColour (getDeviceTint (*this).withAlpha (alpha));
            g.strokePath (wave, juce::PathStrokeType (WavetableTint::playedWidth));
        }

        if (const auto readout = getReadout(); readout.isNotEmpty())
            graph::paintReadout (g, getPalette (*this), area.reduced ((float) Space::xs), readout, juce::Justification::topLeft);
    }

    void resized() override { rebuild(); }

    // A drag across it moves the oscillator's position: the width of the drawing, right or up, is the whole table.
    // From the position set by hand, not where its modulation plays it now (graph::HandleDrag).
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            drag.take ({ &position });
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! drag.isTaken())
            return;

        const auto moved = (float) (e.getDistanceFromDragStartX() - e.getDistanceFromDragStartY()) / (float) std::max (1, getWidth());
        drag.set (0, std::clamp (drag.getStart (0) + moved, 0.0f, 1.0f));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        drag.end();
        repaint();
    }

    // Under the pointer, what a drag moves lights up: the frame it plays.
    void mouseEnter (const juce::MouseEvent&) override { setHovered (true); }
    void mouseMove (const juce::MouseEvent&) override { setHovered (true); }
    void mouseExit (const juce::MouseEvent&) override { setHovered (false); }

    // From the browser: a sound file.
    bool isInterestedInDragSource (const SourceDetails& details) override { return getDraggedFile (details.description).has_value(); }
    void itemDragEnter (const SourceDetails&) override { setDropping (true); }
    void itemDragExit (const SourceDetails&) override { setDropping (false); }

    void itemDropped (const SourceDetails& details) override
    {
        setDropping (false);

        if (const auto file = getDraggedFile (details.description); file.has_value() && editor.dropFile (osc, *file))
            markPlaced (details.description);
    }

    // From Explorer.
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return files.size() == 1; }
    void fileDragEnter (const juce::StringArray&, int, int) override { setDropping (true); }
    void fileDragExit (const juce::StringArray&) override { setDropping (false); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        setDropping (false);
        editor.dropFile (osc, juce::File (files[0]));
    }

private:
    SynthEditor& editor;
    const int osc;
    ParameterHandle position;
    graph::HandleDrag drag { *this }; // after the handle it moves
    std::shared_ptr<const devices::Wavetable> table;
    std::vector<juce::Path> frames; // those drawn, from the front
    float played = 0.0f;
    Synth::Warp warp = Synth::Warp::none;
    float warpAmount = 0.0f;
    bool on = true, dropping = false, hovered = false;

    // A frame's wave drawn at a place in its cycle, from 0 to 1: its nearest sample.
    static float readDrawn (std::span<const float> samples, float along)
    {
        return samples.empty() ? 0.0f : samples[std::min (samples.size() - 1, (size_t) (along * (float) samples.size()))];
    }

    // A wave's values, evenly across a box.
    static juce::Path makePath (const std::vector<float>& values, juce::Rectangle<float> box)
    {
        juce::Path wave;

        for (size_t i = 0; i < values.size(); ++i)
        {
            const auto along = values.size() > 1 ? (float) i / (float) (values.size() - 1) : 0.0f;
            const juce::Point<float> point (box.getX() + along * box.getWidth(), box.getCentreY() - 0.45f * values[i] * box.getHeight());

            if (i == 0)
                wave.startNewSubPath (point);
            else
                wave.lineTo (point);
        }

        return wave;
    }

    void setDropping (bool now)
    {
        dropping = now;
        repaint();
    }

    void setHovered (bool isHovered)
    {
        if (isHovered != hovered)
        {
            hovered = isHovered;
            repaint();
        }
    }

    // Where a frame is drawn: the first at the front, at the bottom left, and the last at the back, at the top right.
    juce::Rectangle<float> getFrameBox (float depth) const
    {
        const auto area = getLocalBounds().toFloat().reduced ((float) Space::s);
        const auto width = area.getWidth() * (1.0f - WavetableTint::shift);
        const auto height = area.getHeight() * (1.0f - WavetableTint::rise);
        return { area.getX() + depth * WavetableTint::shift * area.getWidth(),
                 area.getY() + (1.0f - depth) * WavetableTint::rise * area.getHeight(), width, height };
    }

    // A frame's wave, or a mix of two, across a box.
    static juce::Path makeWave (std::span<const float> from, std::span<const float> to, float morph, juce::Rectangle<float> box)
    {
        juce::Path wave;

        for (int i = 0; i <= pointsPerWave; ++i)
        {
            const auto along = (float) i / (float) pointsPerWave;
            const auto read = [along] (std::span<const float> samples)
            { return samples.empty() ? 0.0f : samples[std::min (samples.size() - 1, (size_t) (along * (float) samples.size()))]; };
            const auto value = read (from) + morph * (read (to) - read (from));
            const juce::Point<float> point (box.getX() + along * box.getWidth(), box.getCentreY() - 0.45f * value * box.getHeight());

            if (i == 0)
                wave.startNewSubPath (point);
            else
                wave.lineTo (point);
        }

        return wave;
    }

    void rebuild()
    {
        frames.clear();

        if (table == nullptr || getWidth() <= 0)
            return;

        const auto total = table->getNumFrames();
        const auto shown = std::min (total, WavetableTint::framesDrawn);

        for (int k = 0; k < shown; ++k)
        {
            const auto frame = shown > 1 ? juce::roundToInt ((double) k * (total - 1) / (shown - 1)) : 0;
            const auto depth = total > 1 ? (float) frame / (float) (total - 1) : 0.0f;
            const auto samples = table->getFrameForDrawing (frame);
            frames.push_back (makeWave (samples, samples, 0.0f, getFrameBox (depth)));
        }
    }
};

//==============================================================================
// An oscillator's picture: the button that names its table, over the drawing of it.
class SynthEditor::Oscillator : public juce::Component
{
public:
    Oscillator (SynthEditor& editor, int osc)
        : view (editor, osc, editor.getDevice().getParameter (getParameterID (osc, Synth::oscPosition)))
    {
        setTitle (getOscillatorName (osc) + " table");
        button.setTitle (getOscillatorName (osc) + " Wavetable Choice");
        button.setTooltip ("Chooses the table this oscillator plays: one that comes with Phazite, or a sound file of yours");
        button.onClick = [&editor, osc, this]
        { editor.getTableMenu (osc).showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&button)); };
        addAndMakeVisible (button);
        addAndMakeVisible (view);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        button.setBounds (area.removeFromTop (controlHeight));
        area.removeFromTop (Space::xs);
        view.setBounds (area);
    }

    juce::TextButton button;
    TableView view;
};

//==============================================================================
// The modulation matrix: each slot a row, its source and target picked from lists, and its amount a knob.
class SynthEditor::Matrix : public juce::Component,
                            private DisplayTicker::Display
{
public:
    explicit Matrix (const DeviceHandle& device)
        : DisplayTicker::Display (static_cast<juce::Component&> (*this))
    {
        setTitle ("Modulation matrix");

        for (int slot = 0; slot < Synth::numModSlots; ++slot)
        {
            auto& row = rows.emplace_back (std::make_unique<Row>());
            const auto id = [slot] (Synth::ModField field) { return getParameterID (Synth::getModParameter (slot, field)); };
            const auto& amount = Synth::info.parameters[(size_t) Synth::getModParameter (slot, Synth::modAmount)];

            row->source.setTitle ("Mod " + juce::String (slot + 1) + " Source");
            row->target.setTitle ("Mod " + juce::String (slot + 1) + " Target");
            row->sourceAttachment.setParameter (device.getParameter (id (Synth::modSource)));
            row->targetAttachment.setParameter (device.getParameter (id (Synth::modTarget)));

            // A slot can't move its own amount: shown in its list, but not to be chosen (the item IDs are the choices from 1).
            row->target.setItemEnabled ((int) Synth::getAmountTarget (slot) + 1, false);
            row->amount = std::make_unique<Knob> (juce::String (amount.name.data(), amount.name.size()), amount.defaultValue);
            showAsParameter (*row->amount, amount);
            row->amountAttachment = std::make_unique<ParameterAttachment> (*row->amount);
            row->amountAttachment->setParameter (device.getParameter (amount.id));
            row->number.setText (juce::String (slot + 1), juce::dontSendNotification);
            row->number.setFont (juce::FontOptions (TextSize::small));
            setThemeColour (row->number, juce::Label::textColourId, &Palette::textMuted);

            for (auto* control : std::initializer_list<juce::Component*> { &row->number, &row->source, &row->target, row->amount.get() })
                addAndMakeVisible (control);
        }

        tick();
    }

    juce::Component* findControl (std::string_view parameterID)
    {
        for (int slot = 0; slot < Synth::numModSlots; ++slot)
        {
            auto& row = *rows[(size_t) slot];

            if (parameterID == getParameterID (Synth::getModParameter (slot, Synth::modSource)))
                return &row.source;

            if (parameterID == getParameterID (Synth::getModParameter (slot, Synth::modTarget)))
                return &row.target;

            if (parameterID == getParameterID (Synth::getModParameter (slot, Synth::modAmount)))
                return row.amount.get();
        }

        return nullptr;
    }

    static constexpr int rowHeight = 24;
    static int getPreferredHeight() { return Synth::numModSlots * rowHeight; }
    static int getPreferredWidth() { return 16 + 2 * 96 + rowHeight + 2 * Space::xs; }

    void resized() override
    {
        auto area = getLocalBounds();

        for (auto& row : rows)
        {
            auto line = area.removeFromTop (rowHeight).reduced (0, 1);
            row->number.setBounds (line.removeFromLeft (16));
            row->amount->setBounds (line.removeFromRight (line.getHeight()));
            line.removeFromRight (Space::xs);
            const auto half = (line.getWidth() - Space::xs) / 2;
            row->source.setBounds (line.removeFromLeft (half));
            line.removeFromLeft (Space::xs);
            row->target.setBounds (line);
        }
    }

private:
    struct Row
    {
        juce::Label number;
        juce::ComboBox source, target;
        ChoiceAttachment sourceAttachment { source }, targetAttachment { target };
        std::unique_ptr<Knob> amount;
        std::unique_ptr<ParameterAttachment> amountAttachment;
    };

    std::vector<std::unique_ptr<Row>> rows;

    void tick() override
    {
        for (auto& row : rows)
        {
            row->sourceAttachment.update();
            row->targetAttachment.update();
            row->amountAttachment->update();
        }
    }
};

//==============================================================================
SynthEditor::SynthEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle),
      DisplayTicker::Display (static_cast<juce::Component&> (*this))
{
    const auto parameter = [this] (int index) { return device.getParameter (getParameterID (index)); };
    const auto shown = [this]() -> const Synth::Shown*
    {
        auto* synth = device.getProcessor<Synth>();
        return synth != nullptr ? &synth->getShown() : nullptr;
    };

    // The oscillators: each its table, then its controls.
    std::vector<DeviceLayout::Part> oscillatorParts;

    for (int osc = 0; osc < numOscillators; ++osc)
    {
        auto& picture = keep (std::make_unique<Oscillator> (*this, osc));
        views[(size_t) osc] = &picture.view;
        tableButtons[(size_t) osc] = &picture.button;

        std::vector<std::string_view> ids;

        for (const auto field :
             { Synth::oscPosition, Synth::oscPitch, Synth::oscFine, Synth::oscLevel, Synth::oscWarp, Synth::oscWarpAmount })
            ids.push_back (getParameterID (osc, field));

        // Its switch at its title's right end, as every part that switches on and off has.
        auto part = makePicturePart (getOscillatorName (osc), picture, WavetableTint::height, WavetableTint::minimumHeight, ids, 3,
                                     ParameterControls::Size::compact, { "Position", "Pitch", "Fine", "Level", "Warp", "Amount" });
        part.titleSwitch = &makePartSwitch (getParameterID (osc, Synth::oscOn), *part.content);
        oscillatorParts.push_back (part);
    }

    const auto subAndNoise = makeControlsPart ("Sub and Noise", { "subLevel", "subOctave", "subShape", "noiseLevel" }, 4,
                                               ParameterControls::Size::compact, { "Sub", "Octave", "Shape", "Noise" });

    // The unison: how many copies, how far apart in pitch and across the stereo field; then where they stack, how far each
    // wanders, and how far apart in the table: the two lists one under the other.
    const auto unison =
        makeControlsPart ("Unison", { "unisonVoices", "unisonDetune", "unisonSpread", "unisonStack", "unisonDrift", "unisonPosition" }, 3,
                          ParameterControls::Size::compact, { "Voices", "Detune", "Spread", "Stack", "Drift", "Position" });

    // The filter: its response, then its controls.
    auto all = std::make_shared<std::vector<ParameterHandle>>();

    for (const auto& each : Synth::info.parameters)
        all->push_back (device.getParameter (each.id));

    auto& filterView = keep (std::make_unique<FilterView> (
        "Filter response", parameter (Synth::cutoff), parameter (Synth::resonance),
        [all]
        {
            std::vector<float> values;

            for (const auto& each : *all)
                values.push_back (each.getValue());

            return values;
        },
        [this] (const std::vector<float>& values, double cutoffHertz, double frequency)
        {
            auto* scope = device.getScope();
            return Synth::getFilterMagnitude (values, cutoffHertz, frequency, scope != nullptr ? scope->getSampleRate() : 48000.0);
        },
        [shown] { return shown() != nullptr ? shown()->cutoff.get() : 0.0f; }));
    filter = &filterView;
    filterView.setReadoutWord (
        [] (const std::vector<float>& values)
        { return values.size() > (size_t) Synth::quality && Synth::isDriven (values) ? juce::String ("Driven") : juce::String(); });

    // Its controls, the key's tracking with the cutoff and resonance it moves, then the slope and the drive, and after the
    // drive its Character.
    auto& filterControls =
        makeControls ({ "filterType", "cutoff", "resonance", "keyTracking", "filterSlope", "drive" }, 4, ParameterControls::Size::compact,
                      "Filter", { "Type", "Cutoff", "Resonance", "Key", "Slope", "Drive" });
    auto& characters = keep (std::make_unique<ChoiceButtons> (
        device, "filterCharacter", ChoiceButtons::Direction::across,
        std::vector<juce::String> {
            "Clean: the filter is linear, and the drive saturates what goes into it, giving back the gain it adds",
            "Driven: the drive pushes the sound into the filter's loop, where its resonance saturates: louder and thicker as it is "
            "driven, and singing on its own at the top of the Resonance (low-pass, high-pass, band-pass and notch)" }));
    addControl ("filterCharacter", characters);
    auto& shownFilter = keep (std::make_unique<FilterPart> (filterView, filterControls, characters));
    DeviceLayout::Part filterPart {
        "Filter", &shownFilter, shownFilter.getPreferredWidth(), 80 + shownFilter.getBelowPicture(), 40 + shownFilter.getBelowPicture(),
        true
    };
    filterPart.titleSwitch = &makePartSwitch ("filterOn", shownFilter);

    // The modulation: the envelopes, the LFOs, the matrix and how it plays.
    std::vector<DeviceLayout::Part> modulation;

    for (int envelope = 0; envelope < 3; ++envelope)
    {
        const auto field = [envelope] (Synth::EnvelopeField f) { return Synth::getEnvelopeParameter (envelope, f); };
        auto& view = keep (std::make_unique<EnvelopeView> (
            "Envelope " + juce::String (envelope + 1), parameter (field (Synth::envAttack)), parameter (field (Synth::envDecay)),
            parameter (field (Synth::envSustain)), parameter (field (Synth::envRelease)),
            [shown, envelope] { return shown() != nullptr ? shown()->envelopes[(size_t) envelope].get() : 0.0f; }));
        view.setCurves (parameter (field (Synth::envAttackCurve)), parameter (field (Synth::envDecayCurve)),
                        parameter (field (Synth::envReleaseCurve)));
        envelopes[(size_t) envelope] = &view;
        const auto name = envelope == 0 ? juce::String ("Amp") : "Envelope " + juce::String (envelope + 1);
        modulation.push_back (makePicturePart (
            name, view, 64, 36,
            { getParameterID (field (Synth::envAttack)), getParameterID (field (Synth::envDecay)),
              getParameterID (field (Synth::envSustain)), getParameterID (field (Synth::envRelease)),
              getParameterID (field (Synth::envAttackCurve)), getParameterID (field (Synth::envDecayCurve)),
              getParameterID (field (Synth::envReleaseCurve)) },
            4, ParameterControls::Size::compact, { "Attack", "Decay", "Sustain", "Release", "A Curve", "D Curve", "R Curve" }));
    }

    for (int lfo = 0; lfo < 2; ++lfo)
    {
        auto lfoShape = std::make_shared<ParameterHandle> (parameter (Synth::getLfoParameter (lfo, Synth::lfoShape)));
        auto& view = keep (std::make_unique<LfoView> (
            "LFO " + juce::String (lfo + 1),
            [lfoShape] (float phase) { return getDrawnLfoValue (juce::roundToInt (lfoShape->getValue()), phase); },
            [shown, lfo] { return shown() != nullptr ? shown()->lfoPhases[(size_t) lfo].get() : -1.0f; }));

        // The wheel for its rate, or its note value while synced; it has no depth of its own (the matrix's slots have).
        view.setControls ({}, parameter (Synth::getLfoParameter (lfo, Synth::lfoRate)),
                          parameter (Synth::getLfoParameter (lfo, Synth::lfoSync)),
                          parameter (Synth::getLfoParameter (lfo, Synth::lfoDivision)));

        std::vector<std::string_view> ids;

        for (const auto field :
             { Synth::lfoShape, Synth::lfoRate, Synth::lfoSync, Synth::lfoDivision, Synth::lfoRetrigger, Synth::lfoFadeIn })
            ids.push_back (getParameterID (Synth::getLfoParameter (lfo, field)));

        modulation.push_back (makePicturePart ("LFO " + juce::String (lfo + 1), view, 64, 36, ids, 3, ParameterControls::Size::compact,
                                               { "Shape", "Rate", "Sync", "Division", "Retrigger", "Fade In" }));
    }

    auto& matrix = keep (std::make_unique<Matrix> (device));

    for (int slot = 0; slot < Synth::numModSlots; ++slot)
        for (const auto field : { Synth::modSource, Synth::modTarget, Synth::modAmount })
            if (auto* control = matrix.findControl (getParameterID (Synth::getModParameter (slot, field))))
                addControl (getParameterID (Synth::getModParameter (slot, field)), *control);

    modulation.push_back ({ "Matrix", &matrix, Matrix::getPreferredWidth(), Matrix::getPreferredHeight(), Matrix::getPreferredHeight() });
    modulation.push_back (makeControlsPart (
        "Voice", { "voiceMode", "glide", "glideMode", "phase", "bendRange", "velocity", "level", "quality" }, 8,
        ParameterControls::Size::compact, { "Mode", "Glide", "Glide Mode", "Phase", "Bend", "Velocity", "Level", "Quality" }));

    if (auto* phase = dynamic_cast<juce::SettableTooltipClient*> (findControl ("phase")))
        phase->setTooltip ("Reset starts every oscillator at the start of its wave at each note, so that a note sounds the same every "
                           "time; Free lets the oscillators run on from where they were");

    // A part to a column, as tall as the dock is short, so that only the list has tabs: in the dock, side by side on each
    // page: the oscillators; the filter and the amp's envelope; the sub and noise over how it plays (a row of seven, with its
    // glide's mode and its phase, as wide as a page); then the unison and the rest of the modulation, the list down its side.
    const auto amp = modulation.front(), voice = modulation.back();
    const std::vector<DeviceLayout::Part> rest (modulation.begin() + 1, modulation.end() - 1);
    setParts ({ { oscillatorParts[0] }, { oscillatorParts[1] }, { filterPart }, { amp }, { subAndNoise, voice }, { unison }, rest }, { 6 });
    getLayout().setColumnName (6, "Modulation");
    refresh();
}

SynthEditor::~SynthEditor() = default;

juce::Component& SynthEditor::getTableView (int osc) const
{
    return *views[(size_t) std::clamp (osc, 0, numOscillators - 1)];
}

juce::Button& SynthEditor::getTableButton (int osc) const
{
    return *tableButtons[(size_t) std::clamp (osc, 0, numOscillators - 1)];
}

juce::PopupMenu SynthEditor::getTableMenu (int osc)
{
    juce::PopupMenu menu;
    const auto current = device.getWavetableName (osc);
    const auto fromFile = device.hasWavetableFile (osc);
    const juce::Component::SafePointer<SynthEditor> safe (this);

    // A sound file of the user's, ticked while it plays.
    if (fromFile)
    {
        menu.addItem (current, false, true, nullptr);
        menu.addSeparator();
    }

    const auto names = devices::getFactoryWavetableNames();

    for (int index = 0; index < (int) names.size(); ++index)
    {
        const juce::String name (names[(size_t) index].data(), names[(size_t) index].size());
        menu.addItem (name, true, ! fromFile && name == current,
                      [safe, osc, index]
                      {
                          if (safe != nullptr)
                          {
                              safe->device.setFactoryWavetable (osc, index);
                              safe->refresh();
                          }
                      });
    }

    menu.addSeparator();
    menu.addItem ("Load Wavetable File...",
                  [safe, osc]
                  {
                      if (safe != nullptr)
                          safe->chooseFile (osc);
                  });
    return menu;
}

bool SynthEditor::dropFile (int osc, const juce::File& file)
{
    const auto put = device.setWavetableFile (osc, file);
    refresh();
    return put;
}

void SynthEditor::chooseFile (int osc)
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a wavetable for " + getOscillatorName (osc), juce::File(),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<SynthEditor> (this), osc] (const juce::FileChooser& chosen)
                          {
                              if (safe != nullptr && chosen.getResult() != juce::File())
                                  safe->dropFile (osc, chosen.getResult());
                          });
}

void SynthEditor::refresh()
{
    auto* synth = device.getProcessor<Synth>();

    for (int osc = 0; osc < numOscillators; ++osc)
    {
        const auto name = device.getWavetableName (osc);
        tableButtons[(size_t) osc]->setButtonText (name);
        views[(size_t) osc]->setTable (device.getWavetable (osc), name);

        const auto on = device.getParameter (getParameterID (osc, Synth::oscOn)).getValue() > 0.5f;
        const auto position =
            synth != nullptr ? synth->getPlayedPosition (osc) : device.getParameter (getParameterID (osc, Synth::oscPosition)).getValue();

        // Its warp at the knob's amount, or while a note plays at the amount the newest note plays it at (its modulation's).
        const auto warp = (Synth::Warp) juce::roundToInt (device.getParameter (getParameterID (osc, Synth::oscWarp)).getValue());
        const auto amount = synth != nullptr && synth->getActiveVoices() > 0
                                ? synth->getShown().warpAmounts[(size_t) osc].get()
                                : device.getParameter (getParameterID (osc, Synth::oscWarpAmount)).getValue();
        views[(size_t) osc]->setPlayed (position, on, warp, amount);
    }

    showOwnModulation();
}

std::vector<float> SynthEditor::getPlayedWave (int osc) const
{
    return views[(size_t) std::clamp (osc, 0, numOscillators - 1)]->getPlayedWave();
}

juce::String SynthEditor::getTableReadout (int osc) const
{
    return views[(size_t) std::clamp (osc, 0, numOscillators - 1)]->getReadout();
}

namespace
{
    // Where a slot's source moves, at its least and most: the envelopes, velocity, the mod wheel and Random from 0 to 1, the
    // LFOs and Drift from -1 to 1, the note from the lowest key to the highest (an octave each side of middle C for each
    // 0.25).
    std::pair<float, float> getSourceRange (Synth::ModSource source)
    {
        switch (source)
        {
            case Synth::ModSource::envelope2:
            case Synth::ModSource::envelope3:
            case Synth::ModSource::velocity:
            case Synth::ModSource::modWheel:
            case Synth::ModSource::random:
                return { 0.0f, 1.0f };
            case Synth::ModSource::lfo1:
            case Synth::ModSource::lfo2:
            case Synth::ModSource::drift:
                return { -1.0f, 1.0f };
            case Synth::ModSource::note:
                return { -60.0f / 48.0f, 67.0f / 48.0f };
            case Synth::ModSource::none:
                break;
        }

        return { 0.0f, 0.0f };
    }

    // How a slot's move shows on a target's knob: added (a part of the knob's range: positions, levels, amounts), as
    // semitones (24 for 100 %), as an octave's twelfths of the cutoff (96 semitones for 100 %), as cents (100 for 100 %),
    // or as the volume's gain, on the Level knob in decibels. As Synth::render() moves each.
    enum class Moves
    {
        plainly,
        semitones,
        cutoff,
        cents,
        volume
    };

    struct RingTarget
    {
        Synth::ModTarget target;
        std::string_view parameterID;
        Moves moves;
    };

    // Every target with a knob of its own (Pan has none); Pitch moves both oscillators' Pitch knobs, with theirs.
    constexpr RingTarget ringTargets[] = {
        { Synth::ModTarget::osc1Position, "osc1Position", Moves::plainly },
        { Synth::ModTarget::osc1Warp, "osc1WarpAmount", Moves::plainly },
        { Synth::ModTarget::osc1Pitch, "osc1Pitch", Moves::semitones },
        { Synth::ModTarget::osc1Level, "osc1Level", Moves::plainly },
        { Synth::ModTarget::osc2Position, "osc2Position", Moves::plainly },
        { Synth::ModTarget::osc2Warp, "osc2WarpAmount", Moves::plainly },
        { Synth::ModTarget::osc2Pitch, "osc2Pitch", Moves::semitones },
        { Synth::ModTarget::osc2Level, "osc2Level", Moves::plainly },
        { Synth::ModTarget::subLevel, "subLevel", Moves::plainly },
        { Synth::ModTarget::noiseLevel, "noiseLevel", Moves::plainly },
        { Synth::ModTarget::cutoff, "cutoff", Moves::cutoff },
        { Synth::ModTarget::resonance, "resonance", Moves::plainly },
        { Synth::ModTarget::volume, "level", Moves::volume },
        { Synth::ModTarget::unisonDetune, "unisonDetune", Moves::cents },
        { Synth::ModTarget::slot1Amount, "mod1Amount", Moves::plainly },
        { Synth::ModTarget::slot2Amount, "mod2Amount", Moves::plainly },
        { Synth::ModTarget::slot3Amount, "mod3Amount", Moves::plainly },
        { Synth::ModTarget::slot4Amount, "mod4Amount", Moves::plainly },
        { Synth::ModTarget::slot5Amount, "mod5Amount", Moves::plainly },
        { Synth::ModTarget::slot6Amount, "mod6Amount", Moves::plainly },
        { Synth::ModTarget::slot7Amount, "mod7Amount", Moves::plainly },
        { Synth::ModTarget::slot8Amount, "mod8Amount", Moves::plainly },
    };

    // A knob's value moved by a slot's move.
    double getMovedValue (Moves moves, double value, float move, double lowest)
    {
        switch (moves)
        {
            case Moves::plainly:
                return value + move;
            case Moves::semitones:
                return value + 24.0 * move;
            case Moves::cutoff:
                return value * std::exp2 (8.0 * move);
            case Moves::cents:
                return value + 100.0 * move;
            case Moves::volume:
            {
                const auto gain = std::clamp (1.0 + move, 0.0, 2.0);
                return gain > 0.0 ? value + 20.0 * std::log10 (gain) : lowest;
            }
        }

        return value;
    }
} // namespace

void SynthEditor::showOwnModulation()
{
    constexpr auto numTargets = (size_t) Synth::ModTarget::slot8Amount + 1;
    constexpr auto numFields = 3; // a slot's source, target and amount
    auto* synth = device.getProcessor<Synth>();
    const auto playing = synth != nullptr && synth->getActiveVoices() > 0;

    // What the rings are worked out from: the slots, the knobs' values, and what the synth shows of the newest note. Nothing
    // moved since the last tick: nothing to do.
    if (ringKnobs.empty())
    {
        for (const auto& each : ringTargets)
            ringKnobs.push_back (dynamic_cast<Knob*> (findControl (each.parameterID)));

        for (int slot = 0; slot < Synth::numModSlots; ++slot)
            for (const auto field : { Synth::modSource, Synth::modTarget, Synth::modAmount })
                slotParameters.push_back (device.getParameter (getParameterID (Synth::getModParameter (slot, field))));

        ringInputs.resize (slotParameters.size() + ringKnobs.size() + 7);
    }

    {
        auto input = ringInputs.begin();

        for (const auto& parameter : slotParameters)
            *input++ = parameter.getValue();

        for (const auto* knob : ringKnobs)
            *input++ = knob != nullptr ? (float) knob->getValue() : 0.0f;

        const auto* shown = synth != nullptr ? &synth->getShown() : nullptr;
        *input++ = playing ? 1.0f : 0.0f;
        *input++ = playing ? shown->cutoff.get() : 0.0f;
        *input++ = playing ? shown->cutoffCentre.get() : 0.0f;

        for (int osc = 0; osc < numOscillators; ++osc)
        {
            *input++ = playing ? synth->getPlayedPosition (osc) : 0.0f;
            *input++ = playing ? shown->warpAmounts[(size_t) osc].get() : 0.0f;
        }
    }

    if (ringInputs.size() == lastRingInputs.size()
        && std::equal (ringInputs.begin(), ringInputs.end(), lastRingInputs.begin(),
                       [] (float a, float b) { return juce::exactlyEqual (a, b); }))
        return;

    lastRingInputs = ringInputs;

    // Each slot's amount as the slots that move it can move it (each by its own amount as set, the sum held within -1 to 1,
    // as the synth moves it), and each target's least and most move: the sum over the slots on it of each one's amount over
    // its source's range. The velocity's on the cutoff apart: while a note plays, the ring is about the cutoff they make.
    const auto slotValue = [this] (int slot, int field) { return ringInputs[(size_t) (slot * numFields + field)]; };
    const auto sourceOf = [&slotValue] (int slot) { return (Synth::ModSource) juce::roundToInt (slotValue (slot, 0)); };
    const auto targetOf = [&slotValue] (int slot) { return (size_t) juce::roundToInt (slotValue (slot, 1)); };
    const auto aims = [&] (int slot)
    {
        const auto target = targetOf (slot);
        return sourceOf (slot) != Synth::ModSource::none && target > 0 && target < numTargets
               && target != (size_t) Synth::getAmountTarget (slot); // a slot on its own amount does nothing
    };

    std::array<float, Synth::numModSlots> amountLows {}, amountHighs {};

    for (int slot = 0; slot < Synth::numModSlots; ++slot)
        amountLows[(size_t) slot] = amountHighs[(size_t) slot] = slotValue (slot, 2);

    for (int mover = 0; mover < Synth::numModSlots; ++mover)
    {
        const auto moved = (int) targetOf (mover) - (int) Synth::ModTarget::slot1Amount;

        if (aims (mover) && moved >= 0 && moved < Synth::numModSlots)
        {
            const auto [from, to] = getSourceRange (sourceOf (mover));
            const auto amount = slotValue (mover, 2);
            amountLows[(size_t) moved] += std::min (from * amount, to * amount);
            amountHighs[(size_t) moved] += std::max (from * amount, to * amount);
        }
    }

    std::array<float, numTargets> lows {}, highs {};
    std::array<bool, numTargets> moved {};
    auto velocityLow = 0.0f, velocityHigh = 0.0f; // the velocity's slots' on the cutoff

    for (int slot = 0; slot < Synth::numModSlots; ++slot)
    {
        if (! aims (slot))
            continue;

        const auto target = targetOf (slot);
        const auto [from, to] = getSourceRange (sourceOf (slot));
        const auto movesAmount = target >= (size_t) Synth::ModTarget::slot1Amount;
        const auto low = movesAmount ? slotValue (slot, 2) : std::clamp (amountLows[(size_t) slot], -1.0f, 1.0f);
        const auto high = movesAmount ? slotValue (slot, 2) : std::clamp (amountHighs[(size_t) slot], -1.0f, 1.0f);
        const auto least = std::min ({ from * low, from * high, to * low, to * high });
        const auto most = std::max ({ from * low, from * high, to * low, to * high });
        lows[target] += least;
        highs[target] += most;
        moved[target] = true;

        if (sourceOf (slot) == Synth::ModSource::velocity && target == (size_t) Synth::ModTarget::cutoff)
        {
            velocityLow += least;
            velocityHigh += most;
        }
    }

    // Pitch moves both oscillators.
    for (const auto osc : { Synth::ModTarget::osc1Pitch, Synth::ModTarget::osc2Pitch })
    {
        lows[(size_t) osc] += lows[(size_t) Synth::ModTarget::pitch];
        highs[(size_t) osc] += highs[(size_t) Synth::ModTarget::pitch];
        moved[(size_t) osc] = moved[(size_t) osc] || moved[(size_t) Synth::ModTarget::pitch];
    }

    // Where a note plays now, for what the synth shows of it: the cutoff, the positions and the warp amounts.
    const auto shownAt = ringInputs.size() - 7;
    const auto nowOf = [this, playing, shownAt] (Synth::ModTarget target) -> std::optional<double>
    {
        if (! playing)
            return std::nullopt;

        if (const auto hertz = ringInputs[shownAt + 1]; target == Synth::ModTarget::cutoff && hertz > 0.0f)
            return hertz;

        for (int osc = 0; osc < numOscillators; ++osc)
        {
            if (target == (osc == 0 ? Synth::ModTarget::osc1Position : Synth::ModTarget::osc2Position))
                return ringInputs[shownAt + 3 + 2 * (size_t) osc];

            if (target == (osc == 0 ? Synth::ModTarget::osc1Warp : Synth::ModTarget::osc2Warp))
                return ringInputs[shownAt + 4 + 2 * (size_t) osc];
        }

        return std::nullopt;
    };

    // While a note plays, the Cutoff's ring is about the cutoff its key tracking and velocity make of the knob's, which
    // hold still for the note, and the velocity's slots aren't in its range.
    const auto centre = ringInputs[shownAt + 2];
    const auto centred = playing && centre > 0.0f;

    if (centred)
    {
        lows[(size_t) Synth::ModTarget::cutoff] -= velocityLow;
        highs[(size_t) Synth::ModTarget::cutoff] -= velocityHigh;
    }

    for (size_t index = 0; index < ringKnobs.size(); ++index)
    {
        auto* knob = ringKnobs[index];
        const auto& [target, parameterID, moves] = ringTargets[index];

        if (knob == nullptr)
            continue;

        if (! moved[(size_t) target])
        {
            knob->setOwnModulation (std::nullopt);
            continue;
        }

        const auto lowest = knob->getMinimum(), highest = knob->getMaximum();
        const auto proportionOf = [knob, lowest, highest] (double value)
        { return (float) knob->valueToProportionOfLength (std::clamp (value, lowest, highest)); };
        const auto value = target == Synth::ModTarget::cutoff && centred ? (double) centre : knob->getValue();
        const auto now = nowOf (target);
        knob->setOwnModulation (ModulationRange { .low = proportionOf (getMovedValue (moves, value, lows[(size_t) target], lowest)),
                                                  .high = proportionOf (getMovedValue (moves, value, highs[(size_t) target], lowest)),
                                                  .now = now.has_value() ? proportionOf (*now) : 0.0f,
                                                  .isOn = now.has_value() });
    }
}
} // namespace phazite::ui
