// SPDX-License-Identifier: AGPL-3.0-or-later
#include "WavetableEditor.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "devices/wavetable/WavetableSynth.h"
#include "ui/input/LibraryDrag.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>

namespace phazite::ui
{
namespace
{
    using devices::WavetableSynth;

    constexpr int refreshRateHz = 15;
    constexpr int pointsPerWave = 72; // where each frame's wave is drawn through

    std::string_view getParameterID (int osc, WavetableSynth::OscillatorField field)
    {
        return WavetableSynth::info.parameters[(size_t) WavetableSynth::getParameter (osc, field)].id;
    }

    juce::String getOscillatorName (int osc)
    {
        return "Osc " + juce::String (osc + 1);
    }

    std::vector<std::vector<DevicePanel::Group>> getOscillatorLayout (int osc)
    {
        std::vector<std::string_view> ids;

        for (const auto field : { WavetableSynth::oscOn, WavetableSynth::oscPosition, WavetableSynth::oscPitch, WavetableSynth::oscFine,
                                  WavetableSynth::oscLevel, WavetableSynth::oscWarp, WavetableSynth::oscWarpAmount })
            ids.push_back (getParameterID (osc, field));

        return { { { getOscillatorName (osc), ids } } };
    }

    // The rest of the synth's controls, in the order the sound goes through them; the modulation slots last.
    std::vector<std::vector<DevicePanel::Group>> getLayout()
    {
        std::vector<std::vector<DevicePanel::Group>> rows {
            { { "Sub", { "subLevel", "subOctave", "subShape" } },
              { "Noise", { "noiseLevel" } },
              { "Unison", { "unisonVoices", "unisonDetune", "unisonSpread", "unisonPosition" } },
              { "Filter", { "filterOn", "filterType", "cutoff", "resonance", "filterSlope", "drive", "keyTracking" } } },
            { { "Envelope 1", { "env1Attack", "env1Decay", "env1Sustain", "env1Release" } },
              { "Envelope 2", { "env2Attack", "env2Decay", "env2Sustain", "env2Release" } },
              { "Envelope 3", { "env3Attack", "env3Decay", "env3Sustain", "env3Release" } } },
            { { "LFO 1", { "lfo1Shape", "lfo1Rate", "lfo1Sync", "lfo1Division", "lfo1Retrigger" } },
              { "LFO 2", { "lfo2Shape", "lfo2Rate", "lfo2Sync", "lfo2Division", "lfo2Retrigger" } },
              { "Play", { "voiceMode", "glide", "bendRange", "velocity", "level" } } },
        };

        // The modulation slots, four to a row.
        for (int slot = 0; slot < WavetableSynth::numModSlots; ++slot)
        {
            if (slot % 4 == 0)
                rows.emplace_back();

            std::vector<std::string_view> ids;

            for (const auto field : { WavetableSynth::modSource, WavetableSynth::modTarget, WavetableSynth::modAmount })
                ids.push_back (WavetableSynth::info.parameters[(size_t) WavetableSynth::getModParameter (slot, field)].id);

            rows.back().push_back ({ "Mod " + juce::String (slot + 1), ids });
        }

        return rows;
    }
} // namespace

//==============================================================================
class WavetableEditor::TableView : public juce::Component,
                                   public juce::SettableTooltipClient,
                                   public juce::DragAndDropTarget,
                                   public juce::FileDragAndDropTarget
{
public:
    TableView (WavetableEditor& e, int index, ParameterHandle positionHandle)
        : editor (e),
          osc (index),
          position (std::move (positionHandle))
    {
        setTitle (getOscillatorName (osc) + " Wavetable");
        setTooltip ("Drag across it to move through the table; drop a sound on it to make that the table");
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

    void setPlayed (float newPosition, bool isOn)
    {
        if (std::abs (newPosition - played) > 0.0005f || isOn != on)
        {
            played = newPosition;
            on = isOn;
            repaint();
        }
    }

    float getPlayedPosition() const noexcept { return played; }
    int getNumFramesDrawn() const noexcept { return (int) frames.size(); }

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

        // The one it plays.
        if (table != nullptr)
        {
            const auto place = std::clamp (played, 0.0f, 1.0f) * (float) (table->getNumFrames() - 1);
            const auto first = std::min ((int) place, table->getNumFrames() - 1);
            const auto second = std::min (first + 1, table->getNumFrames() - 1);
            g.setColour (palette.accent.withAlpha (alpha));
            g.strokePath (makeWave (table->getFrameForDrawing (first), table->getFrameForDrawing (second), place - (float) first,
                                    getFrameBox (std::clamp (played, 0.0f, 1.0f))),
                          juce::PathStrokeType (WavetableTint::playedWidth));
        }
    }

    void resized() override { rebuild(); }

    // A drag across it moves the oscillator's position: the width of the drawing, right or up, is the whole table.
    void mouseDown (const juce::MouseEvent&) override
    {
        position.beginGesture();
        dragStart = position.getValue();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto moved = (float) (e.getDistanceFromDragStartX() - e.getDistanceFromDragStartY()) / (float) std::max (1, getWidth());
        position.setValue (std::clamp (dragStart + moved, 0.0f, 1.0f));
    }

    void mouseUp (const juce::MouseEvent&) override { position.endGesture(); }

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
    WavetableEditor& editor;
    const int osc;
    ParameterHandle position;
    std::shared_ptr<const devices::Wavetable> table;
    std::vector<juce::Path> frames; // those drawn, from the front
    float played = 0.0f, dragStart = 0.0f;
    bool on = true, dropping = false;

    void setDropping (bool now)
    {
        dropping = now;
        repaint();
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
WavetableEditor::WavetableEditor (const DeviceHandle& handle)
    : device (handle)
{
    for (int osc = 0; osc < numOscillators; ++osc)
    {
        auto& view = views[(size_t) osc];
        view = std::make_unique<TableView> (*this, osc, device.getParameter (getParameterID (osc, WavetableSynth::oscPosition)));
        addAndMakeVisible (*view);

        auto& button = tableButtons[(size_t) osc];
        button = std::make_unique<juce::TextButton>();
        button->setTitle (getOscillatorName (osc) + " Wavetable Choice");
        button->setTooltip ("Chooses the table this oscillator plays: one that comes with Phazite, or a sound file of yours");
        button->onClick = [this, osc]
        { getTableMenu (osc).showMenuAsync (juce::PopupMenu::Options().withTargetComponent (tableButtons[(size_t) osc].get())); };
        addAndMakeVisible (*button);

        oscillatorControls[(size_t) osc] = std::make_unique<DevicePanel> (device, getOscillatorLayout (osc));
        addAndMakeVisible (*oscillatorControls[(size_t) osc]);
    }

    controls = std::make_unique<DevicePanel> (device, getLayout());
    addAndMakeVisible (*controls);

    refresh();
    setSize (std::max (getColumnWidth (0) + getColumnWidth (1), controls->getWidth()),
             getTopHeight (WavetableTint::height) + controls->getHeight());
    startTimerHz (refreshRateHz);
}

WavetableEditor::~WavetableEditor()
{
    stopTimer();
}

juce::Component& WavetableEditor::getTableView (int osc) const
{
    return *views[(size_t) std::clamp (osc, 0, numOscillators - 1)];
}

juce::Button& WavetableEditor::getTableButton (int osc) const
{
    return *tableButtons[(size_t) std::clamp (osc, 0, numOscillators - 1)];
}

DevicePanel& WavetableEditor::getOscillatorControls (int osc) const
{
    return *oscillatorControls[(size_t) std::clamp (osc, 0, numOscillators - 1)];
}

juce::PopupMenu WavetableEditor::getTableMenu (int osc)
{
    juce::PopupMenu menu;
    const auto current = device.getWavetableName (osc);
    const auto fromFile = device.hasWavetableFile (osc);
    const juce::Component::SafePointer<WavetableEditor> safe (this);

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

bool WavetableEditor::dropFile (int osc, const juce::File& file)
{
    const auto put = device.setWavetableFile (osc, file);
    refresh();
    return put;
}

void WavetableEditor::chooseFile (int osc)
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a wavetable for " + getOscillatorName (osc), juce::File(),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<WavetableEditor> (this), osc] (const juce::FileChooser& chosen)
                          {
                              if (safe != nullptr && chosen.getResult() != juce::File())
                                  safe->dropFile (osc, chosen.getResult());
                          });
}

void WavetableEditor::refresh()
{
    auto* synth = device.getProcessor<WavetableSynth>();

    for (int osc = 0; osc < numOscillators; ++osc)
    {
        const auto name = device.getWavetableName (osc);
        tableButtons[(size_t) osc]->setButtonText (name);
        views[(size_t) osc]->setTable (device.getWavetable (osc), name);

        const auto on = device.getParameter (getParameterID (osc, WavetableSynth::oscOn)).getValue() > 0.5f;
        const auto position = synth != nullptr ? synth->getPlayedPosition (osc)
                                               : device.getParameter (getParameterID (osc, WavetableSynth::oscPosition)).getValue();
        views[(size_t) osc]->setPlayed (position, on);
    }
}

void WavetableEditor::timerCallback()
{
    if (isShowing()) // nothing to show behind another tab of the Devices view
        refresh();
}

int WavetableEditor::getColumnWidth (int osc) const
{
    return oscillatorControls[(size_t) osc]->getWidth();
}

int WavetableEditor::getTopHeight (int drawingHeight) const
{
    return Space::l + controlHeight + Space::s + drawingHeight + oscillatorControls[0]->getHeight();
}

void WavetableEditor::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void WavetableEditor::resized()
{
    // The drawings at their full height while the editor has room, and else as short as they go.
    const auto full = getTopHeight (WavetableTint::height) + controls->getMinimumHeight() <= getHeight();
    const auto drawingHeight = full ? WavetableTint::height : WavetableTint::minimumHeight;
    auto x = 0;

    for (int osc = 0; osc < numOscillators; ++osc)
    {
        const auto width = getColumnWidth (osc);
        auto column = juce::Rectangle<int> (x, 0, width, getTopHeight (drawingHeight)).reduced (Space::l, 0);
        column.removeFromTop (Space::l);
        tableButtons[(size_t) osc]->setBounds (column.removeFromTop (controlHeight));
        column.removeFromTop (Space::s);
        views[(size_t) osc]->setBounds (column.removeFromTop (drawingHeight));
        oscillatorControls[(size_t) osc]->setBounds (x, column.getY(), width, oscillatorControls[(size_t) osc]->getHeight());
        x += width;
    }

    const auto top = getTopHeight (drawingHeight);
    controls->setBounds (0, top, getWidth(), std::max (0, getHeight() - top));
}

int WavetableEditor::getMinimumHeight() const
{
    return getTopHeight (WavetableTint::minimumHeight) + controls->getMinimumHeight();
}

int WavetableEditor::getWidthForHeight (int height) const
{
    const auto full = getTopHeight (WavetableTint::height) + controls->getMinimumHeight() <= height;
    const auto top = getTopHeight (full ? WavetableTint::height : WavetableTint::minimumHeight);
    return std::max (getColumnWidth (0) + getColumnWidth (1), controls->getWidthForHeight (height - top));
}
} // namespace phazite::ui
