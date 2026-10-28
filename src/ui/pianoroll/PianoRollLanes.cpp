// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll's keys, velocity lane and transform bar.
#include "DrumRows.h"
#include "PianoRollParts.h"
#include "ui/theme/LookAndFeel.h"

#include <map>

namespace phazite::ui
{
namespace
{
    constexpr int velocityHitDistance = 4; // pixels from a note's start that the velocity lane counts as on it
} // namespace

//==============================================================================
PianoRoll::Keys::Keys (PianoRoll& r)
    : roll (r)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Keys");
    setDescription ("Click a key to hear its note. In drum mode, drag a drum up or down to reorder the rows, and right-click to hide it.");
}

void PianoRoll::Keys::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surfaceRaised);

    const auto height = roll.getRowHeight();
    const auto first = std::max (0, roll.getRowAt (0));
    const auto last = roll.getRowAt (getHeight() - 1) >= 0 ? roll.getRowAt (getHeight() - 1) : roll.getNumRows() - 1;

    // Keys that look like a piano's in every theme.
    const auto white = p.surfaceRaised.interpolatedWith (juce::Colours::white, 0.85f);
    const auto black = p.background.interpolatedWith (juce::Colours::black, 0.6f);

    for (auto row = first; row <= last && row < roll.getNumRows(); ++row)
    {
        const auto pitch = roll.getPitchForRow (row);
        auto area = juce::Rectangle<int> (0, roll.getRowTop (row), getWidth(), height);
        const auto playing = pitch == playingPitch;

        if (roll.drumMode)
        {
            g.setColour (playing ? p.accent.withAlpha (0.4f) : (row % 2 == 0 ? p.surface : p.surfaceRaised));
            g.fillRect (area);
            g.setColour (row == draggedRow ? p.accent : p.text);
            g.setFont (juce::FontOptions (TextSize::small));
            g.drawText (roll.drumRows[(size_t) row].name, area.reduced (Space::xs, 0), juce::Justification::centredLeft, true);
            g.setColour (p.border);
            g.fillRect (area.removeFromBottom (1));
            continue;
        }

        const auto isBlack = juce::MidiMessage::isMidiNoteBlack (pitch);
        const auto keyColour = isBlack ? black : white;
        g.setColour (playing ? p.accent : keyColour);
        g.fillRect (isBlack ? area.withWidth (getWidth() * 2 / 3) : area);

        if (isBlack)
        {
            g.setColour (white);
            g.fillRect (area.withTrimmedLeft (getWidth() * 2 / 3));
        }

        g.setColour (p.border);
        g.fillRect (area.removeFromBottom (1));

        // C4 is middle C, as on the top bar's keyboard button.
        if (pitch % 12 == 0 && height >= 9)
        {
            g.setColour (getInkFor (white).withAlpha (0.8f));
            g.setFont (juce::FontOptions (std::min (TextSize::small, (float) height)));
            g.drawText (juce::MidiMessage::getMidiNoteName (pitch, true, true, 4), area.reduced (Space::xs, 0),
                        juce::Justification::centredRight, false);
        }
    }

    // Where a dragged drum would go.
    if (dropRow >= 0)
    {
        g.setColour (p.accent);
        g.fillRect (0, roll.getRowTop (dropRow) - 1, getWidth(), 3);
    }

    g.setColour (p.border);
    g.fillRect (getWidth() - 1, 0, 1, getHeight());
}

void PianoRoll::Keys::mouseDown (const juce::MouseEvent& e)
{
    const auto row = roll.getRowAt (e.y);
    const auto pitch = roll.getPitchForRow (row);
    const auto shown = roll.getClips();

    if (pitch < 0 || shown.empty())
        return;

    if (e.mods.isPopupMenu())
    {
        // Drum mode: hide this drum's row, or show them all again.
        if (const auto track = roll.getTrack(); track.isValid() && roll.drumMode)
        {
            juce::PopupMenu menu;
            const juce::Component::SafePointer<Keys> safeThis (this);

            menu.addItem ("Hide " + roll.drumRows[(size_t) row].name,
                          [safeThis, track, pitch]
                          {
                              if (safeThis != nullptr)
                              {
                                  hideDrumRow (safeThis->roll.context, track, pitch);
                                  safeThis->roll.clipsChanged();
                              }
                          });

            menu.addItem ("Show All Rows",
                          [safeThis, track]
                          {
                              if (safeThis != nullptr)
                              {
                                  showAllDrumRows (safeThis->roll.context, track);
                                  safeThis->roll.clipsChanged();
                              }
                          });

            menu.showMenuAsync (juce::PopupMenu::Options());
        }

        return;
    }

    playingPitch = pitch;
    roll.audition (shown.front(), pitch, roll.lastVelocity);

    if (roll.drumMode)
        draggedRow = row;

    repaint();
}

