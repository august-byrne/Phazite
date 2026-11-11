// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SectionsStrip.h"
#include "ChordLane.h"
#include "model/arrangement/Outlines.h"
#include "model/transport/TempoModel.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int edgeWidth = 6; // dragging this close to a section's right edge resizes it
    constexpr int dragThreshold = 4;
    constexpr float corner = 3.0f;

    const juce::String emptyHint ("The song's sections. Double-click to add one here, or right-click to lay out the song from an outline.");
    const juce::String edgeHint ("Drag to make the section longer or shorter; what comes after it moves along. "
                                 "Hold Shift to resize the section alone, and Alt to place its end freely.");
    const juce::String moveHint ("Drag to move it with everything in it; Ctrl+drag copies it, and Ctrl+Shift+drag copies it as aliases. "
                                 "Double-click to rename it.");

    bool isNear (BeatPosition a, BeatPosition b)
    {
        return std::abs ((a - b).inBeats()) < 1.0e-4;
    }
} // namespace

SectionsStrip::SectionsStrip (TimelineEditor& e)
    : editor (e),
      context (e.getContext()),
      selection (context.getSongSelection()),
      range (context.getSongRange())
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Sections");
    setDescription (emptyHint);
    selection.addChangeListener (this);

    // Enter or clicking elsewhere keeps the new name; Esc keeps the old one. JUCE calls onEditorHide before it
    // takes the typed name, and onTextChange after, so the name is kept there.
    nameLabel.setTitle ("Section name");
    nameLabel.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
    nameLabel.onTextChange = [this] { renamed(); };
    nameLabel.onEditorHide = [this]
    {
        nameLabel.setVisible (false);
        repaint();
    };
    addChildComponent (nameLabel);
}

SectionsStrip::~SectionsStrip()
{
    selection.removeChangeListener (this);
}

void SectionsStrip::showProject()
{
    // The other project's sections can have the same IDs.
    renaming = {};
    nameLabel.hideEditor (true);
    drag.reset();
    repaint();
}

//==============================================================================
juce::Rectangle<int> SectionsStrip::getBlockArea (const SongSection& section) const
{
    const auto& view = editor.getView();
    const auto left = view.beatToColumn (section.range.getStart());
    const auto right = view.beatToColumn (section.range.getEnd());
    return { left, 0, std::max (2, right - left), getHeight() };
}

std::optional<SongSection> SectionsStrip::getSectionAt (juce::Point<int> position) const
{
    for (const auto& section : getSongSections (context))
        if (const auto block = getBlockArea (section); position.x >= block.getX() && position.x < block.getRight())
            return section;

    return std::nullopt;
}

bool SectionsStrip::isOnRightEdge (const SongSection& section, juce::Point<int> position) const
{
    const auto block = getBlockArea (section);
    const auto width = std::min (edgeWidth, block.getWidth() / 4); // narrow sections keep most of their width for moving
    return position.x >= block.getRight() - width && position.x < block.getRight();
}

BeatPosition SectionsStrip::findNearestBoundary (const SongSection& section, double x) const
{
    const auto& view = editor.getView();
    auto nearest = section.range.getStart();

    for (const auto boundary : getSectionBoundaries (context))
        if (std::abs (view.beatToX (boundary) - x) < std::abs (view.beatToX (nearest) - x))
            nearest = boundary;

    return nearest;
}

//==============================================================================
void SectionsStrip::paint (juce::Graphics& g)
{
    // Part of the time bar, as the ruler under it is.
    const auto& p = getPalette (*this);
    g.fillAll (p.surfaceRaised);

    const auto sections = getSongSections (context);

    if (sections.empty())
        return; // the hint bar says what to do here

    const auto clip = g.getClipBounds();

    // Each section a light block in its color, with its name: no lines (the owner, 2026-11-06), the gap between blocks marks
    // where one ends.
    for (const auto& section : sections)
    {
        const auto block = getBlockArea (section);

        if (! block.intersects (clip))
            continue;

        const auto selected = selection.isSelected (section.id);
        const auto body = block.toFloat().reduced (0.5f, 1.5f);

        g.setColour (section.colour.withAlpha (selected ? 0.6f : 0.3f));
        g.fillRoundedRectangle (body, corner);

        if (selected)
        {
            g.setColour (p.text);
            g.drawRoundedRectangle (body, corner, 1.0f);
        }

        if (! (nameLabel.isVisible() && renaming == section.id))
        {
            g.setColour (p.text);
            g.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
            g.drawText (section.name, block.withTrimmedLeft (Space::xs + 2).withTrimmedRight (Space::xs), juce::Justification::centredLeft,
                        true);
        }
    }

    // While dragging: where the section would go, or where its end would be.
    if (const auto target = getDragTarget())
    {
        const auto x = editor.getView().beatToColumn (*target);

        if (const auto section = getSongSection (context, drag->section); section.has_value() && drag->kind == Drag::Kind::resize)
        {
            const auto end = getBlockArea (*section).getRight();
            g.setColour (p.accent.withAlpha (0.25f));
            g.fillRect (std::min (x, end), 0, std::abs (x - end), getHeight());
        }

        g.setColour (p.accent);
        g.fillRect (x - 1, 0, 3, getHeight());
    }
}

