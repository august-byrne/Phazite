// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SamplerEditor.h"
#include "DeviceTint.h"
#include "devices/sampler/FactorySamples.h"
#include "devices/sampler/SampleAnalysis.h"
#include "devices/sampler/Sampler.h"
#include "ui/input/LibraryDrag.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace phazite::ui
{
namespace
{
    using devices::Sampler;

    constexpr int sliceStripHeight = 14;   // under the drawing, in Slice mode: a click there adds a slice
    constexpr int smallestWaveHeight = 56; // the drawing's least, and the height it wants
    constexpr int waveHeight = 120;
    constexpr int sampleWidth = 540, smallestSampleWidth = 400; // the Sample part's, wide enough for Slice mode's controls
    constexpr double snapSeconds = 0.01; // how far a marker looks for a zero crossing to snap to: half a cycle of a low 808
    constexpr float flagSize = 7.0f;     // a marker's flag at its top
    constexpr float auditionVelocity = 0.8f;

    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    std::string_view getID (int parameter)
    {
        return Sampler::info.parameters[(size_t) parameter].id;
    }

    // The sounds that come with the sampler in a menu: the 808 bass, then each drum kit's sixteen under the kit's name.
    struct FactoryKit
    {
        juce::String name;
        int first = 0, count = 0;
    };

    std::vector<FactoryKit> getFactoryKits()
    {
        std::vector<FactoryKit> kits;
        const auto names = devices::getFactorySampleNames();

        for (int first = 1; first < (int) names.size(); first += 16)
        {
            const auto kick = toString (names[(size_t) first]);
            const auto prefix = kick.upToLastOccurrenceOf (" ", false, false);
            kits.push_back ({ first == 1 || prefix == kick ? juce::String ("Default Kit") : prefix + " Kit", first,
                              std::min (16, (int) names.size() - first) });
        }

        return kits;
    }
} // namespace

//==============================================================================
// The sound, drawn, with what plays it marked; its markers drag, and it takes a sound dropped on it.
class SamplerEditor::SampleView : public juce::Component,
                                  public juce::DragAndDropTarget,
                                  public juce::FileDragAndDropTarget,
                                  private DisplayTicker::Display
{
public:
    explicit SampleView (SamplerEditor& e)
        : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
          editor (e)
    {
        setTitle ("Sound");
        setDescription ("The sound the sampler plays. Drag its markers to choose what plays and what loops; in Slice mode, click "
                        "under it to add a slice, drag a slice to move it and double-click one to take it away. Press on it to "
                        "hear it. Drop a sound on it to play that instead.");

        for (const auto& each : Sampler::info.parameters)
            handles.push_back (editor.getDevice().getParameter (each.id));

        values.resize (handles.size());
        slicesToClip.setButtonText ("Slices to Clip");
        slicesToClip.setTooltip ("Writes the slices as notes into a new clip on this track, at the play start, so that the clip plays "
                                 "the sound as it was, in time with the song");
        slicesToClip.onClick = [this] { editor.getDevice().writeSliceNotes(); };
        addChildComponent (slicesToClip);
        refresh();
    }

    float value (int parameter) const { return values[(size_t) parameter]; }
    int getMode() const { return juce::roundToInt (value (Sampler::mode)); }

    // The drawing's area, and where a part of the sound is across it.
    juce::Rectangle<float> getWaveArea() const
    {
        auto area = getLocalBounds().toFloat().reduced (2.0f);

        if (getMode() == (int) Sampler::Mode::slice)
            area.removeFromBottom ((float) sliceStripHeight);

        return area;
    }

    float xFor (float fraction) const
    {
        const auto area = getWaveArea();
        return area.getX() + std::clamp (fraction, 0.0f, 1.0f) * area.getWidth();
    }

    float fractionAt (float x) const
    {
        const auto area = getWaveArea();
        return std::clamp ((x - area.getX()) / std::max (1.0f, area.getWidth()), 0.0f, 1.0f);
    }

    float getMarkerX (int parameter) const { return xFor (value (parameter)); }

    std::vector<float> getSliceXs() const
    {
        std::vector<float> xs;
        const auto frames = sound != nullptr ? sound->audio.getNumSamples() : 0;

        for (int i = 1; i < slices.count && frames > 0; ++i)
            xs.push_back (xFor ((float) slices.bounds[(size_t) i] / (float) frames));

        return xs;
    }

    // What it shows, as it is now; repainted where that moved.
    void refresh()
    {
        auto* sampler = editor.getDevice().getProcessor<Sampler>();
        const auto name = editor.getDevice().getSampleName();

        // The sound the engine plays, or else the one the song names (nothing plays the song).
        if (sampler == nullptr && (fallback == nullptr || name != fallbackName))
        {
            fallback = editor.getDevice().getSampleSound();
            fallbackName = name;
        }

        // Held while it shows: hidden, the view isn't refreshed, and the sampler may let go of a sound swapped out meanwhile.
        held = sampler != nullptr ? sampler->holdSound() : fallback;
        sound = held.get();
        manual = sampler != nullptr ? (sampler->getManualSlices() != nullptr ? *sampler->getManualSlices() : Sampler::ManualSlices())
                                    : editor.getDevice().getManualSlices();
        tempo = sampler != nullptr ? (double) sampler->getShown().tempo.get() : 120.0;

        for (size_t i = 0; i < handles.size(); ++i)
            values[i] = handles[i].getValue();

        slices = sound != nullptr ? Sampler::getSlices (*sound, &manual, values, tempo) : Sampler::Slices();

        std::vector<float> numbers (values.begin(), values.end());

        for (int i = 0; i <= slices.count; ++i)
            numbers.push_back ((float) slices.bounds[(size_t) i]);

        for (int i = 0; i < Sampler::maxVoices; ++i)
            numbers.push_back (sampler != nullptr ? sampler->getShown().positions[(size_t) i].get() : -1.0f);

        numbers.push_back (sampler != nullptr ? sampler->getShown().slice.get() : -1.0f);
        numbers.push_back (draggedTo);
        numbers.push_back (dropping ? 1.0f : 0.0f);
        numbers.push_back ((float) getWidth());

        if (sound != shownSound)
        {
            shownSound = sound;
            peaksWidth = 0;
            numbers.push_back ((float) ++soundsShown);
        }

        if (drawn.changed (std::move (numbers)))
        {
            slicesToClip.setVisible (getMode() == (int) Sampler::Mode::slice);
            repaint();
        }
    }

    void resized() override
    {
        const auto width = slicesToClip.getBestWidthForHeight (controlHeight - Space::xs);
        slicesToClip.setBounds (getWidth() - width - Space::xs, Space::xs, width, controlHeight - Space::xs);
        peaksWidth = 0;
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto tint = getDeviceTint (*this);
        graph::paintBackground (g, p, getLocalBounds().toFloat());

        if (sound == nullptr || sound->audio.getNumSamples() == 0)
            return;

        const auto area = getWaveArea();
        const auto frames = sound->audio.getNumSamples();
        const auto playMode = getMode();
        const auto from = std::min (value (Sampler::start), value (Sampler::end));
        const auto to = std::max (value (Sampler::start), value (Sampler::end));
        updatePeaks ((int) area.getWidth());

        // The loop's band, under the sound.
        if (playMode == (int) Sampler::Mode::classic && value (Sampler::loopOn) > 0.5f)
        {
            const auto band = juce::Rectangle<float>::leftTopRightBottom (xFor (value (Sampler::loopStart)), area.getY(),
                                                                          xFor (value (Sampler::loopEnd)), area.getBottom());
            g.setColour (p.loop.withAlpha (0.18f));
            g.fillRect (band);

            // Its seam's crossfade, fading out toward the loop's end.
            const auto fadeWidth = band.getWidth() * std::min (0.5f, value (Sampler::loopCrossfade));
            g.setGradientFill (juce::ColourGradient (p.loop.withAlpha (0.0f), band.getRight() - fadeWidth, 0.0f, p.loop.withAlpha (0.3f),
                                                     band.getRight(), 0.0f, false));
            g.fillRect (band.withLeft (band.getRight() - fadeWidth));
        }

        // The sound, each column's lowest to highest, faint outside what plays.
        const auto centre = area.getCentreY(), half = area.getHeight() * 0.5f;

        for (int column = 0; column < (int) peaks.size(); ++column)
        {
            const auto x = area.getX() + (float) column + 0.5f;
            const auto fraction = ((float) column + 0.5f) / (float) std::max<size_t> (1, peaks.size());
            g.setColour (fraction >= from && fraction <= to ? tint : tint.withAlpha (GraphTint::partCurve));
            const auto [low, high] = peaks[(size_t) column];
            g.drawVerticalLine ((int) x, centre - std::min (1.0f, high) * half, centre - std::max (-1.0f, low) * half + 1.0f);
        }

        // One-Shot's fades, as ramps at the ends of what plays.
        if (playMode == (int) Sampler::Mode::oneShot)
        {
            const auto perMillisecond = (float) (sound->sampleRate * 0.001 / frames);
            const auto inEnd = std::min (to, from + value (Sampler::fadeIn) * perMillisecond);
            const auto outStart = std::max (from, to - value (Sampler::fadeOut) * perMillisecond);
            g.setColour (p.text.withAlpha (GraphTint::marker));
            g.drawLine (xFor (from), area.getBottom(), xFor (inEnd), area.getY(), 1.5f);
            g.drawLine (xFor (outStart), area.getY(), xFor (to), area.getBottom(), 1.5f);
        }

        if (playMode == (int) Sampler::Mode::slice)
            paintSlices (g, p, tint, area);

        // The start and end markers, each a line with a flag at its top pointing into what plays.
        paintMarker (g, p.text, xFor (from), area, true);
        paintMarker (g, p.text, xFor (to), area, false);

        if (playMode == (int) Sampler::Mode::classic && value (Sampler::loopOn) > 0.5f)
        {
            paintMarker (g, p.loop, xFor (value (Sampler::loopStart)), area, true);
            paintMarker (g, p.loop, xFor (value (Sampler::loopEnd)), area, false);
        }

        // Where each voice is as it plays.
        if (auto* sampler = editor.getDevice().getProcessor<Sampler>())
        {
            g.setColour (p.text);

            for (const auto& position : sampler->getShown().positions)
                if (const auto at = position.get(); at >= 0.0f)
                    g.drawVerticalLine (juce::roundToInt (xFor (at)), area.getY(), area.getBottom());
        }

        // Its root key, its length, and whether it plays backward.
        g.setFont (juce::FontOptions (TextSize::small));
        g.setColour (p.textMuted);
        const auto corner = area.reduced ((float) Space::xs);
        auto info = juce::String (frames / std::max (1.0, sound->sampleRate), 2) + " s";

        if (value (Sampler::reverse) > 0.5f)
            info = "Reversed  " + info;

        g.drawText (info, corner, juce::Justification::bottomRight, false);

        if (playMode != (int) Sampler::Mode::slice)
            g.drawText ("Root " + juce::MidiMessage::getMidiNoteName (juce::roundToInt (value (Sampler::root)), true, true, 4), corner,
                        juce::Justification::topLeft, false);

        if (dropping)
        {
            g.setColour (p.accent);
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), cornerRadius, 2.0f);
        }
    }

    // Markers drag, the loop's band drags whole; in Slice mode a click under the drawing adds a slice; elsewhere a press plays
    // the sound, or the slice under the pointer, while it is held.
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (sound == nullptr)
            return;

        target = findTarget (e.position);
        downX = e.position.x;

        switch (target.kind)
        {
            case Kind::marker:
                startValue = value (target.parameter);
                handles[(size_t) target.parameter].beginGesture();
                return;

            case Kind::loopBand:
                startValue = value (Sampler::loopStart);
                handles[(size_t) Sampler::loopStart].beginGesture();
                handles[(size_t) Sampler::loopEnd].beginGesture();
                return;

            case Kind::slice:
                startValue = draggedTo = sliceFraction (target.slice);
                return;

            case Kind::strip:
                editor.getDevice().addSlice (snap (fractionAt (e.position.x), e.mods));
                target = {};
                return;

            case Kind::none:
                break;
        }

        // A press on the sound plays it at its root, or the slice under the pointer.
        if (auto* sampler = editor.getDevice().getProcessor<Sampler>())
        {
            auditioned = getMode() == (int) Sampler::Mode::slice ? Sampler::firstSliceNote + findSliceAt (fractionAt (e.position.x))
                                                                 : juce::roundToInt (value (Sampler::root));
            sampler->audition (auditioned, auditionVelocity);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (target.kind == Kind::none || sound == nullptr)
            return;

        // Where the pointer takes it: in fine steps with Shift, on the nearest zero crossing unless Alt places it anywhere.
        const auto area = getWaveArea();
        const auto moved = (e.position.x - downX) / std::max (1.0f, area.getWidth());
        auto fraction = e.mods.isShiftDown() ? startValue + moved * GraphTint::fineDragFactor : startValue + moved;
        fraction = snap (std::clamp (fraction, 0.0f, 1.0f), e.mods);
        const auto gap = (float) Sampler::samplesApart / (float) std::max (1, sound->audio.getNumSamples());

        switch (target.kind)
        {
            case Kind::marker:
            {
                const auto low = target.parameter == Sampler::end         ? value (Sampler::start) + gap
                                 : target.parameter == Sampler::loopEnd   ? value (Sampler::loopStart) + gap
                                 : target.parameter == Sampler::loopStart ? value (Sampler::start)
                                                                          : 0.0f;
                const auto high = target.parameter == Sampler::start       ? value (Sampler::end) - gap
                                  : target.parameter == Sampler::loopStart ? value (Sampler::loopEnd) - gap
                                  : target.parameter == Sampler::loopEnd   ? value (Sampler::end)
                                                                           : 1.0f;
                handles[(size_t) target.parameter].setValue (std::clamp (fraction, low, std::max (low, high)));
                break;
            }

            case Kind::loopBand:
            {
                const auto length = value (Sampler::loopEnd) - value (Sampler::loopStart);
                const auto at =
                    std::clamp (fraction, value (Sampler::start), std::max (value (Sampler::start), value (Sampler::end) - length));
                handles[(size_t) Sampler::loopStart].setValue (at);
                handles[(size_t) Sampler::loopEnd].setValue (at + length);
                break;
            }

            case Kind::slice:
            {
                const auto frames = (float) sound->audio.getNumSamples();
                const auto before = (float) slices.bounds[(size_t) target.slice - 1] / frames + gap;
                const auto after = (float) slices.bounds[(size_t) target.slice + 1] / frames - gap;
                draggedTo = std::clamp (fraction, before, std::max (before, after));
                break;
            }

            case Kind::strip:
            case Kind::none:
                break;
        }

        refresh();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        switch (target.kind)
        {
            case Kind::marker:
                handles[(size_t) target.parameter].endGesture();
                break;

            case Kind::loopBand:
                handles[(size_t) Sampler::loopStart].endGesture();
                handles[(size_t) Sampler::loopEnd].endGesture();
                break;

            case Kind::slice:
                // A slice moves once, as the drag ends: one undo step.
                if (std::abs (draggedTo - startValue) > 1.0e-6f)
                    editor.getDevice().moveSlice (target.slice, draggedTo);

                break;

            case Kind::strip:
            case Kind::none:
                break;
        }

        target = {};
        draggedTo = -1.0f;

        if (auditioned >= 0)
            if (auto* sampler = editor.getDevice().getProcessor<Sampler>())
                sampler->stopAudition (auditioned);

        auditioned = -1;
        refresh();
    }

    // A double-click takes a slice away, or puts a marker back where it starts.
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const auto found = findTarget (e.position);

        if (found.kind == Kind::slice)
            editor.getDevice().removeSlice (found.slice);
        else if (found.kind == Kind::marker)
            if (const auto start = handles[(size_t) found.parameter].getDefaultValue())
                handles[(size_t) found.parameter].setValue (*start);

        refresh();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto found = findTarget (e.position);
        setMouseCursor (found.kind == Kind::marker || found.kind == Kind::slice ? juce::MouseCursor::LeftRightResizeCursor
                        : found.kind == Kind::loopBand                          ? juce::MouseCursor::DraggingHandCursor
                                                                                : juce::MouseCursor::NormalCursor);
    }

    // From the browser: a sound file.
    bool isInterestedInDragSource (const SourceDetails& details) override { return getDraggedFile (details.description).has_value(); }
    void itemDragEnter (const SourceDetails&) override { setDropping (true); }
    void itemDragExit (const SourceDetails&) override { setDropping (false); }

    void itemDropped (const SourceDetails& details) override
    {
        setDropping (false);

        if (const auto file = getDraggedFile (details.description); file.has_value() && editor.dropFile (*file))
            markPlaced (details.description);
    }

    // From Explorer.
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return files.size() == 1; }
    void fileDragEnter (const juce::StringArray&, int, int) override { setDropping (true); }
    void fileDragExit (const juce::StringArray&) override { setDropping (false); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        setDropping (false);
        editor.dropFile (juce::File (files[0]));
    }