void PianoRoll::Keys::mouseDrag (const juce::MouseEvent& e)
{
    const auto row = roll.getRowAt (e.y);

    // Drum mode: dragging a row moves it; the line shows where.
    if (roll.drumMode && draggedRow >= 0)
    {
        const auto target = juce::jlimit (0, roll.getNumRows(), (e.y + roll.scrollY + roll.getRowHeight() / 2) / roll.getRowHeight());
        dropRow = target != draggedRow && target != draggedRow + 1 ? target : -1;
        repaint();
        return;
    }

    // Piano: sliding along the keys plays each one.
    if (const auto pitch = roll.getPitchForRow (row); pitch >= 0 && pitch != playingPitch)
    {
        if (const auto shown = roll.getClips(); ! shown.empty())
        {
            playingPitch = pitch;
            roll.audition (shown.front(), pitch, roll.lastVelocity);
            repaint();
        }
    }
}

void PianoRoll::Keys::mouseUp (const juce::MouseEvent&)
{
    roll.stopAudition();

    if (const auto track = roll.getTrack(); track.isValid() && roll.drumMode && draggedRow >= 0 && dropRow >= 0)
    {
        const auto note = roll.drumRows[(size_t) draggedRow].note;
        setDrumRowOrder (roll.context, track, getDrumOrderAfterMove (roll.drumRows, note, dropRow > draggedRow ? dropRow - 1 : dropRow));
        roll.clipsChanged();
    }

    playingPitch = draggedRow = dropRow = -1;
    repaint();
}

//==============================================================================
PianoRoll::VelocityLane::VelocityLane (PianoRoll& r)
    : roll (r)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Velocity");
    setDescription ("How hard each note is played. Drag across the notes to set their velocity: the selected ones, if the mouse is "
                    "on any of them. New notes get the velocity set last.");
}

int PianoRoll::VelocityLane::velocityAt (float y) const
{
    const auto usable = std::max (1.0f, (float) getHeight() - 4.0f);
    return juce::jlimit (1, 127, juce::roundToInt (127.0f * (1.0f - (y - 2.0f) / usable)));
}

void PianoRoll::VelocityLane::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.background);

    // Lines at a quarter, half and three quarters of the full velocity.
    g.setColour (p.border.withMultipliedAlpha (0.5f));

    for (const auto fraction : { 0.25f, 0.5f, 0.75f })
        g.fillRect (0.0f, 2.0f + ((float) getHeight() - 4.0f) * (1.0f - fraction), (float) getWidth(), 1.0f);

    const auto& timeline = roll.getView();
    const auto usable = (float) getHeight() - 4.0f;
    const auto& [notes, colours, selected] = roll.getDrawnNotes();

    for (const auto& note : notes)
    {
        const auto x = (float) timeline.beatToX (note.start);

        if (x < -4.0f || x > (float) getWidth() + 4.0f)
            continue;

        const auto height = usable * (float) note.velocity / 127.0f;
        const auto top = 2.0f + usable - height;
        const auto found = colours.find (note.clip);
        auto colour = selected.contains (note.getRef()) ? p.accent : (found != colours.end() ? found->second : p.textMuted);

        if (note.muted)
            colour = colour.withAlpha (0.4f);

        g.setColour (colour);
        g.fillRect (x, top, 2.0f, height);
        g.fillEllipse (x - 2.5f, top - 2.5f, 6.0f, 6.0f);
    }
}

void PianoRoll::VelocityLane::setVelocitiesAt (const juce::MouseEvent& e)
{
    const auto velocity = velocityAt (e.position.y);
    const auto& timeline = roll.getView();
    const auto selected = roll.getSelectedNoteRefs();
    std::vector<NoteRef> atMouse, selectedAtMouse;

    for (const auto& note : roll.getAllNotes())
    {
        if (std::abs ((float) timeline.beatToX (note.start) - e.position.x) <= (float) velocityHitDistance)
        {
            atMouse.push_back (note.getRef());

            if (selected.contains (note.getRef()))
                selectedAtMouse.push_back (note.getRef());
        }
    }

    setNoteVelocities (roll.context, selectedAtMouse.empty() ? atMouse : selectedAtMouse, velocity);
    roll.lastVelocity = velocity;
    repaint();
}

void PianoRoll::VelocityLane::mouseDown (const juce::MouseEvent& e)
{
    if (! roll.hasProject() || roll.getClips().empty())
        return;

    dragging = true;
    step = std::make_unique<SongGesture> (roll.context, "Change Velocity");
    setVelocitiesAt (e);
}

void PianoRoll::VelocityLane::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging)
        setVelocitiesAt (e);
}

void PianoRoll::VelocityLane::mouseUp (const juce::MouseEvent&)
{
    dragging = false;
    step = nullptr; // the drag's undo step is done
}

//==============================================================================
PianoRoll::TransformBar::TransformBar (PianoRoll& r)
    : roll (r)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Transform");
    title.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));
    info.setFont (juce::FontOptions (TextSize::small));

    for (auto* slider : { &first, &second })
    {
        slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, controlHeight - Space::xs);
        slider->onValueChange = [this] { controlsChanged(); };
    }

    option.onClick = [this] { controlsChanged(); };
    applyButton.onClick = [this] { roll.applyTransform(); };
    cancelButton.onClick = [this] { roll.closeTransform(); };

    for (auto* c : std::initializer_list<juce::Component*> { &title, &info, &first, &second, &option, &applyButton, &cancelButton })
        addAndMakeVisible (c);
}