//==============================================================================
void SectionsStrip::mouseMove (const juce::MouseEvent& e)
{
    // The hint bar shows the section's tip, and what the mouse does here.
    if (const auto section = getSectionAt (e.getPosition()))
    {
        const auto onEdge = isOnRightEdge (*section, e.getPosition());
        setMouseCursor (onEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor);
        setDescription ((section->tip.isNotEmpty() ? section->name + ": " + section->tip + " " : juce::String())
                        + (onEdge ? edgeHint : moveHint));
        return;
    }

    setMouseCursor (juce::MouseCursor::NormalCursor);
    setDescription (emptyHint);
}

void SectionsStrip::mouseExit (const juce::MouseEvent&)
{
    setDescription (emptyHint);
}

void SectionsStrip::mouseDown (const juce::MouseEvent& e)
{
    nameLabel.hideEditor (false); // keeps a name being typed
    drag.reset();
    const auto section = getSectionAt (e.getPosition());

    if (e.mods.isPopupMenu())
    {
        if (section.has_value() && ! selection.isSelected (section->id))
            selection.selectOnly (section->id);

        getMenu (section, editor.getView().xToBeat (e.position.x)).showMenuAsync (juce::PopupMenu::Options());
        return;
    }

    if (! section.has_value())
    {
        if (! selection.getSections().empty())
            selection.clear();

        return;
    }

    // A plain click selects the section at once, so a drag takes it; Ctrl waits to see whether it is a
    // click (adding it to the selection) or a drag (copying it).
    range.clear();

    if (! e.mods.isCommandDown())
        selection.selectOnly (section->id);

    drag = Drag { Drag::Kind::pending, section->id, isOnRightEdge (*section, e.getPosition()), {} };
    beginDragAutoRepeat (TimelineEditor::autoRepeatInterval); // the time line goes on scrolling while the mouse rests past an edge
}

void SectionsStrip::mouseDrag (const juce::MouseEvent& e)
{
    if (! drag.has_value())
        return;

    if (drag->kind == Drag::Kind::pending)
    {
        if (e.getDistanceFromDragStart() < dragThreshold)
            return;

        drag->kind = drag->onEdge ? Drag::Kind::resize : Drag::Kind::move;
    }

    const auto section = getSongSection (context, drag->section);

    if (! section.has_value())
        return;

    // Past an edge of what shows of it (its window, a pixel narrower), the time line scrolls along.
    editor.autoScroll (e, getShownTimeline (*this).toNearestInt(), TimelineEditor::ScrollDirections::sideways);

    if (drag->kind == Drag::Kind::resize)
        drag->target = std::max (section->range.getStart() + BeatDuration::fromBeats (1.0),
                                 editor.snap (editor.getView().xToBeat (e.position.x), e.mods));
    else
        drag->target = findNearestBoundary (*section, e.position.x);

    dragChanged();
}

void SectionsStrip::mouseUp (const juce::MouseEvent& e)
{
    if (! drag.has_value())
        return;

    const auto finished = *drag;
    drag.reset();
    dragChanged();

    const auto section = getSongSection (context, finished.section);

    if (! section.has_value())
        return;

    if (finished.kind == Drag::Kind::pending)
    {
        // A Ctrl-click adds the section to the selection, or takes it out.
        if (e.mods.isCommandDown())
        {
            if (selection.isSelected (section->id))
                selection.remove (section->id);
            else
                selection.add (section->id);
        }

        return;
    }

    if (! finished.target.has_value())
        return;

    const auto target = *finished.target;

    if (finished.kind == Drag::Kind::resize)
    {
        if (! isNear (target, section->range.getEnd()))
            resizeSection (context, section->id, target - section->range.getStart(), ! e.mods.isShiftDown());

        return;
    }

    const auto copy = e.mods.isCommandDown();
    const auto alias = copy && e.mods.isShiftDown();

    if (copy)
    {
        if (const auto copied = copySection (context, section->id, target, alias); copied.isValid())
            selection.selectOnly (copied);
    }
    else if (! isNear (target, section->range.getStart()) && ! isNear (target, section->range.getEnd()))
    {
        moveSection (context, section->id, target);
    }
}