private:
    enum class Kind
    {
        none,
        marker,   // the start, the end, or the loop's start or end
        loopBand, // the loop, whole
        slice,    // a slice after the first
        strip     // under the drawing, in Slice mode: where a click adds a slice
    };

    struct Target
    {
        Kind kind = Kind::none;
        int parameter = -1; // a marker's
        int slice = -1;     // a slice's, from 1
    };

    SamplerEditor& editor;
    std::vector<ParameterHandle> handles; // every parameter, in the description's order
    std::vector<float> values;
    std::shared_ptr<const Sampler::Sound> held; // what sound points at, kept while it shows
    const Sampler::Sound* sound = nullptr;
    const Sampler::Sound* shownSound = nullptr;
    std::shared_ptr<const Sampler::Sound> fallback; // the song's, while nothing plays it
    juce::String fallbackName;
    Sampler::ManualSlices manual;
    Sampler::Slices slices;
    double tempo = 120.0;
    std::vector<std::pair<float, float>> peaks; // each column's lowest and highest
    int peaksWidth = 0, soundsShown = 0;
    DrawnNumbers drawn;
    juce::TextButton slicesToClip;
    Target target;
    float downX = 0.0f, startValue = 0.0f, draggedTo = -1.0f;
    int auditioned = -1;
    bool dropping = false;

    void tick() override { refresh(); }

    void setDropping (bool now)
    {
        dropping = now;
        refresh();
    }

    void updatePeaks (int width)
    {
        if (sound == nullptr || width <= 0 || width == peaksWidth)
            return;

        peaksWidth = width;
        peaks.assign ((size_t) width, { 0.0f, 0.0f });
        const auto frames = sound->audio.getNumSamples();

        for (int channel = 0; channel < sound->audio.getNumChannels(); ++channel)
        {
            const auto* samples = sound->audio.getReadPointer (channel);

            for (int column = 0; column < width; ++column)
            {
                const auto from = (int) ((juce::int64) column * frames / width);
                const auto to = std::max (from + 1, (int) ((juce::int64) (column + 1) * frames / width));
                auto& [low, high] = peaks[(size_t) column];

                for (int i = from; i < std::min (to, frames); ++i)
                {
                    low = std::min (low, samples[i]);
                    high = std::max (high, samples[i]);
                }
            }
        }
    }

    // Where the pointer places a marker: on the nearest zero crossing, unless Alt places it anywhere.
    float snap (float fraction, const juce::ModifierKeys& mods) const
    {
        if (sound == nullptr || mods.isAltDown())
            return fraction;

        const auto frames = sound->audio.getNumSamples();
        const auto at = devices::findZeroCrossing (sound->audio, (int) std::lround (fraction * (float) frames),
                                                   (int) (snapSeconds * sound->sampleRate));
        return (float) at / (float) std::max (1, frames);
    }

    float sliceFraction (int slice) const
    {
        const auto frames = sound != nullptr ? std::max (1, sound->audio.getNumSamples()) : 1;
        return (float) slices.bounds[(size_t) std::clamp (slice, 0, slices.count)] / (float) frames;
    }

    // The slice under a place in the sound, from 0.
    int findSliceAt (float fraction) const
    {
        auto found = 0;

        for (int i = 1; i < slices.count; ++i)
            if (fraction >= sliceFraction (i))
                found = i;

        return found;
    }

    Target findTarget (juce::Point<float> where) const
    {
        Target found;

        if (sound == nullptr)
            return found;

        const auto area = getWaveArea();
        const auto playMode = getMode();

        if (playMode == (int) Sampler::Mode::slice && where.y > area.getBottom())
            found.kind = Kind::strip;

        auto nearest = GraphTint::grabDistance * 0.5f;

        const auto consider = [&] (float x, Kind kind, int parameter, int slice)
        {
            if (const auto distance = std::abs (where.x - x); distance <= nearest)
            {
                nearest = distance;
                found = { kind, parameter, slice };
            }
        };

        if (playMode == (int) Sampler::Mode::slice)
            for (int i = 1; i < slices.count; ++i)
                consider (xFor (sliceFraction (i)), Kind::slice, -1, i);

        consider (xFor (value (Sampler::start)), Kind::marker, Sampler::start, -1);
        consider (xFor (value (Sampler::end)), Kind::marker, Sampler::end, -1);

        if (playMode == (int) Sampler::Mode::classic && value (Sampler::loopOn) > 0.5f)
        {
            consider (xFor (value (Sampler::loopStart)), Kind::marker, Sampler::loopStart, -1);
            consider (xFor (value (Sampler::loopEnd)), Kind::marker, Sampler::loopEnd, -1);

            // The band itself, in its top half, where nothing nearer is.
            if (found.kind == Kind::none && where.x > xFor (value (Sampler::loopStart)) && where.x < xFor (value (Sampler::loopEnd))
                && where.y < area.getCentreY())
                found.kind = Kind::loopBand;
        }

        return found;
    }

    void paintMarker (juce::Graphics& g, juce::Colour colour, float x, juce::Rectangle<float> area, bool pointsRight) const
    {
        g.setColour (colour);
        g.drawLine (x, area.getY(), x, area.getBottom(), 1.5f);
        juce::Path flag;
        const auto tip = pointsRight ? x + flagSize : x - flagSize;
        flag.addTriangle (x, area.getY(), tip, area.getY() + flagSize * 0.5f, x, area.getY() + flagSize);
        g.fillPath (flag);
    }

    void paintSlices (juce::Graphics& g, const Palette& p, juce::Colour tint, juce::Rectangle<float> area) const
    {
        // The slice playing now, washed; each slice's start a line, those put in by hand in the accent colour, numbered.
        auto* sampler = editor.getDevice().getProcessor<Sampler>();
        const auto playing = sampler != nullptr ? juce::roundToInt (sampler->getShown().slice.get()) : -1;
        const auto frames = (float) std::max (1, sound->audio.getNumSamples());

        if (juce::isPositiveAndBelow (playing, slices.count))
        {
            g.setColour (tint.withAlpha (GraphTint::spectrumFill));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (xFor ((float) slices.bounds[(size_t) playing] / frames), area.getY(),
                                                                    xFor ((float) slices.bounds[(size_t) playing + 1] / frames),
                                                                    area.getBottom()));
        }

        g.setFont (juce::FontOptions (TextSize::small));

        for (int i = 0; i < slices.count; ++i)
        {
            const auto dragged = i == target.slice && target.kind == Kind::slice && draggedTo >= 0.0f;
            const auto x = xFor (dragged ? draggedTo : (float) slices.bounds[(size_t) i] / frames);

            if (i > 0)
            {
                g.setColour (slices.byHand[(size_t) i] ? p.accent : p.text.withAlpha (GraphTint::marker));
                g.drawLine (x, area.getY(), x, area.getBottom() + (float) sliceStripHeight, dragged ? 2.0f : 1.0f);
            }

            g.setColour (p.textMuted);
            g.drawText (juce::String (i + 1), juce::Rectangle<float> (x + 2.0f, area.getY() + 1.0f, 24.0f, 12.0f),
                        juce::Justification::topLeft, false);
        }

        // Under the drawing, where a click adds a slice.
        const auto strip = juce::Rectangle<float> (area.getX(), area.getBottom(), area.getWidth(), (float) sliceStripHeight);
        g.setColour (p.surfaceRaised.withAlpha (0.6f));
        g.fillRect (strip);
        g.setColour (p.textMuted);
        g.drawText ("Click here to add a slice", strip, juce::Justification::centred, false);
    }
};

