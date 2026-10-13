// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SectionsStrip.h"
#include "CommandUI.h"
#include "LookAndFeel.h"
#include "model/Outlines.h"
#include "model/Sections.h"

namespace phazite::ui
{
namespace
{
    constexpr int energyBandHeight = 16; // the energy curve, above the blocks
    constexpr int edgeWidth = 6;         // dragging this close to a section's right edge resizes it
    constexpr int dragThreshold = 4;
    constexpr float corner = 3.0f;
    constexpr int barsPerNewSection = 8;

    const juce::String emptyHint ("Double-click to add a section, or right-click to lay out the song from an outline.");
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
    const auto left = juce::roundToInt (view.beatToX (section.getStartBeat()));
    const auto right = juce::roundToInt (view.beatToX (section.getEndBeat()));
    return { left, energyBandHeight, std::max (2, right - left), std::max (1, getHeight() - energyBandHeight - 2) };
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
            if (std::abs (view.beatToX (boundary) - x) < std::abs (view.beatToX (nearest) - x))
                nearest = boundary;
    }

    return nearest;
}

//==============================================================================
void SectionsStrip::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);
    g.setColour (p.border);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);

    auto* e = edit.get();

    if (e == nullptr)
        return;

    const auto sections = getSections (*e);

    if (sections.isEmpty())
    {
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (emptyHint, getLocalBounds().withTrimmedTop (energyBandHeight).reduced (Space::s, 0), juce::Justification::centredLeft,
                    true);
        return;
    }

    paintEnergyCurve (g, sections);

    const auto clip = g.getClipBounds();

    for (auto* section : sections)
    {
        const auto block = getBlockArea (*section);

        if (! block.intersects (clip))
            continue;

        const auto selected = selection.isSelected (section);
        const auto colour = getSectionColour (*section);
        const auto fill = selected ? colour.brighter (0.3f) : colour;
        const auto body = block.toFloat().reduced (0.5f);

        g.setColour (fill);
        g.fillRoundedRectangle (body, corner);
        g.setColour (selected ? p.text : colour.darker (0.5f));
        g.drawRoundedRectangle (body, corner, selected ? 2.0f : 1.0f);

        if (! (nameLabel.isVisible() && renaming.get() == section))
        {
            g.setColour (getInkFor (fill));
            g.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
            g.drawText (section->getName(), block.reduced (Space::xs, 0), juce::Justification::centredLeft, true);
        }
    }

    // While dragging: where the section would go, or where its end would be.
    if (drag.has_value() && drag->target.has_value())
    {
        const auto x = juce::roundToInt (editor.getView().beatToX (*drag->target));
        g.setColour (p.accent);

        if (drag->kind == Drag::Kind::resize)
        {
            if (auto* section = drag->section.get())
            {
                const auto end = getBlockArea (*section).getRight();
                g.setColour (p.accent.withAlpha (0.25f));
                g.fillRect (std::min (x, end), energyBandHeight, std::abs (x - end), getHeight() - energyBandHeight);
            }

            g.setColour (p.accent);
        }

        g.fillRect (x - 1, 0, 3, getHeight());
    }
}

void SectionsStrip::paintEnergyCurve (juce::Graphics& g, const juce::Array<te::ArrangerClip*>& sections)
{
    // A smooth line through each section's energy at its middle, held flat to the outer ends.
    const auto& p = getPalette (*this);
    const auto top = 2.0f, bottom = (float) energyBandHeight - 2.0f;
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

    g.setColour (p.accent.withAlpha (0.15f));
    g.fillPath (area);
    g.setColour (p.accent.withAlpha (0.8f));
    g.strokePath (line, juce::PathStrokeType (1.5f));
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

        showMenu (section);
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

    if (drag->kind == Drag::Kind::resize)
        drag->target = std::max (section->getStartBeat() + te::BeatDuration::fromBeats (1.0),
                                 editor.snap (editor.getView().xToBeat (e.position.x), e.mods));
    else
        drag->target = findNearestBoundary (*section, e.position.x);

    repaint();
}

