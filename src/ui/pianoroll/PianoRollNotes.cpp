// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll's note grid: finding the notes under the mouse, the hint for what it does there, and drawing the rows and
// notes. What the tools do with the mouse is PianoRollNoteTools.cpp's.
#include "PianoRollParts.h"
#include "model/chords/ChordModel.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr float noteCorner = 2.0f;

    const juce::String
        emptyHint ("Click to add a note with the last length and velocity, and drag to move it; Ctrl+drag selects notes in a "
                   "rectangle. Right-click deletes, Shift+right-click opens the menu. Ctrl+wheel zooms, Ctrl+Shift+wheel "
                   "makes the rows taller, Alt+wheel changes the selected notes' velocity.");
    const juce::String
        bodyHint ("Drag to move the note, and the selected notes with it; Shift+drag copies them, Alt places them off the grid. "
                  "Ctrl+click adds it to the selection; right-click deletes it.");
    const juce::String endHint ("Drag to make the note longer or shorter, and the selected notes with it. Hold Alt to leave the grid.");
    const juce::String startHint ("Drag to move the note's start, keeping its end. Hold Alt to leave the grid.");
    const juce::String noClipsHint ("Select a MIDI clip or a track in the playlist, or double-click a clip, to edit its notes here.");
    const juce::String emptyTrackHint ("This track has no MIDI clip yet. Draw a note to start one here.");

    juce::Path makeDiamond (juce::Rectangle<float> area)
    {
        juce::Path diamond;
        diamond.addQuadrilateral (area.getCentreX(), area.getY(), area.getRight(), area.getCentreY(), area.getCentreX(), area.getBottom(),
                                  area.getX(), area.getCentreY());
        return diamond;
    }
} // namespace

//==============================================================================
std::map<ClipID, juce::Colour> getNoteColours (const ProjectContext& context, const std::vector<NoteData>& notes)
{
    // Each clip's notes in its track's color, as in the playlist.
    std::vector<ClipID> ids;

    for (const auto& note : notes)
        if (std::find (ids.begin(), ids.end(), note.clip) == ids.end())
            ids.push_back (note.clip);

    std::map<ClipID, juce::Colour> colours;

    for (const auto& clip : getNoteClips (context, ids))
        colours[clip.id] = clip.colour;

    return colours;
}

//==============================================================================
juce::String PianoRoll::describeNoteTool (Tool toolToDescribe)
{
    switch (toolToDescribe)
    {
        case Tool::paint:
            return "Drag along a row to lay notes one after another, a step of the grid each.";
        case Tool::slice:
            return "Click a note to split it there. Hold Alt to split off the grid.";
        case Tool::mute:
            return "Click a note to mute or unmute it; drag to do it to several.";
        case Tool::erase:
            return "Click a note to delete it; drag over notes to delete them all.";
        case Tool::select:
            return "Drag to select the notes in a rectangle; with Ctrl, add them to the selection.";
        case Tool::zoom:
            return "Click to zoom in, Alt-click to zoom out, or drag across the time to zoom to.";
        case Tool::smart:
        case Tool::draw:
        case Tool::slip:
        default:
            return emptyHint;
    }
}

juce::String PianoRoll::describeToolHere (Tool toolToDescribe) const
{
    return describeNoteTool (toolToDescribe);
}

//==============================================================================
PianoRoll::NoteGrid::NoteGrid (PianoRoll& r)
    : roll (r)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Notes");
    setDescription (emptyHint);
    setWantsKeyboardFocus (true); // a click here gives the piano roll the keys (and makes it the editor the tools act on)
}