//==============================================================================
// The sound's part: the modes, its name, which opens its menu, and Reverse; the drawing; and under it, in one row, the
// controls of the mode it plays in (in Slice mode, the knob of the way it slices).
class SamplerEditor::SamplePart : public juce::Component
{
public:
    // Which of the controls show: Classic's, One-Shot's, then Slice's for each way to slice, in SliceBy's order.
    static constexpr int numControls = 6;

    static int getShown (int mode, int sliceBy)
    {
        return mode == (int) Sampler::Mode::classic ? 0 : (mode == (int) Sampler::Mode::oneShot ? 1 : 2 + std::clamp (sliceBy, 0, 3));
    }

    SamplePart (SamplerEditor& e, ChoiceButtons& m, SampleView& v, ParameterSwitch& r, std::array<ParameterControls*, numControls> c)
        : editor (e),
          modes (m),
          view (v),
          reverse (r),
          controls (c)
    {
        soundButton.setTitle ("Sound Choice");
        soundButton.setTooltip ("Chooses the sound the sampler plays: one that comes with Phazite, or a sound file of yours");
        soundButton.onClick = [this]
        { editor.getSoundMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&soundButton)); };
        addAndMakeVisible (soundButton);
        addAndMakeVisible (modes);
        addAndMakeVisible (view);
        addAndMakeVisible (reverse);

        for (auto* each : controls)
            addChildComponent (each);
    }

    int getPreferredHeight() const { return controlHeight + Space::xs + waveHeight + Space::xs + getControlsHeight(); }
    int getMinimumHeight() const { return controlHeight + Space::xs + smallestWaveHeight + Space::xs + getControlsHeight(); }

    int getPreferredWidth() const
    {
        auto widest = modes.getPreferredWidth() + Space::s + 120 + Space::s + reverse.getPreferredWidth();

        for (auto* each : controls)
            widest = std::max (widest, each->getPreferredWidth());

        return widest;
    }

    void show (int shown)
    {
        for (size_t i = 0; i < controls.size(); ++i)
            controls[i]->setVisible ((int) i == shown);
    }

    ParameterControls& getControls (int index) const { return *controls[(size_t) std::clamp (index, 0, numControls - 1)]; }

    void showSound (const juce::String& name)
    {
        if (soundButton.getButtonText() != name)
            soundButton.setButtonText (name);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto top = area.removeFromTop (controlHeight);
        modes.setBounds (top.removeFromLeft (std::min (top.getWidth() / 2, modes.getPreferredWidth())));
        top.removeFromLeft (Space::s);
        reverse.setBounds (top.removeFromRight (reverse.getPreferredWidth()));
        top.removeFromRight (Space::s);
        soundButton.setBounds (top);
        area.removeFromTop (Space::xs);

        const auto below = getControlsHeight();

        for (auto* each : controls)
            each->setBounds (area.withTop (area.getBottom() - below));

        area.removeFromBottom (below + Space::xs);
        view.setBounds (area);
    }

    juce::TextButton soundButton;