void PianoRoll::TransformBar::show (const Transform& shown)
{
    const auto gridName = roll.hasProject() ? describeGridStep (roll.context, roll.getGridStep()) : juce::String();

    // Each kind uses the controls it needs; the values come from the settings used last.
    const auto setUp = [] (ValueSlider& slider, const juce::String& name, double min, double max, double step, double value,
                           std::function<juce::String (double)> toText)
    {
        slider.setTitle (name);
        slider.setRange (min, max, step);
        slider.textFromValueFunction = std::move (toText);
        slider.setValue (value, juce::dontSendNotification);
        slider.updateText();
        slider.setVisible (true);
    };

    const auto percent = [] (double value) { return juce::String (juce::roundToInt (value)) + "%"; };
    first.setVisible (false);
    second.setVisible (false);
    option.setVisible (false);

    switch (shown.kind)
    {
        case Transform::Kind::quantize:
            title.setText ("Quantize", juce::dontSendNotification);
            info.setText ("to the grid: " + gridName, juce::dontSendNotification);
            setUp (first, "Strength", 0.0, 100.0, 1.0, shown.quantize.strength * 100.0, percent);
            setUp (second, "Swing", 0.0, 100.0, 1.0, shown.quantize.swing * 100.0, percent);
            option.setButtonText ("Ends too");
            option.setToggleState (shown.quantize.ends, juce::dontSendNotification);
            option.setVisible (true);
            break;

        case Transform::Kind::legato:
            title.setText ("Legato", juce::dontSendNotification);
            info.setText ("each note lasts until the next one starts", juce::dontSendNotification);
            setUp (first, "Gap", 0.0, 0.5, 1.0 / 64.0, shown.legatoGapBeats,
                   [] (double beats) { return juce::String (beats, 3) + " beat"; });
            break;

        case Transform::Kind::chop:
            title.setText ("Chop", juce::dontSendNotification);
            info.setText ("into pieces as long as the grid's step: " + gridName, juce::dontSendNotification);
            break;

        case Transform::Kind::transpose:
        default:
            title.setText ("Transpose", juce::dontSendNotification);
            info.setText ({}, juce::dontSendNotification);
            setUp (first, shown.transposeInKey ? "Steps" : "Semitones", -24.0, 24.0, 1.0, shown.transposeSteps,
                   [] (double steps) { return (steps > 0.0 ? "+" : "") + juce::String (juce::roundToInt (steps)); });
            // In steps of the track's key, if it has one.
            {
                const auto key = roll.getKey();
                option.setButtonText (key.has_value() ? "In key (" + describeKey (*key) + ")"
                                                      : juce::String ("In key (the track has none)"));
                option.setEnabled (key.has_value());
                option.setToggleState (shown.transposeInKey && key.has_value(), juce::dontSendNotification);
            }

            option.setVisible (true);
            break;
    }

    applyButton.setTooltip ("Applies the change shown (Enter)");
    cancelButton.setTooltip ("Closes this without changing the notes (Esc)");
    resized();
}

void PianoRoll::TransformBar::controlsChanged()
{
    auto& settings = roll.transform;

    switch (settings.kind)
    {
        case Transform::Kind::quantize:
            settings.quantize.strength = (float) (first.getValue() / 100.0);
            settings.quantize.swing = (float) (second.getValue() / 100.0);
            settings.quantize.ends = option.getToggleState();
            break;

        case Transform::Kind::legato:
            settings.legatoGapBeats = first.getValue();
            break;

        case Transform::Kind::transpose:
            settings.transposeSteps = juce::roundToInt (first.getValue());

            if (settings.transposeInKey != option.getToggleState())
            {
                settings.transposeInKey = option.getToggleState();
                first.setTitle (settings.transposeInKey ? "Steps" : "Semitones");
            }

            break;

        case Transform::Kind::chop:
        default:
            break;
    }

    roll.grid->repaint();
}

void PianoRoll::TransformBar::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surfaceRaised);
    g.setColour (p.accent);
    g.fillRect (0, 0, getWidth(), 2);
}

void PianoRoll::TransformBar::resized()
{
    auto area = getLocalBounds().reduced (Space::s, Space::xs).withTrimmedTop (2);
    applyButton.setBounds (area.removeFromRight (80));
    area.removeFromRight (Space::xs);
    cancelButton.setBounds (area.removeFromRight (80));
    area.removeFromRight (Space::s);
    title.setBounds (area.removeFromLeft (96));

    for (auto* c : std::initializer_list<juce::Component*> { &first, &second })
    {
        if (c->isVisible())
        {
            c->setBounds (area.removeFromLeft (200));
            area.removeFromLeft (Space::s);
        }
    }

    if (option.isVisible())
    {
        option.setBounds (area.removeFromLeft (180));
        area.removeFromLeft (Space::s);
    }

    info.setBounds (area);
}
} // namespace phazite::ui