//==============================================================================
std::optional<NoteData> PianoRoll::NoteGrid::getNoteAt (juce::Point<float> position) const
{
    // A note in the row the mouse is in, the row a click there adds a note to: notes are drawn a pixel inside their rows,
    // and a click on a row's edge would otherwise miss the note there and stack a new one on it. The last drawn is on top.
    const auto row = roll.getRowAt (juce::roundToInt (std::floor (position.y)));
    const auto notes = roll.getAllNotes();

    for (auto it = notes.rbegin(); it != notes.rend(); ++it)
    {
        if (row < 0 || roll.getRowForPitch (it->pitch) != row)
            continue;

        if (const auto bounds = roll.getNoteBounds (*it); position.x >= bounds.getX() && position.x < bounds.getRight())
            return *it;
    }

    return {};
}

PianoRoll::NoteGrid::Zone PianoRoll::NoteGrid::getZone (const NoteData& note, juce::Point<float> position) const
{
    if (roll.drumMode)
        return Zone::body; // hits have no length to change

    const auto bounds = roll.getNoteBounds (note);
    const auto edge = std::min (5.0f, bounds.getWidth() / 4.0f);

    if (position.x >= bounds.getRight() - edge)
        return Zone::end;

    if (bounds.getWidth() > 16.0f && position.x <= bounds.getX() + 3.0f)
        return Zone::start;

    return Zone::body;
}

std::vector<NoteData> PianoRoll::NoteGrid::getGhostNotes() const
{
    if (! roll.hasProject() || ! roll.showGhostNotes)
        return {};

    // The other tracks' MIDI clips that are in view.
    std::vector<TrackID> shownTracks { roll.getTrack() };

    for (const auto& clip : roll.getShownClips())
        if (std::find (shownTracks.begin(), shownTracks.end(), clip.track) == shownTracks.end())
            shownTracks.push_back (clip.track);

    const auto& timeline = roll.getView();
    return getNotesOfOtherTracks (roll.context, shownTracks, { timeline.xToBeat (0), timeline.xToBeat (getWidth()) });
}

const PianoRoll::NoteGrid::DrawnGhosts& PianoRoll::NoteGrid::getDrawnGhosts()
{
    const auto& timeline = roll.getView();
    const BeatRange range { timeline.xToBeat (0), timeline.xToBeat (getWidth()) };
    const auto songVersion = roll.context.getSongVersion();

    if (! drawnGhosts.has_value() || drawnGhosts->songVersion != songVersion || drawnGhosts->range != range
        || drawnGhosts->clips != roll.clips || drawnGhosts->track != roll.emptyTrack || drawnGhosts->shown != roll.showGhostNotes)
    {
        auto notes = getGhostNotes();
        auto colours = getNoteColours (roll.context, notes);
        drawnGhosts =
            DrawnGhosts { std::move (notes), std::move (colours), songVersion, range, roll.clips, roll.emptyTrack, roll.showGhostNotes };
    }

    return *drawnGhosts;
}

ClipID PianoRoll::NoteGrid::getGhostClipAt (juce::Point<float> position) const
{
    for (const auto& note : getGhostNotes())
        if (roll.getNoteBounds (note).contains (position))
            return note.clip;

    return {};
}