private:
    SamplerEditor& editor;
    ChoiceButtons& modes;
    SampleView& view;
    ParameterSwitch& reverse;
    std::array<ParameterControls*, numControls> controls;

    int getControlsHeight() const
    {
        auto tallest = 0;

        for (auto* each : controls)
            tallest = std::max (tallest, each->getPreferredHeight());

        return tallest;
    }
};

//==============================================================================
SamplerEditor::SamplerEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle),
      DisplayTicker::Display (static_cast<juce::Component&> (*this))
{
    const auto parameter = [this] (int index) { return device.getParameter (getID (index)); };
    const auto shown = [this]() -> const Sampler::Shown*
    {
        auto* sampler = device.getProcessor<Sampler>();
        return sampler != nullptr ? &sampler->getShown() : nullptr;
    };

    // The sound: its drawing, the modes, and each mode's controls under it.
    auto& view = keep (std::make_unique<SampleView> (*this));
    sampleView = &view;
    auto& modes = keep (std::make_unique<ChoiceButtons> (
        device, getID (Sampler::mode), ChoiceButtons::Direction::across,
        std::vector<juce::String> { "Plays the sound across the keys, for as long as a note is held, with its loop",
                                    "Plays the sound to its end at each note, however short the note, as a drum pad does",
                                    "Cuts the sound into slices, each on a key of its own from C2 up" }));
    addControl (getID (Sampler::mode), modes);

    // Each mode's controls in one row: Classic's markers and loop, One-Shot's markers, playback and fades, and Slice's way to
    // slice, with the knob that way has (none by hand), its playback, fades, choke and play-through.
    const auto row = [this] (std::vector<std::string_view> ids, std::vector<juce::String> names) -> ParameterControls&
    {
        const auto count = (int) ids.size();
        return makeControls (std::move (ids), count, ParameterControls::Size::compact, {}, names);
    };

    const std::vector<std::string_view> sliceStart { "sliceBy" }, sliceEnd { "playback", "fadeIn", "fadeOut", "choke", "playThrough" };
    const std::vector<juce::String> sliceEndNames { "Playback", "Fade In", "Fade Out", "Choke", "Through" };
    const auto sliceRow = [&] (std::optional<std::pair<std::string_view, juce::String>> knob) -> ParameterControls&
    {
        auto ids = sliceStart;
        std::vector<juce::String> names { "Slice By" };

        if (knob.has_value())
        {
            ids.push_back (knob->first);
            names.push_back (knob->second);
        }

        ids.insert (ids.end(), sliceEnd.begin(), sliceEnd.end());
        names.insert (names.end(), sliceEndNames.begin(), sliceEndNames.end());
        return row (ids, names);
    };

    std::array<ParameterControls*, SamplePart::numControls> rows {
        &row ({ "start", "end", "loopOn", "loopStart", "loopEnd", "loopCrossfade" },
              { "Start", "End", "Loop", "Loop Start", "Loop End", "Crossfade" }),
        &row ({ "start", "end", "playback", "fadeIn", "fadeOut" }, { "Start", "End", "Playback", "Fade In", "Fade Out" }),
        &sliceRow (std::pair<std::string_view, juce::String> { "sensitivity", "Sensitivity" }),
        &sliceRow (std::pair<std::string_view, juce::String> { "sliceDivision", "Division" }),
        &sliceRow (std::pair<std::string_view, juce::String> { "sliceCount", "Slices" }),
        &sliceRow ({}),
    };

    auto& reverse = keep (std::make_unique<ParameterSwitch> (device, getID (Sampler::reverse), "Reverse",
                                                             "Plays the sound backward, from its end marker to its start; in Slice mode, "
                                                             "each slice backward"));
    addControl (getID (Sampler::reverse), reverse);

    auto& part = keep (std::make_unique<SamplePart> (*this, modes, view, reverse, rows));
    samplePart = &part;
    DeviceLayout::Part sample { "Sample",
                                &part,
                                std::max (sampleWidth, part.getPreferredWidth()),
                                part.getPreferredHeight(),
                                part.getMinimumHeight(),
                                true,
                                std::max (smallestSampleWidth, part.getPreferredWidth()) };

    // How it plays: its pitch, and its voices.
    const auto pitch = makeControlsPart ("Pitch", { "root", "transpose", "fine" }, 3);
    const auto voice = makeControlsPart ("Voice", { "voiceMode", "glide", "bendRange", "velocity", "level" }, 5,
                                         ParameterControls::Size::compact, { "Mode", "Glide", "Bend", "Velocity", "Level" });

    // The amp envelope, Classic's.
    auto& ampView = keep (std::make_unique<EnvelopeView> ("Amp envelope", parameter (Sampler::attack), parameter (Sampler::decay),
                                                          parameter (Sampler::sustain), parameter (Sampler::release),
                                                          [shown] { return shown() != nullptr ? shown()->amp.get() : 0.0f; }));
    ampView.setCurves (parameter (Sampler::attackCurve), parameter (Sampler::decayCurve), parameter (Sampler::releaseCurve));
    const auto amp =
        makePicturePart ("Amp", ampView, 56, 32, { "attack", "decay", "sustain", "release", "attackCurve", "decayCurve", "releaseCurve" },
                         4, ParameterControls::Size::compact, { "Attack", "Decay", "Sustain", "Release", "A Curve", "D Curve", "R Curve" });
    ampContent = amp.content;

    // The filter, its envelope and the LFO, each switched on at its title.
    auto all = std::make_shared<std::vector<ParameterHandle>>();

    for (const auto& each : Sampler::info.parameters)
        all->push_back (device.getParameter (each.id));

    auto& filterView = keep (std::make_unique<FilterView> (
        "Filter response", parameter (Sampler::cutoff), parameter (Sampler::resonance),
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
            return Sampler::getFilterMagnitude (values, cutoffHertz, frequency, scope != nullptr ? scope->getSampleRate() : 48000.0);
        },
        [shown] { return shown() != nullptr ? shown()->cutoff.get() : 0.0f; }));
    auto filter = makePicturePart ("Filter", filterView, 70, 40,
                                   { "filterType", "cutoff", "resonance", "filterSlope", "filterEnvelope", "keyTracking" }, 3,
                                   ParameterControls::Size::compact, { "Type", "Cutoff", "Resonance", "Slope", "Envelope", "Key" });
    filter.titleSwitch = &makePartSwitch ("filterOn", *filter.content);

    auto& filterEnvelopeView = keep (std::make_unique<EnvelopeView> (
        "Filter envelope", parameter (Sampler::filterAttack), parameter (Sampler::filterDecay), parameter (Sampler::filterSustain),
        parameter (Sampler::filterRelease), [shown] { return shown() != nullptr ? shown()->filterEnvelope.get() : 0.0f; }));
    filterEnvelopeView.setCurves (parameter (Sampler::filterAttackCurve), parameter (Sampler::filterDecayCurve),
                                  parameter (Sampler::filterReleaseCurve));
    auto filterEnvelope = makePicturePart (
        "Filter Envelope", filterEnvelopeView, 56, 32,
        { "filterAttack", "filterDecay", "filterSustain", "filterRelease", "filterAttackCurve", "filterDecayCurve", "filterReleaseCurve" },
        4, ParameterControls::Size::compact, { "Attack", "Decay", "Sustain", "Release", "A Curve", "D Curve", "R Curve" });
    filterEnvelope.titleSwitch = &makePartSwitch ("filterEnvelopeOn", *filterEnvelope.content);

    auto lfoShape = std::make_shared<ParameterHandle> (parameter (Sampler::lfoShape));
    auto& lfoView = keep (std::make_unique<LfoView> (
        "LFO wave", [lfoShape] (float phase) { return getDrawnLfoValue (juce::roundToInt (lfoShape->getValue()), phase); },
        [shown] { return shown() != nullptr ? shown()->lfoPhase.get() : -1.0f; }));
    lfoView.setControls (parameter (Sampler::lfoDepth), parameter (Sampler::lfoRate));
    auto lfo = makePicturePart ("LFO", lfoView, 56, 32, { "lfoShape", "lfoRate", "lfoTarget", "lfoDepth" }, 2,
                                ParameterControls::Size::compact, { "Shape", "Rate", "Target", "Depth" });
    lfo.titleSwitch = &makePartSwitch ("lfoOn", *lfo.content);

    // The sound alone on the first page of the default dock, as wide as the dock; then how it plays and the amp; then the filter
    // and its envelope, and the LFO.
    setParts ({ { sample }, { pitch, voice }, { amp }, { filter }, { filterEnvelope }, { lfo } });
    refresh();
}

