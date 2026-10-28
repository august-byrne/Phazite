// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ChordLane.h"
#include "model/arrangement/TimelineModel.h"
#include "model/project/ProjectContext.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

#include <juce_audio_basics/juce_audio_basics.h> // MidiMessage::getMidiNoteName()

namespace phazite::ui
{
namespace
{
    constexpr int dragThreshold = 4;

    const juce::String emptyHint ("The song's chords. Click to add one, or choose a progression from the menu at the left; "
                                  "parts made from the chords change with them.");
    const juce::String chordHint ("Click to change the chord, or right-click for more. ");
    const juce::String edgeHint ("Drag to make the chord longer or shorter.");

    bool isNear (BeatPosition a, BeatPosition b)
    {
        return std::abs ((a - b).inBeats()) < 1.0e-4;
    }

    bool isSameChord (const Numeral& a, const Numeral& b)
    {
        return a.degree == b.degree && a.accidental == b.accidental && a.type == b.type;
    }

    juce::String describeChoice (const Numeral& numeral, const Key& key)
    {
        return getChordNameIn (numeral, key) + "    " + describeNumeral (numeral);
    }

    // The chords a key offers, the scale's first and then a submenu of the others; choosing one calls back.
    void addChordChoices (juce::PopupMenu& menu, const std::vector<Numeral>& choices, const Key& key, const std::optional<Numeral>& current,
                          const std::function<void (Numeral)>& choose)
    {
        juce::PopupMenu outside;

        for (size_t i = 0; i < choices.size(); ++i)
        {
            auto numeral = choices[i];

            if (current.has_value())
                numeral.inversion = 0;

            const auto ticked = current.has_value() && isSameChord (*current, numeral);
            (i < 7 ? menu : outside).addItem (describeChoice (numeral, key), true, ticked, [choose, numeral] { choose (numeral); });
        }

        menu.addSubMenu ("Outside the Key", outside);
    }
} // namespace

ChordLane::ChordLane (TimelineEditor& e)
    : editor (e),
      context (e.getContext())
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Chords");
    setDescription (emptyHint);
}

void ChordLane::showProject()
{
    drag.reset();
    hovered.reset();
    repaint();
}

//==============================================================================
juce::Rectangle<int> ChordLane::getBlockArea (const LaneChord& chord) const
{
    const auto& view = editor.getView();
    const auto left = juce::roundToInt (view.beatToX (chord.range.getStart()));
    const auto right = juce::roundToInt (view.beatToX (chord.range.getEnd()));
    return { left, 0, std::max (2, right - left), getHeight() };
}

std::optional<LaneChord> ChordLane::getChordAt (juce::Point<int> position) const
{
    for (const auto& chord : getLaneChords (context))
        if (const auto block = getBlockArea (chord); position.x >= block.getX() && position.x < block.getRight())
            return chord;

    return std::nullopt;
}

bool ChordLane::isOnRightEdge (const LaneChord& chord, juce::Point<int> position) const
{
    const auto block = getBlockArea (chord);
    const auto width = std::min (ChordTint::grabDistance, block.getWidth() / 3); // narrow chords keep most of their width for clicks
    return position.x >= block.getRight() - width && position.x < block.getRight();
}

BeatRange ChordLane::getNewChordRange (BeatPosition where) const
{
    // The bar there, cut where the chords before and after it end and start.
    const auto bar = getBarAt (context, where);
    auto start = getBarStart (context, bar), end = getBarStart (context, bar + 1);

    for (const auto& chord : getLaneChords (context))
    {
        if (chord.range.getEnd() <= where)
            start = std::max (start, chord.range.getEnd());
        else if (chord.range.getStart() > where)
            end = std::min (end, chord.range.getStart());
    }

    return end > start ? BeatRange (start, end) : BeatRange (where, BeatDuration::fromBeats (1.0));
}

std::optional<BeatPosition> ChordLane::getDragTarget() const
{
    if (drag.has_value() && drag->dragging)
        return drag->target;

    return std::nullopt;
}

