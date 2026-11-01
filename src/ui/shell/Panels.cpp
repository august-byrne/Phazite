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

namespace phazite::ui
{
namespace
{
    constexpr int tabBarDepth = 32;
    constexpr int routingButtonWidth = 80;
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
Panel ViewPlaces::getArea (View view) const
{
    return std::find (upper.begin(), upper.end(), view) != upper.end() ? Panel::song : Panel::editor;
}

const std::vector<View>& ViewPlaces::getViews (Panel area) const
{
    return area == Panel::song ? upper : lower;
}

void ViewPlaces::move (View view, Panel area, int index)
{
    auto& to = area == Panel::song ? upper : lower;

    // Within an area, the index counts the views before the one moved.
    if (const auto at = std::find (to.begin(), to.end(), view); at != to.end() && index > (int) std::distance (to.begin(), at))
        --index;

    for (auto* views : { &upper, &lower })
        views->erase (std::remove (views->begin(), views->end(), view), views->end());

    to.insert (juce::isPositiveAndBelow (index, (int) to.size() + 1) ? to.begin() + index : to.end(), view);
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

    return join (upper) + ";" + join (lower);
}

ViewPlaces ViewPlaces::fromString (const juce::String& text)
{
    const auto areas = juce::StringArray::fromTokens (text, ";", {});

    if (areas.size() != 2)
        return {};

    ViewPlaces places;
    places.upper.clear();
    places.lower.clear();

    for (int i = 0; i < 2; ++i)
    {
        for (const auto& name : juce::StringArray::fromTokens (areas[i], ",", {}))
        {
            const auto view = findView (name);

            if (! view.has_value())
                return {};

            (i == 0 ? places.upper : places.lower).push_back (*view);
        }
    }

    // Every view once.
    for (const auto view : allViews)
        if (std::count (places.upper.begin(), places.upper.end(), view) + std::count (places.lower.begin(), places.lower.end(), view) != 1)
            return {};

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

ViewArea::ViewArea (Panel which, const Views& shown)
    : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop),
      area (which),
      views (shown)
{
    setTitle (area == Panel::song ? "Upper panel" : "Lower panel");
    setTabBarDepth (tabBarDepth);
    setOutline (0);

    for (auto& holder : holders)
        holder.setTitle ("View");

    routingButton.setClickingTogglesState (true);
    routingButton.setTooltip ("Shows every channel's wires in place of the mixer's strips: where its sound goes, its sends and the "
                              "sidechains that listen to it. Drag from a channel's Out to route it");
    routingButton.onClick = [this]
    {
        if (onRoutingChanged != nullptr)
            onRoutingChanged (routingButton.getToggleState());
    };
    addChildComponent (routingButton);
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

    resized(); // the Routing switch shows with the mixer
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
            const auto text = about + ". Drag the tab to " + getOtherAreaName (area).toLowerCase() + " to move it there";

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
    const auto other = area == Panel::song ? Panel::editor : Panel::song;
    const juce::Component::SafePointer<ViewArea> safeThis (this);
    juce::PopupMenu menu;
    menu.addItem ("Move to " + getOtherAreaName (area),
                  [safeThis, view, other]
                  {
                      if (safeThis != nullptr && safeThis->onMove != nullptr)
                          safeThis->onMove (view, other, -1);
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
    leaveCorner (*this, area == Panel::song ? 2 : 3); // maximize and hide; the lower panel's own window too

    for (auto& holder : holders)
        holder.resized();

    // While the mixer shows here, its Routing switch at the end of the tab bar, before the corner.
    routingButton.setVisible (getShownTab() == ViewTab::mixer);

    if (routingButton.isVisible())
    {
        auto& bar = getTabbedButtonBar();
        auto barArea = bar.getBounds();
        routingButton.setBounds (barArea.removeFromRight (routingButtonWidth).reduced (0, Space::xs));
        bar.setBounds (barArea.withTrimmedRight (Space::xs));
    }
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

void ViewArea::paintOverChildren (juce::Graphics& g)
{
    if (! dropIndex.has_value())
        return;

    // The whole area tinted, and a line in the tab bar where the tab would go.
    const auto accent = getPalette (*this).accent;
    g.setColour (accent.withAlpha (ViewDropTint::wash));
    g.fillRect (getLocalBounds());
    g.setColour (accent);
    const auto bar = getTabbedButtonBar().getBounds().toFloat();
    g.fillRect (
        juce::Rectangle<float> (getDropLineX (*dropIndex) - ViewDropTint::line / 2.0f, bar.getY(), ViewDropTint::line, bar.getHeight()));
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
    if (const auto index = getDropIndexAt (details.localPosition); index != dropIndex)
    {
        dropIndex = index;
        repaint();
    }
}

void ViewArea::itemDragExit (const SourceDetails&)
{
    dropIndex.reset();
    repaint();
}

void ViewArea::itemDropped (const SourceDetails& details)
{
    const auto index = dropIndex.value_or (getDropIndexAt (details.localPosition));
    dropIndex.reset();
    repaint();

    if (const auto view = readViewDrag (details.description); view.has_value() && onMove != nullptr)
        onMove (*view, area, index);
}
} // namespace phazite::ui