SamplerEditor::~SamplerEditor() = default;

juce::Component& SamplerEditor::getSampleView() const
{
    return *sampleView;
}

juce::Button& SamplerEditor::getSoundButton() const
{
    return samplePart->soundButton;
}

juce::Component& SamplerEditor::getModeControls (int mode) const
{
    const auto sliceBy = juce::roundToInt (device.getParameter (getID (Sampler::sliceBy)).getValue());
    return samplePart->getControls (SamplePart::getShown (mode, sliceBy));
}

float SamplerEditor::getMarkerX (int parameter) const
{
    return sampleView->getMarkerX (parameter);
}

std::vector<float> SamplerEditor::getSliceXs() const
{
    return sampleView->getSliceXs();
}

juce::PopupMenu SamplerEditor::getSoundMenu()
{
    juce::PopupMenu menu;
    const juce::Component::SafePointer<SamplerEditor> safe (this);
    const auto factory = device.getFactorySample();

    // A sound file of the user's, ticked while it plays.
    if (device.hasSampleFile())
    {
        menu.addItem (device.getSampleName(), false, true, nullptr);
        menu.addSeparator();
    }

    const auto put = [safe] (int index)
    {
        if (safe != nullptr)
        {
            safe->device.setFactorySample (index);
            safe->refresh();
        }
    };

    const auto names = devices::getFactorySampleNames();
    menu.addItem (toString (names[0]), true, factory == 0, [put] { put (0); });

    for (const auto& kit : getFactoryKits())
    {
        juce::PopupMenu sounds;

        for (int index = kit.first; index < kit.first + kit.count; ++index)
            sounds.addItem (toString (names[(size_t) index]), true, factory == index, [put, index] { put (index); });

        menu.addSubMenu (kit.name, sounds, true, nullptr, factory >= kit.first && factory < kit.first + kit.count);
    }

    menu.addSeparator();
    menu.addItem ("Load Sound File...",
                  [safe]
                  {
                      if (safe != nullptr)
                          safe->chooseFile();
                  });
    menu.addItem ("Find Root Key",
                  [safe]
                  {
                      if (safe != nullptr)
                          safe->device.findRootKey();
                  });
    return menu;
}