//==============================================================================
void ChordLane::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);
    g.setColour (p.border);
    g.fillRect (getLocalBounds().removeFromBottom (1));

    const auto chords = getLaneChords (context);

    if (chords.empty())
    {
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (emptyHint, getLocalBounds().reduced (Space::s, 0), juce::Justification::centredLeft, true);
        return;
    }

    const auto clip = g.getClipBounds();
    const juce::Font nameFont { juce::FontOptions (TextSize::small, juce::Font::bold) };
    const juce::Font numeralFont { juce::FontOptions (TextSize::small) };

    for (const auto& chord : chords)
    {
        auto shown = chord;

        if (const auto target = getDragTarget(); target.has_value() && isNear (drag->chordStart, chord.range.getStart()))
            shown.range = { chord.range.getStart(), *target };

        const auto block = getBlockArea (shown);

        if (! block.intersects (clip))
            continue;

        const auto isHovered = hovered.has_value() && isNear (*hovered, chord.range.getStart());
        const auto body = block.toFloat().reduced (0.5f, 2.0f);
        g.setColour (p.accent.withAlpha (isHovered ? ChordTint::hoveredBlock : ChordTint::block));
        g.fillRoundedRectangle (body, cornerRadius);
        g.setColour (p.accent.withAlpha (ChordTint::edge));
        g.fillRect (body.withWidth (2.0f));

        // The chord's name, then its numeral, as far as they fit.
        auto text = block.withTrimmedLeft (Space::xs + 2).withTrimmedRight (Space::xs);
        g.setColour (p.text);
        g.setFont (nameFont);
        g.drawText (chord.name, text, juce::Justification::centredLeft, true);

        text.removeFromLeft (juce::roundToInt (juce::GlyphArrangement::getStringWidth (nameFont, chord.name)) + Space::xs);

        if (text.getWidth() > Space::l)
        {
            g.setColour (p.textMuted);
            g.setFont (numeralFont);
            g.drawText (describeNumeral (chord.numeral), text, juce::Justification::centredLeft, true);
        }
    }

    // Where the key changes, after the song's start.
    const auto keys = getLaneKeyChanges (context);

    for (size_t i = 1; i < keys.size(); ++i)
    {
        const auto x = juce::roundToInt (editor.getView().beatToX (keys[i].start));
        g.setColour (p.warning.withAlpha (ChordTint::keyChange));
        g.fillRect (x - 1, 0, 2, getHeight());
    }

    // While an edge is dragged: where it would go.
    if (const auto target = getDragTarget())
    {
        g.setColour (p.accent);
        g.fillRect (juce::roundToInt (editor.getView().beatToX (*target)) - 1, 0, 3, getHeight());
    }
}

//==============================================================================
void ChordLane::mouseMove (const juce::MouseEvent& e)
{
    const auto chord = getChordAt (e.getPosition());
    const auto start = chord.has_value() ? std::optional<BeatPosition> (chord->range.getStart()) : std::nullopt;

    if (start != hovered)
    {
        hovered = start;
        repaint();
    }

    if (! chord.has_value())
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        setDescription (emptyHint);
        return;
    }

    const auto onEdge = isOnRightEdge (*chord, e.getPosition());
    setMouseCursor (onEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::PointingHandCursor);

    auto about = chord->name + ": " + describeNumeral (chord->numeral) + " in " + describeKey (chord->key) + ". ";
    setDescription (about + (onEdge ? edgeHint : chordHint));
}

void ChordLane::mouseExit (const juce::MouseEvent&)
{
    hovered.reset();
    setDescription (emptyHint);
    repaint();
}

void ChordLane::mouseDown (const juce::MouseEvent& e)
{
    drag.reset();

    if (! context.hasProject())
        return;

    if (e.mods.isPopupMenu())
    {
        getMenuAt (e.getPosition()).showMenuAsync (juce::PopupMenu::Options());
        return;
    }

    if (const auto chord = getChordAt (e.getPosition()))
        drag = Drag { chord->range.getStart(), isOnRightEdge (*chord, e.getPosition()), false, {} };
    else
        drag = Drag { editor.getView().xToBeat (e.position.x), false, false, {} };

    beginDragAutoRepeat (TimelineEditor::autoRepeatInterval); // the time line goes on scrolling while the mouse rests past an edge
}

