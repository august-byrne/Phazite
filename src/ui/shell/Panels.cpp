// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Panels.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/project/ProjectContext.h"
#include "ui/audioclip/AudioClipEditor.h"
#include "ui/input/CommandUI.h"
#include "ui/mixer/DevicesView.h"
#include "ui/pianoroll/PianoRoll.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <utility>

namespace phazite::ui
{
namespace
{
    constexpr int tabBarDepth = 32;
    constexpr int dragThreshold = 4; // how far a tab moves before it is dragged rather than clicked
    constexpr const char* dragPrefix = "phazite.view:";
    constexpr std::array allViews { View::playlist, View::mixer, View::clipEditor, View::devices };
    constexpr std::array viewNames { "playlist", "mixer", "clipEditor", "devices" };

    // A panel's tabs end before its buttons (PanelLayout.h), in the bar's top right corner; where they don't all fit, the
    // tab bar shows the rest in its menu.
    void leaveCorner (juce::TabbedComponent& tabs, int buttons)
    {
        auto& bar = tabs.getTabbedButtonBar();
        bar.setBounds (bar.getBounds().withTrimmedRight (Shell::getPanelCornerWidth (buttons) + Space::xs));
    }

    // The command that shows a tab, which lessons also name its button by (Lessons::Step::target).
    const char* getCommandID (ViewTab tab)
    {
        switch (tab)
        {
            case ViewTab::playlist:
                return "view.playlist";
            case ViewTab::mixer:
                return "view.mixer";
            case ViewTab::clipEditor:
                return "view.pianoRoll";
            case ViewTab::instrument:
                return "view.instrument";
            case ViewTab::effects:
            default:
                return "view.effects";
        }
    }

    juce::String getOtherAreaName (Panel area)
    {
        return area == Panel::song ? "the Lower Panel" : "the Upper Panel";
    }

    Panel getOtherRow (Panel row)
    {
        return row == Panel::song ? Panel::editor : Panel::song;
    }

