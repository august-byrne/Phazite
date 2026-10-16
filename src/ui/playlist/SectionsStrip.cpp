// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SectionsStrip.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/Sections.h"
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

    bool isNear (te::BeatPosition a, te::BeatPosition b)
    {
        return std::abs ((a - b).inBeats()) < 1.0e-4;
    }
} // namespace

SectionsStrip::SectionsStrip (TimelineEditor& e, te::SelectionManager& s, RangeSelection& r)
    : editor (e),
      selection (s),
      range (r)
{
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
    editState.removeListener (this);
}

void SectionsStrip::setEdit (te::Edit* newEdit)
{
    if (newEdit == edit.get() && editState.isValid() == (newEdit != nullptr))
        return;

    nameLabel.hideEditor (true);
    drag.reset();
    editState.removeListener (this);
    edit = newEdit;
    editState = newEdit != nullptr ? newEdit->state : juce::ValueTree();
    editState.addListener (this);
    repaint();
}

//==============================================================================
juce::Rectangle<int> SectionsStrip::getBlockArea (const te::Clip& section) const
{
    const auto& view = editor.getView();
    const auto left = juce::roundToInt (view.beatToX (fromTracktion (section.getStartBeat())));
    const auto right = juce::roundToInt (view.beatToX (fromTracktion (section.getEndBeat())));
    return { left, 0, std::max (2, right - left), getHeight() };
}

te::ArrangerClip* SectionsStrip::getSectionAt (juce::Point<int> position) const
{
    if (auto* e = edit.get())
        for (auto* section : getSections (*e))
            if (const auto block = getBlockArea (*section); position.x >= block.getX() && position.x < block.getRight())
                return section;

    return nullptr;
}

bool SectionsStrip::isOnRightEdge (const te::Clip& section, juce::Point<int> position) const
{
    const auto block = getBlockArea (section);
    const auto width = std::min (edgeWidth, block.getWidth() / 4); // narrow sections keep most of their width for moving
    return position.x >= block.getRight() - width && position.x < block.getRight();
}

te::BeatPosition SectionsStrip::findNearestBoundary (const te::ArrangerClip& section, double x) const
{
    auto nearest = section.getStartBeat();

    if (auto* e = edit.get())
    {
        const auto& view = editor.getView();

        for (const auto boundary : getSectionBoundaries (*e))
            if (std::abs (view.beatToX (fromTracktion (boundary)) - x) < std::abs (view.beatToX (fromTracktion (nearest)) - x))
                nearest = boundary;
    }

    return nearest;
}

//==============================================================================
void SectionsStrip::paint (juce::Graphics& g)
{
    // Part of the time bar, as the ruler under it is.
    const auto& p = getPalette (*this);
    g.fillAll (p.surfaceRaised);

    auto* e = edit.get();

    if (e == nullptr)
        return;

    const auto sections = getSections (*e);

    if (sections.isEmpty())
        return; // the hint bar says what to do here

    paintEnergyCurve (g, sections);

    const auto clip = g.getClipBounds();

    // Each section a light block in its color, solid where it starts, with its name.
    for (auto* section : sections)
    {
        const auto block = getBlockArea (*section);

        if (! block.intersects (clip))
            continue;

        const auto selected = selection.isSelected (section);
        const auto colour = getSectionColour (*section);
        const auto body = block.toFloat().reduced (0.5f, 1.5f);

        g.setColour (colour.withAlpha (selected ? 0.6f : 0.3f));
        g.fillRoundedRectangle (body, corner);
        g.setColour (colour);
        g.fillRect (body.withWidth (2.0f));

        if (selected)
        {
            g.setColour (p.text);
            g.drawRoundedRectangle (body, corner, 1.0f);
        }

        if (! (nameLabel.isVisible() && renaming.get() == section))
        {
            g.setColour (p.text);
            g.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
            g.drawText (section->getName(), block.withTrimmedLeft (Space::xs + 2).withTrimmedRight (Space::xs),
                        juce::Justification::centredLeft, true);
        }
    }

    // While dragging: where the section would go, or where its end would be.
    if (const auto target = getDragTarget())
    {
        const auto x = juce::roundToInt (editor.getView().beatToX (fromTracktion (*target)));

        if (auto* section = drag->section.get(); section != nullptr && drag->kind == Drag::Kind::resize)
        {
            const auto end = getBlockArea (*section).getRight();
            g.setColour (p.accent.withAlpha (0.25f));
            g.fillRect (std::min (x, end), 0, std::abs (x - end), getHeight());
        }

        g.setColour (p.accent);
        g.fillRect (x - 1, 0, 3, getHeight());
    }
}

