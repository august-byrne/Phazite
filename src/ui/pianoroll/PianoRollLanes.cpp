// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll's keys, velocity lane and transform bar.
#include "DrumRows.h"
#include "PianoRollParts.h"
#include "ui/theme/LookAndFeel.h"

#include <map>
#include <unordered_map>

namespace phazite::ui
{
namespace
{
    constexpr int velocityHitDistance = 4; // pixels from a note's start that the velocity lane counts as on it
    constexpr int velocityPerNotch = 4;    // the wheel over the velocity lane
} // namespace

//==============================================================================
PianoRoll::Keys::Keys (PianoRoll& r)
    : roll (r)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Keys");
    setDescription ("Click a key to hear its note and select its notes; Ctrl+click adds them to the selection, and a drag along the "
                    "keys selects the notes of every key it passes. In drum mode, click a drum to select its hits, drag it up or down to "
                    "reorder the rows, and right-click to hide it.");
}

void PianoRoll::Keys::selectPitches (int from, int to)
{
    // As in Live and Logic Pro: the shown notes of these pitches, with those selected before when Ctrl or Shift was held.
    const auto low = std::min (from, to), high = std::max (from, to);
    auto notes = selectionBefore;

    for (const auto& note : roll.getAllNotes())
        if (note.pitch >= low && note.pitch <= high && std::find (notes.begin(), notes.end(), note.getRef()) == notes.end())
            notes.push_back (note.getRef());

    roll.selectNotes (notes, false);
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

        // A black key covers the left of its row. On its right, the white keys beside it meet, each reaching halfway
        // into its row, as on a piano; where two white keys meet without one between (E and F, B and C), a line parts
        // them right across.
        const auto whiteColour = [this, &p, white] (int key) { return key == playingPitch ? p.accent : white; };
        const auto blackWidth = getWidth() * 2 / 3;

        if (juce::MidiMessage::isMidiNoteBlack (pitch))
        {
            const auto right = area.withTrimmedLeft (blackWidth);
            g.setColour (playing ? p.accent : black);
            g.fillRect (area.withWidth (blackWidth));
            g.setColour (whiteColour (pitch + 1));
            g.fillRect (right.withHeight (height / 2));
            g.setColour (whiteColour (pitch - 1));
            g.fillRect (right.withTrimmedTop (height / 2));
            g.setColour (p.border);
            g.fillRect (right.withTrimmedTop (height / 2).withHeight (1));
        }
        else
        {
            g.setColour (whiteColour (pitch));
            g.fillRect (area);

            if (pitch == 0 || ! juce::MidiMessage::isMidiNoteBlack (pitch - 1))
            {
                g.setColour (p.border);
                g.fillRect (area.removeFromBottom (1));
            }
        }

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

    if (pitch < 0 || shown.empty() || e.mods.isMiddleButtonDown())
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
    addsToSelection = e.mods.isCommandDown() || e.mods.isShiftDown();
    selectionBefore = addsToSelection ? roll.getSelectedRefsHere() : std::vector<NoteRef>();

    // A drum's hits are selected when the mouse goes up without moving its row; a key's notes at once.
    if (roll.drumMode)
    {
        draggedRow = row;
    }
    else
    {
        pressedPitch = pitch;
        selectPitches (pitch, pitch);
    }

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

    // Piano: sliding along the keys plays each one, and selects the notes of the keys from the one pressed to it.
    if (const auto pitch = roll.getPitchForRow (row); pitch >= 0 && pitch != playingPitch)
    {
        if (const auto shown = roll.getClips(); ! shown.empty())
        {
            playingPitch = pitch;
            roll.audition (shown.front(), pitch, roll.lastVelocity);

            if (pressedPitch >= 0)
                selectPitches (pressedPitch, pitch);

            repaint();
        }
    }
}

void PianoRoll::Keys::mouseUp (const juce::MouseEvent&)
{
    roll.stopAudition();

    if (const auto track = roll.getTrack(); track.isValid() && roll.drumMode && draggedRow >= 0)
    {
        const auto note = roll.drumRows[(size_t) draggedRow].note;

        if (dropRow >= 0)
        {
            setDrumRowOrder (roll.context, track,
                             getDrumOrderAfterMove (roll.drumRows, note, dropRow > draggedRow ? dropRow - 1 : dropRow));
            roll.clipsChanged();
        }
        else
        {
            selectPitches (note, note);
        }
    }

    playingPitch = pressedPitch = draggedRow = dropRow = -1;
    selectionBefore.clear();
    repaint();
}