    // A view by the name the settings and drags give it.
    std::optional<View> findView (const juce::String& name)
    {
        for (size_t i = 0; i < viewNames.size(); ++i)
            if (name == juce::String (viewNames[i]))
                return allViews[i];

        return {};
    }
} // namespace

View getView (ViewTab tab)
{
    switch (tab)
    {
        case ViewTab::playlist:
            return View::playlist;
        case ViewTab::mixer:
            return View::mixer;
        case ViewTab::clipEditor:
            return View::clipEditor;
        case ViewTab::instrument:
        case ViewTab::effects:
        default:
            return View::devices;
    }
}

//==============================================================================
ViewPlaces::Row& ViewPlaces::getRow (Panel row)
{
    return row == Panel::song ? upper : lower;
}

const ViewPlaces::Row& ViewPlaces::getRow (Panel row) const
{
    return row == Panel::song ? upper : lower;
}

ViewPlace ViewPlaces::getPlace (View view) const
{
    for (const auto row : { Panel::song, Panel::editor })
        for (int column = 0; column < 2; ++column)
            if (const auto& views = getRow (row)[(size_t) column]; std::find (views.begin(), views.end(), view) != views.end())
                return { row, column };

    return {};
}

const std::vector<View>& ViewPlaces::getViews (ViewPlace place) const
{
    return getRow (place.row)[(size_t) juce::jlimit (0, 1, place.column)];
}

bool ViewPlaces::isEmpty (Panel row) const
{
    return getRow (row)[0].empty() && getRow (row)[1].empty();
}

bool ViewPlaces::hasRoomBeside (Panel row) const
{
    return getRow (row)[0].empty() || getRow (row)[1].empty();
}

void ViewPlaces::tidy()
{
    for (auto* row : { &upper, &lower })
        if ((*row)[0].empty())
            std::swap ((*row)[0], (*row)[1]);
}

void ViewPlaces::move (View view, ViewPlace place, int index)
{
    auto& to = getRow (place.row)[(size_t) juce::jlimit (0, 1, place.column)];

    // Within an area, the index counts the views before the one moved.
    if (const auto at = std::find (to.begin(), to.end(), view); at != to.end() && index > (int) std::distance (to.begin(), at))
        --index;

    for (auto* row : { &upper, &lower })
        for (auto& views : *row)
            views.erase (std::remove (views.begin(), views.end(), view), views.end());

    to.insert (juce::isPositiveAndBelow (index, (int) to.size() + 1) ? to.begin() + index : to.end(), view);
    tidy();
}

void ViewPlaces::placeBeside (View view, Panel row, bool left)
{
    for (auto* each : { &upper, &lower })
        for (auto& views : *each)
            views.erase (std::remove (views.begin(), views.end(), view), views.end());

    tidy();
    auto& areas = getRow (row);

    // Into the row, if it has no views left; beside its one area; or into the area on that side, if it has two.
    if (areas[0].empty())
        areas[0] = { view };
    else if (! areas[1].empty())
        areas[left ? 0 : 1].push_back (view);
    else if (left)
        areas = { std::vector { view }, areas[0] };
    else
        areas[1] = { view };

    tidy();
}

juce::String ViewPlaces::toString() const
{
    const auto join = [] (const std::vector<View>& views)
    {
        juce::StringArray names;

        for (const auto view : views)
            names.add (viewNames[(size_t) view]);

        return names.joinIntoString (",");
    };

    // A row's second area only while it has views.
    const auto joinRow = [&join] (const Row& row) { return join (row[0]) + (row[1].empty() ? juce::String() : "|" + join (row[1])); };
    return joinRow (upper) + ";" + joinRow (lower);
}

ViewPlaces ViewPlaces::fromString (const juce::String& text)
{
    const auto rows = juce::StringArray::fromTokens (text, ";", {});

    if (rows.size() != 2)
        return {};

    ViewPlaces places;
    places.upper = {};
    places.lower = {};
    std::vector<View> seen;

    for (int i = 0; i < 2; ++i)
    {
        const auto areas = juce::StringArray::fromTokens (rows[i], "|", {});

        if (areas.size() > 2)
            return {};

        for (int column = 0; column < areas.size(); ++column)
        {
            for (const auto& name : juce::StringArray::fromTokens (areas[column], ",", {}))
            {
                const auto view = findView (name);

                if (! view.has_value())
                    return {};

                places.getRow (i == 0 ? Panel::song : Panel::editor)[(size_t) column].push_back (*view);
                seen.push_back (*view);
            }
        }
    }

    // Every view once.
    for (const auto view : allViews)
        if (std::count (seen.begin(), seen.end(), view) != 1)
            return {};

    places.tidy();
    return places;
}

juce::var makeViewDrag (View view)
{
    return juce::String (dragPrefix) + viewNames[(size_t) view];
}

std::optional<View> readViewDrag (const juce::var& description)
{
    const auto text = description.toString();

    if (! text.startsWith (dragPrefix))
        return {};

    return findView (text.fromFirstOccurrenceOf (dragPrefix, false, false));
}

//==============================================================================
ClipEditorView::ClipEditorView (ProjectContext& projectContext, PianoRoll& notes, AudioClipEditor& audio)
    : context (projectContext),
      pianoRoll (notes),
      audioClipEditor (audio)
{
    setTitle ("Clip editor");
    addAndMakeVisible (pianoRoll);
    addChildComponent (audioClipEditor);
    context.getSongSelection().addChangeListener (this);
}

ClipEditorView::~ClipEditorView()
{
    context.getSongSelection().removeChangeListener (this);
}

void ClipEditorView::openClip (ClipID clip)
{
    if (const auto info = getClip (context, clip))
    {
        if (info->kind == ClipType::audio)
            audioClipEditor.showClip (clip);
        else
            pianoRoll.showClip (clip);

        show (info->kind);
    }
}

TimelineEditor& ClipEditorView::getEditor() const noexcept
{
    if (showingAudio)
        return audioClipEditor;

    return pianoRoll;
}

void ClipEditorView::show (ClipType type)
{
    const auto audio = type == ClipType::audio;
    pianoRoll.setVisible (! audio);
    audioClipEditor.setVisible (audio);

    if (audio != showingAudio)
    {
        showingAudio = audio;

        if (onEditorChanged != nullptr)
            onEditorChanged();
    }
}

void ClipEditorView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (const auto type = getClipTypeToEdit (context))
        show (*type);
}