void SectionsStrip::paintEnergyCurve (juce::Graphics& g, const juce::Array<te::ArrangerClip*>& sections)
{
    // A smooth line through each section's energy at its middle, held flat to the outer ends: faint, behind
    // the names.
    const auto& p = getPalette (*this);
    const auto top = 2.0f, bottom = (float) getHeight() - 2.0f;
    const auto yFor = [=] (const te::Clip& section) { return bottom - (bottom - top) * getSectionEnergy (section); };

    juce::Path line;
    const auto first = getBlockArea (*sections.getFirst()).toFloat();
    line.startNewSubPath (first.getX(), yFor (*sections.getFirst()));
    juce::Point<float> previous (first.getCentreX(), yFor (*sections.getFirst()));
    line.lineTo (previous);

    for (int i = 1; i < sections.size(); ++i)
    {
        const juce::Point<float> next (getBlockArea (*sections[i]).toFloat().getCentreX(), yFor (*sections[i]));
        const auto midX = (previous.x + next.x) / 2.0f;
        line.cubicTo (midX, previous.y, midX, next.y, next.x, next.y);
        previous = next;
    }

    line.lineTo ((float) getBlockArea (*sections.getLast()).getRight(), previous.y);

    auto area = line;
    area.lineTo ((float) getBlockArea (*sections.getLast()).getRight(), bottom + 2.0f);
    area.lineTo (first.getX(), bottom + 2.0f);
    area.closeSubPath();

    g.setColour (p.accent.withAlpha (0.08f));
    g.fillPath (area);
    g.setColour (p.accent.withAlpha (0.4f));
    g.strokePath (line, juce::PathStrokeType (1.0f));
}

//==============================================================================
void SectionsStrip::mouseMove (const juce::MouseEvent& e)
{
    // The hint bar shows the section's tip, and what the mouse does here.
    if (auto* section = getSectionAt (e.getPosition()))
    {
        const auto onEdge = isOnRightEdge (*section, e.getPosition());
        const auto tip = getSectionTip (*section);
        setMouseCursor (onEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor);
        setDescription ((tip.isNotEmpty() ? section->getName() + ": " + tip + " " : juce::String()) + (onEdge ? edgeHint : moveHint));
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
    auto* section = getSectionAt (e.getPosition());

    if (e.mods.isPopupMenu())
    {
        if (section != nullptr && ! selection.isSelected (section))
            selection.selectOnly (section);

        showMenu (section, toTracktion (editor.getView().xToBeat (e.position.x)));
        return;
    }

    if (section == nullptr)
    {
        if (! getSelectedSections (selection).isEmpty())
            selection.deselectAll();

        return;
    }

    // A plain click selects the section at once, so a drag takes it; Ctrl waits to see whether it is a
    // click (adding it to the selection) or a drag (copying it).
    range.clear();

    if (! e.mods.isCommandDown())
        selection.selectOnly (section);

    drag = Drag { Drag::Kind::pending, section, isOnRightEdge (*section, e.getPosition()), {} };
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

    auto* section = drag->section.get();

    if (section == nullptr)
        return;

    // Past an edge, the time line scrolls along.
    editor.autoScroll (e.getPosition(), getLocalBounds(), TimelineEditor::ScrollDirections::sideways);

    if (drag->kind == Drag::Kind::resize)
        drag->target = std::max (section->getStartBeat() + te::BeatDuration::fromBeats (1.0),
                                 toTracktion (editor.snap (editor.getView().xToBeat (e.position.x), e.mods)));
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

    auto* section = finished.section.get();
    auto* ed = edit.get();

    if (section == nullptr || ed == nullptr)
        return;

    if (finished.kind == Drag::Kind::pending)
    {
        // A Ctrl-click adds the section to the selection, or takes it out.
        if (e.mods.isCommandDown())
        {
            if (selection.isSelected (section))
                selection.deselect (section);
            else
                selection.addToSelection (section);
        }

        return;
    }

    if (! finished.target.has_value())
        return;

    const auto target = *finished.target;

    if (finished.kind == Drag::Kind::resize)
    {
        if (! isNear (target, section->getEndBeat()))
            resizeSection (*section, target - section->getStartBeat(), ! e.mods.isShiftDown());

        return;
    }

    const auto copy = e.mods.isCommandDown();
    const auto alias = copy && e.mods.isShiftDown();

    if (copy)
    {
        if (auto* copied = copySection (*section, target, alias))
            selection.selectOnly (copied);
    }
    else if (! isNear (target, section->getStartBeat()) && ! isNear (target, section->getEndBeat()))
    {
        moveSection (*section, target);
    }
}

void SectionsStrip::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (auto* section = getSectionAt (e.getPosition()))
    {
        startRenaming (*section);
        return;
    }

    addSectionAt (toTracktion (editor.getView().xToBeat (e.position.x)));
}