//==============================================================================
void PianoRoll::NoteGrid::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.background);

    if (! roll.hasProject())
        return;

    const auto area = g.getClipBounds();

    if (! roll.getTrack().isValid())
    {
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::body));
        g.drawFittedText (noClipsHint, getLocalBounds().reduced (Space::xl), juce::Justification::centred, 2);
        return;
    }

    paintRows (g, area);

    if (! roll.drumMode)
        paintChordTones (g, area);

    // Where the clips are: a line in the clip's color at each end, and a tint between.
    const auto& timeline = roll.getView();
    const auto shown = roll.getShownClips();

    for (const auto& clip : shown)
    {
        const auto left = juce::roundToInt (timeline.beatToX (clip.range.getStart()));
        const auto right = juce::roundToInt (timeline.beatToX (clip.range.getEnd()));
        g.setColour (clip.colour.withAlpha (0.06f));
        g.fillRect (left, area.getY(), right - left, area.getHeight());
        g.setColour (clip.colour.withAlpha (0.6f));
        g.fillRect (left, area.getY(), 1, area.getHeight());
        g.fillRect (right - 1, area.getY(), 1, area.getHeight());
    }

    paintGridLines (g, roll, area);

    // A track without clips yet: the first note drawn starts one.
    if (shown.empty())
    {
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::body));
        g.drawFittedText (emptyTrackHint, getLocalBounds().reduced (Space::xl), juce::Justification::centred, 2);
    }

    // Other tracks' notes, faintly.
    const auto& drawn = getDrawnGhosts();
    const auto& ghosts = drawn.notes;
    const auto& ghostColours = drawn.colours;
    const auto colourOf = [&ghostColours, &p] (ClipID clip)
    {
        const auto found = ghostColours.find (clip);
        return found != ghostColours.end() ? found->second : p.textMuted;
    };

    for (const auto& note : ghosts)
    {
        const auto bounds = roll.getNoteBounds (note);

        if (! bounds.isEmpty() && bounds.intersects (area.toFloat()))
        {
            g.setColour (colourOf (note.clip).withAlpha (0.22f));

            if (roll.drumMode)
                g.fillPath (makeDiamond (bounds));
            else
                g.fillRoundedRectangle (bounds, noteCorner);
        }
    }

    paintNotes (g);

    if (! rubberBand.isEmpty())
    {
        g.setColour (p.accent.withAlpha (0.15f));
        g.fillRect (rubberBand);
        g.setColour (p.accent);
        g.drawRect (rubberBand, 1);
    }
}

void PianoRoll::NoteGrid::paintRows (juce::Graphics& g, juce::Rectangle<int> area)
{
    // Rows outside the track's key are darker; without a key, those of the black keys, as on a piano. In drum mode
    // the rows alternate.
    const auto& p = getPalette (*this);
    const auto key = roll.getKey().value_or (Key { 0, Scale::major }); // C major has the white keys
    const auto height = roll.getRowHeight();
    const auto first = std::max (0, roll.getRowAt (area.getY()));
    const auto last = roll.getRowAt (area.getBottom() - 1) >= 0 ? roll.getRowAt (area.getBottom() - 1) : roll.getNumRows() - 1;

    for (auto row = first; row <= last; ++row)
    {
        const auto pitch = roll.getPitchForRow (row);
        juce::Rectangle<int> rowArea (area.getX(), roll.getRowTop (row), area.getWidth(), height);
        const auto inKey = roll.drumMode ? row % 2 == 0 : isInKey (pitch, key);

        g.setColour (inKey ? p.surface : p.background);
        g.fillRect (rowArea);

        // A line under each row, stronger under each C.
        const auto isOctave = ! roll.drumMode && pitch % 12 == 0;
        g.setColour (isOctave ? p.border : p.border.withMultipliedAlpha (0.4f));
        g.fillRect (rowArea.removeFromBottom (1));
    }
}

void PianoRoll::NoteGrid::paintChordTones (juce::Graphics& g, juce::Rectangle<int> area)
{
    // While each of the chord lane's chords lasts, the rows of its notes are tinted, so that notes that fit it are easy
    // to find (docs/design.md, section 14).
    const auto& p = getPalette (*this);
    const auto& timeline = roll.getView();
    const auto height = roll.getRowHeight();
    const auto first = std::max (0, roll.getRowAt (area.getY()));
    const auto last = roll.getRowAt (area.getBottom() - 1) >= 0 ? roll.getRowAt (area.getBottom() - 1) : roll.getNumRows() - 1;
    g.setColour (p.accent.withAlpha (ChordTint::pianoRollRows));

    for (const auto& chord : getLaneChords (roll.context))
    {
        const auto left = std::max (area.getX(), juce::roundToInt (timeline.beatToX (chord.range.getStart())));
        const auto right = std::min (area.getRight(), juce::roundToInt (timeline.beatToX (chord.range.getEnd())));

        if (right <= left)
            continue;

        for (auto row = first; row <= last; ++row)
        {
            const auto pitchClass = roll.getPitchForRow (row) % 12;

            if (std::find (chord.pitchClasses.begin(), chord.pitchClasses.end(), pitchClass) != chord.pitchClasses.end())
                g.fillRect (left, roll.getRowTop (row), right - left, height - 1);
        }
    }
}