void ClipEditorView::resized()
{
    for (auto* editor : getChildren())
        editor->setBounds (getLocalBounds());
}

//==============================================================================
// A tab, which a drag takes to the other area, or along its own.
class ViewArea::TabButton : public juce::TabBarButton
{
public:
    TabButton (const juce::String& name, juce::TabbedButtonBar& bar, ViewArea& area)
        : juce::TabBarButton (name, bar),
          owner (area)
    {
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        juce::TabBarButton::mouseDrag (e);
        const auto index = getIndex();

        if (dragging || e.mods.isPopupMenu() || e.getDistanceFromDragStart() < dragThreshold
            || ! juce::isPositiveAndBelow (index, (int) owner.tabs.size()))
            return;

        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            dragging = true;
            container->startDragging (makeViewDrag (getView (owner.tabs[(size_t) index])), this,
                                      juce::ScaledImage (createComponentSnapshot (getLocalBounds())));
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        dragging = false;
        juce::TabBarButton::mouseUp (e);
    }

private:
    ViewArea& owner;
    bool dragging = false;
};

void ViewArea::Holder::resized()
{
    for (auto* view : getChildren())
        view->setBounds (getLocalBounds());
}

ViewArea::ViewArea (ViewPlace which, const Views& shown)
    : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop),
      place (which),
      views (shown)
{
    setTitle (juce::String (place.row == Panel::song ? "Upper panel" : "Lower panel") + (place.column == 0 ? "" : ", right"));
    setTabBarDepth (tabBarDepth);
    setOutline (0);

    for (auto& holder : holders)
        holder.setTitle ("View");
}

ViewArea::~ViewArea() = default;

juce::Component& ViewArea::getViewComponent (ViewTab tab) const
{
    switch (getView (tab))
    {
        case View::playlist:
            return views.playlist;
        case View::mixer:
            return views.mixer;
        case View::clipEditor:
            return views.clipEditor;
        case View::devices:
        default:
            return views.devices;
    }
}

void ViewArea::setViews (const std::vector<View>& shown)
{
    std::vector<ViewTab> newTabs;

    for (const auto view : shown)
    {
        switch (view)
        {
            case View::playlist:
                newTabs.push_back (ViewTab::playlist);
                break;
            case View::mixer:
                newTabs.push_back (ViewTab::mixer);
                break;
            case View::clipEditor:
                newTabs.push_back (ViewTab::clipEditor);
                break;
            case View::devices:
            default:
                newTabs.insert (newTabs.end(), { ViewTab::instrument, ViewTab::effects });
                break;
        }
    }

    if (newTabs == tabs)
        return;

    const auto before = getShownTab();
    clearTabs();
    tabs = std::move (newTabs);

    for (const auto tab : tabs)
        addTab (describeTab (tab).name, juce::Colours::transparentBlack, &holders[(size_t) tab], false); // each view paints its own

    refreshTabs();

    if (before.has_value() && holds (*before))
        showTab (*before);
    else if (! tabs.empty())
        setCurrentTabIndex (0);

    resized();
}

bool ViewArea::holds (ViewTab tab) const
{
    return std::find (tabs.begin(), tabs.end(), tab) != tabs.end();
}

void ViewArea::showTab (ViewTab tab)
{
    if (const auto at = std::find (tabs.begin(), tabs.end(), tab); at != tabs.end())
        setCurrentTabIndex ((int) std::distance (tabs.begin(), at));
}

std::optional<ViewTab> ViewArea::getShownTab() const
{
    if (const auto index = getCurrentTabIndex(); juce::isPositiveAndBelow (index, (int) tabs.size()))
        return tabs[(size_t) index];

    return {};
}