//==============================================================================
void SectionsStrip::addSectionAt (te::BeatPosition beat)
{
    if (auto* e = edit.get(); e != nullptr && phazite::addSectionAt (*e, beat, &selection) != nullptr)
        range.clear();
}

void SectionsStrip::startRenaming (te::ArrangerClip& section)
{
    nameLabel.hideEditor (false);
    renaming = &section;

    const auto block = getBlockArea (section);
    nameLabel.setText (section.getName(), juce::dontSendNotification);
    nameLabel.setBounds (block.withWidth (std::max (120, block.getWidth())));
    nameLabel.setVisible (true);
    nameLabel.showEditor();
    repaint();
}

void SectionsStrip::renamed()
{
    const auto name = nameLabel.getText().trim();

    if (auto* section = renaming.get())
        renameSection (*section, name);

    renaming = nullptr;
    repaint();
}

void SectionsStrip::showMenu (te::ArrangerClip* section, te::BeatPosition where)
{
    auto& bridge = editor.getCommands();
    const juce::Component::SafePointer<SectionsStrip> safeThis (this);
    juce::PopupMenu menu;

    if (section != nullptr)
    {
        const te::SafeSelectable<te::ArrangerClip> target (section);

        menu.addItem ("Rename",
                      [safeThis, target]
                      {
                          if (auto* s = target.get(); s != nullptr && safeThis != nullptr)
                              safeThis->startRenaming (*s);
                      });

        menu.addItem ("Color...",
                      [safeThis, target]
                      {
                          if (auto* s = target.get(); s != nullptr && safeThis != nullptr)
                              showColourMenu (*safeThis, getSectionColour (*s),
                                              [target] (juce::Colour colour)
                                              {
                                                  if (auto* chosen = target.get())
                                                      setSectionColour (*chosen, colour);
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

    // Each outline lays out the whole song; it replaces the sections there are.
    juce::PopupMenu outlines;

    for (const auto& outline : getBuiltInOutlines())
        addCommandItem (outlines, bridge, getOutlineCommandID (outline.id));

    menu.addSubMenu ("Song Outline", outlines);

    // Where playback loops: the whole song, unless something else is looped on purpose.
    menu.addSeparator();
    addCommandItems (menu, bridge, { "transport.loop", "transport.loopSong", "transport.loopSelection" });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

std::optional<te::BeatPosition> SectionsStrip::getDragTarget() const
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

//==============================================================================
void SectionsStrip::sectionsChanged (const juce::ValueTree& tree)
{
    // A section, or the tempo, which sets where in time they are.
    for (auto t = tree; t.isValid(); t = t.getParent())
    {
        if (t.hasType (te::IDs::ARRANGERTRACK) || t.hasType (te::IDs::TEMPOSEQUENCE))
        {
            repaint();
            return;
        }
    }
}

void SectionsStrip::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    sectionsChanged (tree);
}

void SectionsStrip::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    sectionsChanged (parent);
}

void SectionsStrip::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int)
{
    sectionsChanged (parent);
}
} // namespace phazite::ui