void ChordLane::mouseDrag (const juce::MouseEvent& e)
{
    if (! drag.has_value() || ! drag->onEdge)
        return;

    if (! drag->dragging && e.getDistanceFromDragStart() < dragThreshold)
        return;

    drag->dragging = true;
    editor.autoScroll (e.getPosition(), getLocalBounds(), TimelineEditor::ScrollDirections::sideways);
    drag->target =
        std::max (drag->chordStart + BeatDuration::fromBeats (0.25), editor.snap (editor.getView().xToBeat (e.position.x), e.mods));
    repaint();
}

void ChordLane::mouseUp (const juce::MouseEvent& e)
{
    if (! drag.has_value())
        return;

    const auto finished = *drag;
    drag.reset();
    repaint();

    if (finished.dragging)
    {
        if (finished.target.has_value())
            setLaneChordEnd (context, finished.chordStart, *finished.target);

        return;
    }

    // A click: the menu of what is there.
    if (e.getDistanceFromDragStart() < dragThreshold)
        getMenuAt (e.getMouseDownPosition()).showMenuAsync (juce::PopupMenu::Options());
}

//==============================================================================
juce::PopupMenu ChordLane::getMenuAt (juce::Point<int> position)
{
    juce::PopupMenu menu;

    if (! context.hasProject())
        return menu;

    const auto where = editor.getView().xToBeat ((double) position.x);
    const auto chord = getChordAt (position);
    const auto keyFrom = chord.has_value() ? chord->range.getStart() : getBarStart (context, getBarAt (context, where));

    if (chord.has_value())
        addChordMenu (menu, *chord);
    else
        addEmptyMenu (menu, where);

    // The key from here on, and the rest of what the lane offers.
    menu.addSeparator();
    auto& ctx = context;
    const auto key = getLaneKeyAt (context, keyFrom);
    menu.addSubMenu ("Change Key from Here", getKeyChoiceMenu (key, [&ctx, keyFrom] (Key chosen) { setLaneKey (ctx, keyFrom, chosen); }));

    for (const auto& change : getLaneKeyChanges (context))
        if (change.start > BeatPosition() && isNear (change.start, keyFrom))
            menu.addItem ("Remove Key Change Here", [&ctx, keyFrom] { removeLaneKeyChange (ctx, keyFrom); });

    menu.addSeparator();
    addCommandItem (menu, editor.getCommands(), "chords.progressions");
    menu.addSubMenu ("Make a Part from the Chords", getMakePartMenu (context), canMakeChordPart (context));
    return menu;
}

void ChordLane::addChordMenu (juce::PopupMenu& menu, const LaneChord& chord)
{
    auto& ctx = context;
    const auto start = chord.range.getStart();
    const auto change = [&ctx, start] (Numeral numeral) { changeLaneChord (ctx, start, numeral); };

    menu.addSectionHeader (chord.name + ", " + describeNumeral (chord.numeral) + " in " + describeKey (chord.key));
    addChordChoices (menu, getChordChoices (context, start), chord.key, chord.numeral, change);

    // Its kind and inversion, on the same root.
    juce::PopupMenu kinds, inversions;

    for (const auto& shape : getChordShapes())
    {
        auto numeral = chord.numeral;
        numeral.type = shape.type;
        numeral.inversion = 0;
        kinds.addItem (getChordNameIn (numeral, chord.key), true, chord.numeral.type == shape.type,
                       [change, numeral] { change (numeral); });
    }

    constexpr const char* inversionNames[] = { "Root Position", "First Inversion", "Second Inversion", "Third Inversion" };
    const auto notes = std::min ((int) getChordIntervals (chord.numeral.type).size(), 4);

    for (int i = 0; i < notes; ++i)
    {
        auto numeral = chord.numeral;
        numeral.inversion = i;
        inversions.addItem (juce::String (inversionNames[i]) + ": " + getChordNameIn (numeral, chord.key), true,
                            chord.numeral.inversion == i, [change, numeral] { change (numeral); });
    }

    menu.addSubMenu ("Kind", kinds);
    menu.addSubMenu ("Inversion", inversions, notes > 1);
    menu.addSeparator();
    menu.addItem ("Delete Chord", [&ctx, start] { deleteLaneChord (ctx, start); });
}