void ViewArea::currentTabChanged (int index, const juce::String&)
{
    if (! juce::isPositiveAndBelow (index, (int) tabs.size()))
        return;

    // The tab's view moves into its holder here (from wherever it was), and the device tabs pick the Devices view's page.
    const auto tab = tabs[(size_t) index];
    auto& holder = holders[(size_t) tab];
    holder.addAndMakeVisible (getViewComponent (tab));
    holder.resized();

    if (tab == ViewTab::instrument || tab == ViewTab::effects)
        views.devices.showPage (tab == ViewTab::instrument ? DevicesView::Page::instrument : DevicesView::Page::effects);
}

void ViewArea::refreshTooltips (const CommandBridge& commands)
{
    shortcuts = &commands;
    refreshTabs();
}

ViewArea::TabText ViewArea::describeTab (ViewTab tab) const
{
    switch (tab)
    {
        case ViewTab::playlist:
            return { "Playlist", "Playlist: the song's tracks and their clips, over time" };
        case ViewTab::mixer:
            return { "Mixer", "Mixer: a strip for each track, with its devices, sends, pan and fader" };
        case ViewTab::clipEditor:
            return views.clipEditor.showsAudioClip()
                       ? TabText { "Audio clip", "Audio clip: the selected audio clip's edges, fades, gain, pitch and tempo" }
                       : TabText { "Piano roll", "Piano roll: the notes of the selected clips" };
        case ViewTab::instrument:
            return { "Instrument", "Instrument: the selected track's instrument, with its modulators and macros" };
        case ViewTab::effects:
        default:
            return { "Effects", "Effects: the selected track's effects, as cards, one open at a time" };
    }
}

void ViewArea::refreshTabs()
{
    auto& bar = getTabbedButtonBar();

    for (int i = 0; i < (int) tabs.size(); ++i)
    {
        const auto tab = tabs[(size_t) i];
        const auto [name, about] = describeTab (tab);
        setTabName (i, name);

        if (auto* button = bar.getTabButton (i))
        {
            button->setComponentID (getCommandID (tab));
            const auto text = about + ". Drag the tab onto another panel to move it there, or onto a panel's side to put it beside";

            // The audio clip editor has no key of its own.
            const auto keyed = shortcuts != nullptr && ! (tab == ViewTab::clipEditor && views.clipEditor.showsAudioClip());
            button->setTooltip (keyed ? withShortcut (*shortcuts, text, getCommandID (tab)) : text);
        }
    }
}

juce::TabBarButton* ViewArea::createTabButton (const juce::String& name, int)
{
    return new TabButton (name, getTabbedButtonBar(), *this);
}

void ViewArea::popupMenuClickOnTab (int index, const juce::String&)
{
    if (! juce::isPositiveAndBelow (index, (int) tabs.size()))
        return;

    const auto view = getView (tabs[(size_t) index]);
    const auto other = getOtherRow (place.row);
    const juce::Component::SafePointer<ViewArea> safeThis (this);
    juce::PopupMenu menu;
    menu.addItem ("Move to " + getOtherAreaName (place.row),
                  [safeThis, view, other]
                  {
                      if (safeThis != nullptr && safeThis->onMove != nullptr)
                          safeThis->onMove (view, { other, 0 }, -1);
                  });
    menu.addItem ("Reset View Places",
                  [safeThis]
                  {
                      if (safeThis != nullptr && safeThis->onReset != nullptr)
                          safeThis->onReset();
                  });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (getTabbedButtonBar().getTabButton (index)));
}

void ViewArea::resized()
{
    juce::TabbedComponent::resized();
    leaveCorner (*this, cornerButtons); // the row's maximize and hide, and the lower row's own window, in its right area

    for (auto& holder : holders)
        holder.resized();
}