//==============================================================================
PianoRoll::VelocityLane::VelocityLane (PianoRoll& r)
    : roll (r)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Velocity");
    setDescription ("How hard each note is played, which the notes show too. Drag across the notes to set their velocity; drag a "
                    "selected note's bar to move every selected note's by as much; right-drag to draw a straight ramp; and turn the "
                    "wheel to raise or lower them. New notes get the velocity set last.");
    setWantsKeyboardFocus (true); // a press here gives the piano roll the keys: Escape takes the drag back
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

        // The stem on the screen's pixels, and the ball centred on it.
        const auto left = snapToPixels (g, x - VelocityTint::stem / 2.0f);
        const auto centre = left + VelocityTint::stem / 2.0f;
        g.setColour (colour);
        g.fillRect (left, snapToPixels (g, top), VelocityTint::stem, height);
        g.fillEllipse (juce::Rectangle<float> (VelocityTint::ball, VelocityTint::ball).withCentre ({ centre, top }));
    }

    // The ramp being drawn.
    if (drag == Drag::ramp)
    {
        g.setColour (p.accent);
        g.drawLine ({ rampStart, rampEnd }, VelocityTint::stem);
    }
}

std::vector<NoteRef> PianoRoll::VelocityLane::getNotesAt (float x) const
{
    const auto& timeline = roll.getView();
    const auto selected = roll.getSelectedNoteRefs();
    std::vector<NoteRef> atMouse, selectedAtMouse;

    for (const auto& note : roll.getAllNotes())
    {
        if (std::abs ((float) timeline.beatToX (note.start) - x) <= (float) velocityHitDistance)
        {
            atMouse.push_back (note.getRef());

            if (selected.contains (note.getRef()))
                selectedAtMouse.push_back (note.getRef());
        }
    }

    return selectedAtMouse.empty() ? atMouse : selectedAtMouse;
}

void PianoRoll::VelocityLane::setVelocitiesAt (const juce::MouseEvent& e)
{
    const auto velocity = velocityAt (e.position.y);
    setNoteVelocities (roll.context, getNotesAt (e.position.x), velocity);
    roll.setNextNote (roll.lastLength, velocity); // new notes get the velocity set last
    repaint();
}

void PianoRoll::VelocityLane::shiftVelocities (const juce::MouseEvent& e)
{
    // By as much as the pointer moved, as far as every selected note can go, so that their differences stay; notes with the
    // same velocity change together.
    const auto selected = roll.getSelectedNoteRefs();
    auto change = velocityAt (e.position.y) - velocityAt (pressY);
    std::map<int, std::vector<NoteRef>> byVelocity;

    for (const auto& note : before)
    {
        if (selected.contains (note.getRef()))
        {
            change = juce::jlimit (1 - note.velocity, 127 - note.velocity, change);
            byVelocity[note.velocity].push_back (note.getRef());
        }
    }

    for (const auto& [velocity, notes] : byVelocity)
        setNoteVelocities (roll.context, notes, velocity + change);

    repaint();
}

void PianoRoll::VelocityLane::drawRamp (const juce::MouseEvent& e)
{
    // From where the right button went down to the pointer, on a straight line: the velocities of the notes between (of the
    // selected ones, if any is selected). The others are as they were when the drag began.
    rampEnd = e.position;
    const auto& timeline = roll.getView();
    const auto selected = roll.getSelectedNoteRefs();
    const auto left = std::min (rampStart.x, rampEnd.x) - (float) velocityHitDistance;
    const auto right = std::max (rampStart.x, rampEnd.x) + (float) velocityHitDistance;
    const auto width = rampEnd.x - rampStart.x;
    std::unordered_map<NoteRef, int> now;

    for (const auto& note : roll.getAllNotes())
        now[note.getRef()] = note.velocity;

    std::map<int, std::vector<NoteRef>> byVelocity;

    for (const auto& note : before)
    {
        const auto x = (float) timeline.beatToX (note.start);
        auto velocity = note.velocity;

        if (x >= left && x <= right && (selected.empty() || selected.contains (note.getRef())))
        {
            const auto along = std::abs (width) < 1.0f ? 1.0f : juce::jlimit (0.0f, 1.0f, (x - rampStart.x) / width);
            velocity = velocityAt (rampStart.y + (rampEnd.y - rampStart.y) * along);
        }

        if (const auto found = now.find (note.getRef()); found != now.end() && found->second != velocity)
            byVelocity[velocity].push_back (note.getRef());
    }

    for (const auto& [velocity, notes] : byVelocity)
        setNoteVelocities (roll.context, notes, velocity);

    repaint();
}

void PianoRoll::VelocityLane::mouseDown (const juce::MouseEvent& e)
{
    if (! roll.hasProject() || roll.getClips().empty() || e.mods.isMiddleButtonDown())
        return;

    const auto selected = roll.getSelectedNoteRefs();
    const auto atMouse = getNotesAt (e.position.x);
    before = roll.getAllNotes();
    pressY = e.position.y;
    step = std::make_unique<SongGesture> (roll.context, "Change Velocity");

    // The right button draws a straight ramp across the notes, as in FL Studio.
    if (e.mods.isPopupMenu())
    {
        drag = Drag::ramp;
        rampStart = rampEnd = e.position;
        drawRamp (e);
        return;
    }

    // From a selected note's bar, every selected note's velocity moves by as much, as Logic Pro's Velocity tool moves them;
    // elsewhere, the velocities under the mouse follow it.
    if (std::any_of (atMouse.begin(), atMouse.end(), [&selected] (NoteRef note) { return selected.contains (note); }))
    {
        drag = Drag::shift;
        return;
    }

    drag = Drag::draw;
    setVelocitiesAt (e);
}