bool SamplerEditor::dropFile (const juce::File& file)
{
    const auto put = device.setSampleFile (file);
    refresh();
    return put;
}

void SamplerEditor::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a sound for the sampler", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<SamplerEditor> (this)] (const juce::FileChooser& chosen)
                          {
                              if (safe != nullptr && chosen.getResult() != juce::File())
                                  safe->dropFile (chosen.getResult());
                          });
}

void SamplerEditor::refresh()
{
    // The sound's name, the mode's controls, and the parts the mode doesn't use faint: the amp envelope outside Classic, the
    // root key and the voices' mode and glide in Slice mode, where each slice plays at its own pitch and choke cuts them.
    const auto playMode = juce::roundToInt (device.getParameter (getID (Sampler::mode)).getValue());
    samplePart->showSound (device.getSampleName());
    samplePart->show (SamplePart::getShown (playMode, juce::roundToInt (device.getParameter (getID (Sampler::sliceBy)).getValue())));
    sampleView->refresh();

    const auto faintUnless = [] (juce::Component* control, bool used)
    {
        if (control != nullptr)
            control->setAlpha (used ? 1.0f : SwitchTint::partOff);
    };

    faintUnless (ampContent, playMode == (int) Sampler::Mode::classic);
    faintUnless (findControl (getID (Sampler::root)), playMode != (int) Sampler::Mode::slice);
    faintUnless (findControl (getID (Sampler::voiceMode)), playMode != (int) Sampler::Mode::slice);
    faintUnless (findControl (getID (Sampler::glide)), playMode != (int) Sampler::Mode::slice);
}
} // namespace phazite::ui