//==============================================================================
int ViewArea::getDropIndexAt (juce::Point<int> position) const
{
    auto& bar = getTabbedButtonBar();

    // Over the tabs: before the view whose tabs' middle is right of the pointer. Anywhere else: after the views.
    if (! bar.getBounds().contains (position))
        return -1;

    int index = 0;

    for (size_t i = 0; i < tabs.size(); ++i)
    {
        // Each view's tabs together: the devices' two are one view.
        auto last = i;

        while (last + 1 < tabs.size() && getView (tabs[last + 1]) == getView (tabs[i]))
            ++last;

        const auto* first = bar.getTabButton ((int) i);
        const auto* end = bar.getTabButton ((int) last);

        if (first != nullptr && end != nullptr && position.x - bar.getX() < (first->getX() + end->getRight()) / 2)
            return index;

        ++index;
        i = last;
    }

    return -1;
}

float ViewArea::getDropLineX (int index) const
{
    auto& bar = getTabbedButtonBar();
    int viewIndex = 0;

    for (size_t i = 0; i < tabs.size(); ++i)
    {
        if (i > 0 && getView (tabs[i]) == getView (tabs[i - 1]))
            continue;

        if (viewIndex++ == index)
            if (const auto* button = bar.getTabButton ((int) i))
                return (float) (bar.getX() + button->getX());
    }

    // After the views: past the last tab.
    if (const auto* last = tabs.empty() ? nullptr : bar.getTabButton ((int) tabs.size() - 1))
        return (float) (bar.getX() + last->getRight());

    return (float) bar.getX();
}

void ViewArea::setDropSides (bool beside, bool otherRow)
{
    canGoBeside = beside;
    canMakeRow = otherRow;
}

void ViewArea::setCornerButtons (int buttons)
{
    if (std::exchange (cornerButtons, buttons) != buttons)
        resized();
}

ViewArea::Drop ViewArea::getDropAt (juce::Point<int> position) const
{
    // Over the tabs, among them; on the views, a quarter of the way in from their left or right edge, beside this area, while
    // its row has room, and from its edge toward a row without views, into that row; anywhere else, among the tabs too.
    if (getTabbedButtonBar().getBounds().contains (position))
        return Drop::tabs;

    const auto content = getLocalBounds().withTrimmedTop (getTabBarDepth());

    if (canGoBeside && position.x < content.getX() + content.getWidth() / 4)
        return Drop::left;

    if (canGoBeside && position.x >= content.getRight() - content.getWidth() / 4)
        return Drop::right;

    const auto towardOtherRow = place.row == Panel::song ? position.y >= content.getBottom() - content.getHeight() / 4
                                                         : position.y < content.getY() + content.getHeight() / 4;
    return canMakeRow && towardOtherRow ? Drop::otherRow : Drop::tabs;
}

void ViewArea::paintOverChildren (juce::Graphics& g)
{
    if (! drop.has_value())
        return;

    const auto accent = getPalette (*this).accent;
    const auto content = getLocalBounds().withTrimmedTop (getTabBarDepth());
    g.setColour (accent.withAlpha (ViewDropTint::wash));

    // Beside it, or in the other row: the half of it where the view would go, tinted, with a line along its edge.
    if (drop != Drop::tabs)
    {
        const auto half = drop == Drop::left         ? content.withWidth (content.getWidth() / 2)
                          : drop == Drop::right      ? content.withTrimmedLeft (content.getWidth() / 2)
                          : place.row == Panel::song ? content.withTrimmedTop (content.getHeight() / 2)
                                                     : content.withHeight (content.getHeight() / 2);
        g.fillRect (half);
        g.setColour (accent);
        g.drawRect (half.toFloat(), ViewDropTint::line);
        return;
    }

    // Among the tabs: the whole area tinted, and a line in the tab bar where the tab would go.
    g.fillRect (getLocalBounds());
    g.setColour (accent);
    const auto bar = getTabbedButtonBar().getBounds().toFloat();
    g.fillRect (juce::Rectangle<float> (getDropLineX (dropIndex.value_or (-1)) - ViewDropTint::line / 2.0f, bar.getY(), ViewDropTint::line,
                                        bar.getHeight()));
}

bool ViewArea::isInterestedInDragSource (const SourceDetails& details)
{
    return readViewDrag (details.description).has_value();
}