void ChordLane::addEmptyMenu (juce::PopupMenu& menu, BeatPosition where)
{
    auto& ctx = context;
    const auto range = getNewChordRange (where);
    const auto key = getLaneKeyAt (context, range.getStart());

    menu.addSectionHeader ("Add a chord in bar " + juce::String (getBarAt (context, where) + 1) + ", in " + describeKey (key));
    addChordChoices (menu, getChordChoices (context, range.getStart()), key, std::nullopt,
                     [&ctx, range] (Numeral numeral) { addLaneChord (ctx, range, numeral); });
}

//==============================================================================
ChordLaneHeader::ChordLaneHeader (TimelineEditor& e)
    : editor (e),
      context (e.getContext())
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Chord lane");
    nameLabel.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
    addAndMakeVisible (nameLabel);

    keyButton.setTitle ("Song key");
    keyButton.setTooltip ("The song's key: the chords follow it when it changes.");
    keyButton.onClick = [this] { getKeyMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&keyButton)); };
    addAndMakeVisible (keyButton);

    moreButton.setTitle ("Chord lane menu");
    moreButton.setTooltip ("Chord progressions to try, and parts made from the chords.");
    moreButton.onClick = [this] { getMoreMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&moreButton)); };
    addAndMakeVisible (moreButton);

    update();
}

void ChordLaneHeader::update()
{
    const auto hasProject = context.hasProject();
    keyButton.setButtonText (hasProject ? describeKey (getLaneKeyAt (context, {})) : juce::String());
    keyButton.setEnabled (hasProject);
    moreButton.setEnabled (hasProject);
}

juce::PopupMenu ChordLaneHeader::getKeyMenu()
{
    auto& ctx = context;
    auto menu = getKeyChoiceMenu (getLaneKeyAt (context, {}), [&ctx] (Key key) { setLaneKey (ctx, {}, key); });
    return menu;
}

juce::PopupMenu ChordLaneHeader::getMoreMenu()
{
    juce::PopupMenu menu;
    addCommandItem (menu, editor.getCommands(), "chords.progressions");
    menu.addSeparator();
    menu.addSubMenu ("Make a Part from the Chords", getMakePartMenu (context), canMakeChordPart (context));
    return menu;
}

void ChordLaneHeader::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);
    g.setColour (p.border);
    g.fillRect (getLocalBounds().removeFromBottom (1));
    g.fillRect (getLocalBounds().removeFromRight (1));
}

void ChordLaneHeader::resized()
{
    auto area = getLocalBounds().reduced (Space::s, 2);
    moreButton.setBounds (area.removeFromRight (area.getHeight() + Space::xs));
    area.removeFromRight (Space::xs);
    nameLabel.setBounds (area.removeFromLeft (56));
    keyButton.setBounds (area);
}

//==============================================================================
juce::PopupMenu getMakePartMenu (ProjectContext& context)
{
    juce::PopupMenu menu;
    auto& ctx = context;

    for (const auto kind : { PartKind::chords, PartKind::bass, PartKind::arpeggio })
    {
        juce::PopupMenu styles;

        for (const auto style : getPartStyles())
            if (getPartKind (style) == kind)
                styles.addItem (getPartStyleName (style), [&ctx, style] { makeChordPartHere (ctx, style); });

        menu.addSubMenu (getPartKindName (kind), styles);
    }

    return menu;
}

juce::PopupMenu getKeyChoiceMenu (const Key& current, std::function<void (Key)> choose)
{
    juce::PopupMenu menu;

    for (int root = 0; root < 12; ++root)
    {
        juce::PopupMenu scales;

        for (const auto scale : getKeyScales())
        {
            const Key key { root, scale };
            scales.addItem (getScaleName (scale), true, key == current, [choose, key] { choose (key); });
        }

        menu.addSubMenu (juce::MidiMessage::getMidiNoteName (root, true, false, 0), scales, true, nullptr, current.root == root);
    }

    return menu;
}
} // namespace phazite::ui