void PianoRoll::VelocityLane::mouseDrag (const juce::MouseEvent& e)
{
    switch (drag)
    {
        case Drag::draw:
            setVelocitiesAt (e);
            break;
        case Drag::shift:
            shiftVelocities (e);
            break;
        case Drag::ramp:
            drawRamp (e);
            break;
        case Drag::none:
        default:
            break;
    }
}

void PianoRoll::VelocityLane::mouseUp (const juce::MouseEvent&)
{
    drag = Drag::none;
    step = nullptr; // the drag's undo step is done
    before.clear();
    repaint();
}

void PianoRoll::VelocityLane::cancelDrag()
{
    if (drag == Drag::none)
        return;

    if (step != nullptr)
        step->rollBack();

    step = nullptr;
    drag = Drag::none;
    before.clear();
    repaint();
}

void PianoRoll::VelocityLane::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // The wheel raises or lowers the velocities of the notes under the pointer (the selected ones, if it is on any), or else
    // of the selected notes, each turn a step of its own. With a modifier it zooms and scrolls, as over the notes.
    if (e.mods.isCommandDown() || e.mods.isShiftDown() || e.mods.isAltDown() || ! roll.hasProject())
    {
        juce::Component::mouseWheelMove (e, wheel); // the piano roll's
        return;
    }

    auto notes = getNotesAt (e.position.x);

    if (notes.empty())
        notes = roll.getSelectedRefsHere();

    if (const auto notches = getWheelNotches (wheel.deltaY); ! notes.empty() && ! juce::exactlyEqual (notches, 0.0))
        changeNoteVelocities (roll.context, notes, notches > 0.0 ? velocityPerNotch : -velocityPerNotch);
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
    const auto gridName = roll.hasProject() ? describeGridStep (roll.getGridStep()) : juce::String();

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
    const auto length = [] (double beats) { return describeNoteLength (BeatDuration::fromBeats (beats)); };
    first.setVisible (false);
    second.setVisible (false);
    option.setVisible (false);
    option.setEnabled (true);

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

        case Transform::Kind::strum:
            title.setText ("Strum", juce::dontSendNotification);
            info.setText ("each chord's notes start one after another, and end where they did", juce::dontSendNotification);
            setUp (first, "Step", 0.0, 0.25, 1.0 / 128.0, shown.strum.stepBeats, length);
            option.setButtonText ("From the top note down");
            option.setToggleState (shown.strum.down, juce::dontSendNotification);
            option.setVisible (true);
            break;

        case Transform::Kind::humanize:
            title.setText ("Humanize", juce::dontSendNotification);
            info.setText ("each note a little early or late, softer or louder", juce::dontSendNotification);
            setUp (first, "Timing", 0.0, 0.125, 1.0 / 256.0, shown.humanize.timingBeats,
                   [length] (double beats) { return "up to " + length (beats); });
            setUp (second, "Velocity", 0.0, 40.0, 1.0, shown.humanize.velocity,
                   [] (double velocity) { return "up to " + juce::String (juce::roundToInt (velocity)); });
            break;

        case Transform::Kind::arpeggiate:
            title.setText ("Arpeggiate", juce::dontSendNotification);
            info.setText ("each chord a note at a time, a step of the grid apart: " + gridName, juce::dontSendNotification);
            setUp (first, "Gate", 10.0, 100.0, 1.0, shown.arpeggio.gate * 100.0, percent);
            setUp (second, "Order", 0.0, 2.0, 1.0, (double) static_cast<int> (shown.arpeggio.order),
                   [] (double order)
                   {
                       const auto index = juce::roundToInt (order);
                       return index == 0 ? juce::String ("Up") : (index == 1 ? juce::String ("Down") : juce::String ("Up and down"));
                   });
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
    // A change a control sends after the bar closed (a slider's asynchronous one) would change the settings it put back.
    if (! roll.transformOpen)
        return;

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

        case Transform::Kind::strum:
            settings.strum.stepBeats = first.getValue();
            settings.strum.down = option.getToggleState();
            break;

        case Transform::Kind::humanize:
            settings.humanize.timingBeats = first.getValue();
            settings.humanize.velocity = juce::roundToInt (second.getValue());
            break;

        case Transform::Kind::arpeggiate:
            settings.arpeggio.gate = (float) (first.getValue() / 100.0);
            settings.arpeggio.order = static_cast<ArpeggioOrder> (juce::jlimit (0, 2, juce::roundToInt (second.getValue())));
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