void ViewArea::itemDragEnter (const SourceDetails& details)
{
    itemDragMove (details);
}

void ViewArea::itemDragMove (const SourceDetails& details)
{
    const auto at = getDropAt (details.localPosition);
    const auto index = at == Drop::tabs ? std::optional (getDropIndexAt (details.localPosition)) : std::nullopt;

    if (at != drop || index != dropIndex)
    {
        drop = at;
        dropIndex = index;
        repaint();
    }
}

void ViewArea::itemDragExit (const SourceDetails&)
{
    drop.reset();
    dropIndex.reset();
    repaint();
}

void ViewArea::itemDropped (const SourceDetails& details)
{
    const auto at = drop.value_or (getDropAt (details.localPosition));
    const auto index = dropIndex.value_or (getDropIndexAt (details.localPosition));
    drop.reset();
    dropIndex.reset();
    repaint();

    const auto view = readViewDrag (details.description);

    if (! view.has_value())
        return;

    if ((at == Drop::left || at == Drop::right) && onPlaceBeside != nullptr)
        onPlaceBeside (*view, place.row, at == Drop::left);
    else if (at == Drop::otherRow && onMove != nullptr)
        onMove (*view, { getOtherRow (place.row), 0 }, -1);
    else if (onMove != nullptr)
        onMove (*view, place, index);
}

//==============================================================================
ViewRow::ViewRow (Panel which, const ViewArea::Views& views)
    : row (which),
      divider (which == Panel::song ? "Line between the upper areas" : "Line between the lower areas", true)
{
    setTitle (row == Panel::song ? "Upper row" : "Lower row");

    for (int column = 0; column < 2; ++column)
    {
        areas[(size_t) column] = std::make_unique<ViewArea> (ViewPlace { row, column }, views);
        addChildComponent (*areas[(size_t) column]);
    }

    // Dragged, the line gives one area more of the row and the other less, each keeping its least.
    divider.onDragStart = [this] { shareAtDragStart = share; };
    divider.onDrag = [this] (int distance)
    {
        const auto room = std::max (1, getWidth() - Shell::dividerSize);
        const auto least = std::min (0.5, (double) Shell::minimumAreaWidth / room);
        share = juce::jlimit (least, 1.0 - least, shareAtDragStart + (double) distance / room);
        resized();

        if (onShareChanged != nullptr)
            onShareChanged();
    };

    addChildComponent (divider);
}

void ViewRow::setViews (const ViewPlaces::Row& views)
{
    for (size_t column = 0; column < 2; ++column)
    {
        areas[column]->setViews (views[column]);
        areas[column]->setVisible (! views[column].empty());
    }

    resized();
}

bool ViewRow::isEmpty() const noexcept
{
    return areas[0]->isEmpty() && areas[1]->isEmpty();
}

bool ViewRow::holds (ViewTab tab) const
{
    return areas[0]->holds (tab) || areas[1]->holds (tab);
}

ViewArea& ViewRow::getAreaOf (ViewTab tab)
{
    return areas[1]->holds (tab) ? *areas[1] : *areas[0];
}

void ViewRow::resized()
{
    // Two areas side by side at their shares, with the line between them; or one, the whole row. The row's panel buttons
    // are in the right one's corner: the upper row's maximize and hide, the lower one's its own window too.
    const auto both = ! areas[0]->isEmpty() && ! areas[1]->isEmpty();
    const auto buttons = row == Panel::song ? 2 : 3;
    auto area = getLocalBounds();
    divider.setVisible (both);

    if (both)
    {
        const auto room = std::max (0, area.getWidth() - Shell::dividerSize);
        areas[0]->setBounds (area.removeFromLeft (juce::roundToInt (share * room)));
        divider.setBounds (area.removeFromLeft (Shell::dividerSize));
        areas[1]->setBounds (area);
        areas[0]->setCornerButtons (0);
        areas[1]->setCornerButtons (buttons);
        return;
    }

    for (auto& each : areas)
    {
        each->setBounds (area);
        each->setCornerButtons (buttons);
    }
}
} // namespace phazite::ui