void PianoRoll::NoteGrid::paintNotes (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto area = g.getClipBounds().toFloat();
    const auto preview = roll.getPreview();
    const auto previewing = roll.transformOpen;
    const auto& [notes, colours, selected] = roll.getDrawnNotes();

    for (const auto& note : notes)
    {
        const auto bounds = roll.getNoteBounds (note);

        if (bounds.isEmpty() || ! bounds.intersects (area))
            continue;

        const auto isSelected = selected.contains (note.getRef());
        const auto found = colours.find (note.clip);
        const auto colour = found != colours.end() ? found->second : p.textMuted;
        auto fill = isSelected ? colour.brighter (0.35f) : colour;

        if (note.muted)
            fill = fill.withMultipliedSaturation (0.2f).withAlpha (0.45f);

        if (previewing && isSelected)
            fill = fill.withAlpha (0.3f); // what the transform replaces

        const auto outline = isSelected ? p.text : colour.darker (0.6f);

        if (roll.drumMode)
        {
            const auto diamond = makeDiamond (bounds);
            g.setColour (fill);
            g.fillPath (diamond);
            g.setColour (outline);
            g.strokePath (diamond, juce::PathStrokeType (isSelected ? 2.0f : 1.0f));
            continue;
        }

        g.setColour (fill);
        g.fillRoundedRectangle (bounds, noteCorner);
        g.setColour (outline);
        g.drawRoundedRectangle (bounds, noteCorner, isSelected ? 2.0f : 1.0f);

        // The note's name, where it fits.
        if (bounds.getHeight() >= 11.0f && bounds.getWidth() >= 30.0f)
        {
            g.setColour (getInkFor (fill).withAlpha (0.85f));
            g.setFont (juce::FontOptions (std::min (TextSize::small, bounds.getHeight() - 2.0f)));
            g.drawText (juce::MidiMessage::getMidiNoteName (note.pitch, true, true, 4), bounds.reduced (3.0f, 0.0f),
                        juce::Justification::centredLeft, false);
        }
    }

    // What the transform would make of the selected notes.
    for (const auto& note : preview)
    {
        const auto bounds = roll.getNoteBounds (note);

        if (bounds.isEmpty() || ! bounds.intersects (area))
            continue;

        g.setColour (p.accent.withAlpha (0.45f));

        if (roll.drumMode)
            g.fillPath (makeDiamond (bounds));
        else
            g.fillRoundedRectangle (bounds, noteCorner);

        g.setColour (p.accent);

        if (roll.drumMode)
            g.strokePath (makeDiamond (bounds), juce::PathStrokeType (1.5f));
        else
            g.drawRoundedRectangle (bounds, noteCorner, 1.5f);
    }
}

//==============================================================================
void PianoRoll::NoteGrid::updateHint (juce::Point<float> position)
{
    if (! roll.getTrack().isValid())
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        setDescription (noClipsHint);
        return;
    }

    const auto currentTool = roll.getTool();

    if (currentTool == Tool::smart || currentTool == Tool::draw)
    {
        if (const auto note = getNoteAt (position))
        {
            const auto zone = getZone (*note, position);
            setMouseCursor (zone == Zone::body ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::LeftRightResizeCursor);
            setDescription (zone == Zone::body ? bodyHint : (zone == Zone::end ? endHint : startHint));
            return;
        }

        if (getGhostClipAt (position).isValid())
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);
            setDescription ("A ghost note, from another track's clip: double-right-click it to edit that clip. A click adds a note here.");
            return;
        }
    }

    setMouseCursor (currentTool == Tool::smart || currentTool == Tool::draw ? juce::MouseCursor::NormalCursor
                                                                            : juce::MouseCursor::CrosshairCursor);
    setDescription (describeNoteTool (currentTool));
}
} // namespace phazite::ui