void SectionsStrip::mouseUp (const juce::MouseEvent& e)
{
    if (! drag.has_value())
        return;

    const auto finished = *drag;
    drag.reset();
    repaint();

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
    auto& undo = ed->getUndoManager();

    if (finished.kind == Drag::Kind::resize)
    {
        if (! isNear (target, section->getEndBeat()))
        {
            undo.beginNewTransaction ("Resize Section");
            resizeSection (*section, target - section->getStartBeat(), ! e.mods.isShiftDown());
        }

        return;
    }

    const auto copy = e.mods.isCommandDown();
    const auto alias = copy && e.mods.isShiftDown();

    if (copy)
    {
        undo.beginNewTransaction (alias ? "Copy Section as Aliases" : "Copy Section");

        if (auto* copied = copySection (*section, target, alias))
            selection.selectOnly (copied);
    }
    else if (! isNear (target, section->getStartBeat()) && ! isNear (target, section->getEndBeat()))
    {
        undo.beginNewTransaction ("Move Section");
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

    addSectionAt (editor.getView().xToBeat (e.position.x));
}

//==============================================================================
void SectionsStrip::addSectionAt (te::BeatPosition beat)
{
    auto* e = edit.get();

    if (e == nullptr)
        return;

    // From the start of the bar (or the end of the section before, if that ends later), for 8 bars or up to the
    // next section: sections never overlap, which the section edits rely on.
    const auto& tempo = e->tempoSequence;
    const auto bar = std::max (0, tempo.toBarsAndBeats (tempo.toTime (beat)).bars);
    auto start = tempo.toBeats (te::tempo::BarsAndBeats { bar, {}, 0 });
    auto end = tempo.toBeats (te::tempo::BarsAndBeats { bar + barsPerNewSection, {}, 0 });
    const auto sections = getSections (*e);

    for (auto* other : sections)
        if (other->getStartBeat() <= beat && other->getEndBeat() > start)
            start = other->getEndBeat();

    for (auto* other : sections)
        if (other->getStartBeat() > beat)
            end = std::min (end, other->getStartBeat());

    if (end <= start)
        return;

    e->getUndoManager().beginNewTransaction ("Add Section");

    if (auto* section = insertSection (*e, start, { "Section " + juce::String (sections.size() + 1), end - start }))
    {
        range.clear();
        selection.selectOnly (section);
    }
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

    if (auto* section = renaming.get(); section != nullptr && name.isNotEmpty() && name != section->getName())
    {
        section->edit.getUndoManager().beginNewTransaction ("Rename Section");
        section->setName (name);
    }

    renaming = nullptr;
    repaint();
}

void SectionsStrip::showMenu (te::ArrangerClip* section)
{
    auto& registry = editor.getCommands();
    juce::PopupMenu menu;

    if (section != nullptr)
    {
        const te::SafeSelectable<te::ArrangerClip> target (section);

        menu.addItem ("Rename",
                      [this, target]
                      {
                          if (auto* s = target.get())
                              startRenaming (*s);
                      });

        menu.addItem ("Color...",
                      [this, target]
                      {
                          if (auto* s = target.get())
                              showColourMenu (*this, getSectionColour (*s),
                                              [target] (juce::Colour colour)
                                              {
                                                  if (auto* chosen = target.get())
                                                  {
                                                      chosen->edit.getUndoManager().beginNewTransaction ("Change Section Color");
                                                      chosen->setColour (colour);
                                                  }
                                              });
                      });

        menu.addSeparator();
        addCommandItems (menu, registry, { "edit.duplicate", "edit.duplicateAsAlias", "edit.delete" });
        menu.addSeparator();
    }

    addCommandItems (menu, registry, { "section.add" });

    juce::PopupMenu outlines;

    for (const auto& outline : getBuiltInOutlines())
        addCommandItem (outlines, registry, "section.outline." + outline.id);

    menu.addSubMenu ("Apply Song Outline", outlines);
    menu.showMenuAsync (juce::PopupMenu::Options());
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