void SectionsStrip::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (const auto section = getSectionAt (e.getPosition()))
    {
        startRenaming (section->id);
        return;
    }

    addSectionAt (editor.getView().xToBeat (e.position.x));
}

//==============================================================================
void SectionsStrip::addSectionAt (BeatPosition beat)
{
    if (phazite::addSectionAt (context, beat).isValid())
        range.clear();
}

juce::PopupMenu SectionsStrip::getMenuAt (BeatPosition where)
{
    // As a right-click on the section there would.
    std::optional<SongSection> section;

    for (const auto& each : getSongSections (context))
        if (each.range.contains (where))
            section = each;

    if (section.has_value() && ! selection.isSelected (section->id))
        selection.selectOnly (section->id);

    return getMenu (section, where);
}

void SectionsStrip::showMenuAt (BeatPosition where)
{
    getMenuAt (where).showMenuAsync (juce::PopupMenu::Options());
}

void SectionsStrip::startRenaming (SectionID id)
{
    const auto section = getSongSection (context, id);

    if (! section.has_value())
        return;

    nameLabel.hideEditor (false);
    renaming = id;

    nameLabel.setText (section->name, juce::dontSendNotification);
    placeNameField (*section);
    nameLabel.setVisible (true);
    nameLabel.showEditor();
    repaint();
}

void SectionsStrip::viewChanged()
{
    if (! nameLabel.isVisible())
        return;

    if (const auto section = getSongSection (context, renaming))
        placeNameField (*section);
}

void SectionsStrip::placeNameField (const SongSection& section)
{
    const auto block = getBlockArea (section);
    nameLabel.setBounds (block.withWidth (std::max (120, block.getWidth())));
}

void SectionsStrip::renamed()
{
    renameSection (context, renaming, nameLabel.getText().trim());
    renaming = {};
    repaint();
}

juce::PopupMenu SectionsStrip::getMenu (const std::optional<SongSection>& section, BeatPosition where)
{
    auto& bridge = editor.getCommands();
    const juce::Component::SafePointer<SectionsStrip> safeThis (this);
    juce::PopupMenu menu;

    if (section.has_value())
    {
        const auto id = section->id;

        menu.addItem ("Rename",
                      [safeThis, id]
                      {
                          if (safeThis != nullptr)
                              safeThis->startRenaming (id);
                      });

        menu.addItem ("Color...",
                      [safeThis, id]
                      {
                          if (safeThis == nullptr)
                              return;

                          if (const auto shown = getSongSection (safeThis->context, id))
                              showColourMenu (*safeThis, shown->colour,
                                              [safeThis, id] (juce::Colour colour)
                                              {
                                                  if (safeThis != nullptr)
                                                      setSectionColour (safeThis->context, id, colour);
                                              });
                      });

        menu.addSeparator();
        addCommandItem (menu, bridge, "section.loop");
        menu.addSeparator();
        addCommandItems (menu, bridge, { "edit.duplicate", "edit.duplicateAsAlias", "edit.delete" });
        menu.addSeparator();
    }

    menu.addItem ("Add Section Here",
                  [safeThis, where]
                  {
                      if (safeThis != nullptr)
                          safeThis->addSectionAt (where);
                  });

    // A chord there: a song's first shows the chord lane. And a tempo change there, on the grid, at the tempo there, which
    // shows the tempo lane.
    menu.addSubMenu ("Add Chord Here", getAddChordMenu (context, where));
    auto& ctx = context;
    const auto tempoBeat = std::max (BeatPosition(), editor.snap (where, {}));
    menu.addItem ("Add Tempo Change Here", context.hasProject(), false, [&ctx, tempoBeat] { addTempoChange (ctx, tempoBeat); });

    // Each outline lays out the whole song; it replaces the sections there are.
    juce::PopupMenu outlines;

    for (const auto& outline : getBuiltInOutlines())
        addCommandItem (outlines, bridge, getOutlineCommandID (outline.id));

    menu.addSubMenu ("Song Outline", outlines);

    // Where playback loops: the whole song, unless something else is looped on purpose.
    menu.addSeparator();
    addCommandItems (menu, bridge, { "transport.loop", "transport.loopSong", "transport.loopSelection" });
    return menu;
}

std::optional<BeatPosition> SectionsStrip::getDragTarget() const
{
    if (drag.has_value() && drag->kind != Drag::Kind::pending)
        return drag->target;

    return std::nullopt;
}

void SectionsStrip::dragChanged()
{
    repaint();

    if (onDragChanged != nullptr)
        onDragChanged();
}
} // namespace phazite::ui
