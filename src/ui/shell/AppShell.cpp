// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppShell.h"
#include "CommandPalette.h"
#include "model/mixer/MixerModel.h"
#include "model/presets/PresetModel.h"
#include "ui/browser/PluginPicker.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr std::array<Panel, 4> allPanels { Panel::browser, Panel::song, Panel::editor, Panel::inspector };

    // Where a panel's own window goes: on another monitor if there is one, otherwise over the main window.
    juce::Rectangle<int> choosePanelWindowBounds (const juce::Component& mainWindowContent, juce::Point<int> size)
    {
        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto mainCentre = mainWindowContent.getScreenBounds().getCentre();
        const auto bounds = juce::Rectangle<int> (size.x, size.y);

        for (const auto& display : displays.displays)
            if (! display.userBounds.contains (mainCentre.toFloat()))
                return bounds.withCentre (display.userBounds.getCentre().roundToInt());

        if (const auto* display = displays.getDisplayForPoint (mainCentre.toFloat()))
            return bounds.withCentre (mainCentre).constrainedWithin (display->userBounds.getSmallestIntegerContainer());

        return bounds.withCentre (mainCentre);
    }
} // namespace

//==============================================================================
// A panel in a window of its own. The shortcuts work there too, and closing the window puts the panel
// back into the main window.
struct AppShell::PanelWindow : public juce::DocumentWindow
{
    PanelWindow (juce::Component& panel, juce::Rectangle<int> bounds, juce::KeyListener& keys, std::function<void()> onClose)
        : juce::DocumentWindow (panel.getTitle() + " - Phazite", getPalette (panel).background, juce::DocumentWindow::allButtons),
          shortcuts (keys),
          close (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        setContentNonOwned (&panel, false);
        setResizable (true, false);
        setResizeLimits (240, 240, 16384, 16384);
        setBounds (bounds);
        addKeyListener (&shortcuts);
        setVisible (true);
    }

    ~PanelWindow() override
    {
        removeKeyListener (&shortcuts);
        clearContentComponent(); // the panel belongs to the main window
    }

    void closeButtonPressed() override { close(); }

    juce::KeyListener& shortcuts;
    std::function<void()> close;
};

//==============================================================================
AppShell::AppShell (EngineContext& engine, CommandBridge& bridge, KeyDispatcher& keys, ComputerKeyboard& keyboard, ProjectContext& context,
                    Settings& s, SoundPack& pack, Library& l, Previewer& p)
    : engineContext (engine),
      commands (bridge),
      shortcutKeys (keys),
      projectContext (context),
      settings (s),
      library (l),
      previewer (p),
      computerKeyboard (keyboard),
      titleBar (bridge),
      topBar (bridge, keyboard, context),
      browser (library, previewer, pack, s, context),
      playlist (bridge, context),
      inspector (bridge, context),
      pianoRoll (bridge, context),
      audioClipEditor (bridge, context),
      mixer (bridge, context),
      devices (bridge, context),
      statusBar (engine, bridge, context),
      guide (bridge, s),
      lessons (bridge, context, s)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Phazite");

    // The guide's steps that no command does. The first note's step is saved after the notes played with it have gone out:
    // marking it writes the settings file.
    computerKeyboard.onNotePlayed = [this]
    {
        if (! guide.isDone (Guide::Step::playNotes))
            juce::MessageManager::callAsync (
                [shell = juce::Component::SafePointer<AppShell> (this)]
                {
                    if (shell != nullptr)
                        shell->guide.markDone (Guide::Step::playNotes);
                });
    };
    browser.onUsed = [this] (const LibraryItem&) { guide.markDone (Guide::Step::addSound); };
    getHintBar().setDefaultHint ([this] { return getDefaultHint(); });

    // The lesson that runs, in its card over the window's corner, with a ring around the control its step is about.
    lessons.onChange = [this]
    {
        lessonCard.update();
        lessonCard.getRing().toFront (false);
        lessonCard.toFront (false);
    };

    // Double-clicking a clip opens it in its editor, a MIDI clip in the piano roll and an audio clip in the audio clip
    // editor, which then has the keys.
    playlist.onOpenClip = [this] (ClipID clip)
    {
        clipEditor.openClip (clip);
        revealView (ViewTab::clipEditor);
        auto& editor = clipEditor.getEditor();
        editor.grabKeyboardFocus();
        setActiveEditor (&editor);
    };

    // The mixer's Routing switch is in the tab bar of the area the mixer is in, so that the mixer has no row of its own for
    // it. And the views move between the areas by their tabs (Panels.h).
    for (auto* area : { &upperArea, &lowerArea })
    {
        area->onRoutingChanged = [this] (bool on)
        {
            mixer.showRouting (on);

            for (auto* each : { &upperArea, &lowerArea })
                each->getRoutingButton().setToggleState (on, juce::dontSendNotification);
        };

        area->onMove = [this] (View view, Panel to, int index) { moveView (view, to, index); };
        area->onReset = [this] { commands.invoke ("view.resetViewPlaces"); };
    }

    clipEditor.onEditorChanged = [this]
    {
        upperArea.refreshTabs();
        lowerArea.refreshTabs();
    };

    // Enter or a double-click in the browser, and the plug-in picker.
    browser.onPlace = [this] (const LibraryItem& item) { return placeItem (item); };
    browser.onShowSoundPack = [this] { commands.invoke ("help.phaziteSounds"); };
    devices.onShowPresets = [this] (DeviceID device, const juce::String& name, juce::Component& target)
    { browser.showDevicePresetMenu (device, name, target); };

    for (auto* c : std::initializer_list<juce::Component*> { &titleBar, &topBar, &browser, &browserDivider, &centre, &inspectorDivider,
                                                             &inspector, &statusBar, &browserButtons, &inspectorButtons })
        addAndMakeVisible (c);

    for (auto* c : std::initializer_list<juce::Component*> { &upperArea, &editorDivider, &lowerArea, &songButtons, &editorButtons })
        centre.addAndMakeVisible (c);

    centre.onResize = [this] { layOutCentre(); };

    // Dragged, the dividers size the panels beside them.
    browserDivider.onDragStart = [this] { sizeAtDragStart = browser.getWidth(); };
    browserDivider.onDrag = [this] (int distance) { dragDivider (Panel::browser, distance); };
    inspectorDivider.onDragStart = [this] { sizeAtDragStart = inspector.getWidth(); };
    inspectorDivider.onDrag = [this] (int distance) { dragDivider (Panel::inspector, distance); };
    editorDivider.onDragStart = [this] { sizeAtDragStart = lowerArea.getHeight(); };
    editorDivider.onDrag = [this] (int distance) { dragDivider (Panel::editor, distance); };

    // Clicking in an editor makes it the one the time line's commands act on; so does the keyboard focus
    // moving into it.
    playlist.onUsed = [this] { setActiveEditor (&playlist); };
    pianoRoll.onUsed = [this] { setActiveEditor (&pianoRoll); };
    audioClipEditor.onUsed = [this] { setActiveEditor (&audioClipEditor); };
    setActiveEditor (&playlist);
    juce::Desktop::getInstance().addFocusChangeListener (this);
    projectContext.addListener (this);
    engineContext.addChangeListener (this);
    updateAudioAlert();

    // Until a project is open, as Phazite starts: the window shows at once, painted, while the project loads (docs/
    // performance-audit.md, R4).
    openingLabel.setText ("Opening...", juce::dontSendNotification);
    openingLabel.setJustificationType (juce::Justification::centred);
    openingLabel.setFont (juce::FontOptions (TextSize::heading));
    openingLabel.setColour (juce::Label::backgroundColourId, getPalette (*this).surface);
    openingLabel.setColour (juce::Label::textColourId, getPalette (*this).textMuted);
    addChildComponent (openingLabel);
    openingLabel.setVisible (! projectContext.hasProject());

    // Over everything while a lesson runs: the ring around a step's control, which clicks pass through, and the card.
    addChildComponent (lessonCard.getRing());
    addChildComponent (lessonCard);

    // The panels as they were left, or every panel, and the views where they were left.
    panels.setFromText (settings.get (Settings::panelLayout));
    viewPlaces = ViewPlaces::fromString (settings.get (Settings::viewPlaces));
    placeViews();
    updateLayout();
}

AppShell::~AppShell() // here, where PanelWindow is complete
{
    engineContext.removeChangeListener (this);
    projectContext.removeListener (this);
    computerKeyboard.onNotePlayed = nullptr; // the keyboard is the main window's, and outlives this
    juce::Desktop::getInstance().removeFocusChangeListener (this);
}

void AppShell::globalFocusChanged (juce::Component* focused)
{
    // The panel in use, which "Maximize the Panel in Use" fills the window with.
    for (const auto panel : allPanels)
        if (focused != nullptr && getPanel (panel).isParentOf (focused))
            panelInUse = panel;

    // Focus that moves elsewhere (to the mixer, say) leaves the last editor active.
    for (auto* c = focused; c != nullptr; c = c->getParentComponent())
    {
        if (auto* editor = dynamic_cast<TimelineEditor*> (c))
        {
            setActiveEditor (editor);
            return;
        }
    }
}

void AppShell::setActiveEditor (TimelineEditor* editor)
{
    activeEditor = editor;

    for (auto* each : std::initializer_list<TimelineEditor*> { &playlist, &pianoRoll, &audioClipEditor })
        each->setActive (each == editor);
}

void AppShell::projectChanged()
{
    openingLabel.setVisible (! projectContext.hasProject());
    previewer.stop(); // which may be in time with the project that closes

    // A menu's choices name the items of the project that was open, and the new one's can have the same IDs.
    juce::PopupMenu::dismissAllActiveMenus();
}

void AppShell::setLayout (Layout layout)
{
    panels.restore();

    for (const auto panel : allPanels)
        panels.show (panel);

    if (layout == Layout::mix)
        panels.hide (Panel::browser);
    else if (layout == Layout::beat)
        panels.hide (Panel::inspector);

    panels.editorShare = layout == Layout::mix ? Shell::mixEditorShare : Shell::editorShare; // tall enough for most devices' editors
    // The layout's views, wherever they are: its main one last, so that it shows where both are in one area.
    const auto tabs =
        layout == Layout::mix ? std::array { ViewTab::effects, ViewTab::mixer } : std::array { ViewTab::clipEditor, ViewTab::playlist };

    for (const auto tab : tabs)
        getAreaOf (tab).showTab (tab);

    updateLayout();
}

ViewArea& AppShell::getAreaOf (ViewTab tab)
{
    return upperArea.holds (tab) ? upperArea : lowerArea;
}

bool AppShell::takesRoom (Panel panel) const
{
    // An area without views takes none, as if it were hidden.
    if ((panel == Panel::song && upperArea.isEmpty()) || (panel == Panel::editor && lowerArea.isEmpty()))
        return false;

    return panels.takesRoom (panel);
}

void AppShell::showView (ViewTab tab)
{
    auto& area = getAreaOf (tab);
    const auto lower = area.getArea() == Panel::editor;

    // In its own window, the lower panel just shows that tab.
    if (lower && editorWindow != nullptr)
    {
        area.showTab (tab);
        editorWindow->toFront (true);
        return;
    }

    // In the lower panel, like FL Studio's F7: shows that view, or hides the panel if it shows it already.
    if (lower && takesRoom (Panel::editor) && area.getShownTab() == tab)
    {
        panels.hide (Panel::editor);
    }
    else
    {
        panels.show (area.getArea());
        area.showTab (tab);
    }

    updateLayout();
}

void AppShell::revealView (ViewTab tab)
{
    auto& area = getAreaOf (tab);
    area.showTab (tab);

    if (area.getArea() == Panel::editor && editorWindow != nullptr)
    {
        editorWindow->toFront (true);
        return;
    }

    panels.show (area.getArea());
    updateLayout();
}

void AppShell::placeViews()
{
    upperArea.setViews (viewPlaces.upper);
    lowerArea.setViews (viewPlaces.lower);
}

void AppShell::moveView (View view, Panel area, int index)
{
    viewPlaces.move (view, area, index);
    settings.set (Settings::viewPlaces, viewPlaces.toString());
    placeViews();

    // It shows where it went: the devices on the page they showed.
    switch (view)
    {
        case View::playlist:
            revealView (ViewTab::playlist);
            break;
        case View::mixer:
            revealView (ViewTab::mixer);
            break;
        case View::clipEditor:
            revealView (ViewTab::clipEditor);
            break;
        case View::devices:
        default:
            revealView (devices.getPage() == DevicesView::Page::instrument ? ViewTab::instrument : ViewTab::effects);
            break;
    }
}

void AppShell::resetViewPlaces()
{
    viewPlaces = {};
    settings.set (Settings::viewPlaces, viewPlaces.toString());
    placeViews();
    updateLayout();
}

void AppShell::showSelectedDevice()
{
    if (const auto selected = projectContext.getSongSelection().getDevices(); ! selected.empty())
        devices.showDevice (selected.front());

    // The tab of the page the device is on: its track's instrument, or the effects.
    revealView (devices.getPage() == DevicesView::Page::instrument ? ViewTab::instrument : ViewTab::effects);
}

void AppShell::showCommandPalette()
{
    CommandPalette::show (commands, titleBar.getSearchBox());
}

void AppShell::showPluginPicker()
{
    PluginPicker::show (library, titleBar.getSearchBox(),
                        [safeThis = juce::Component::SafePointer<AppShell> (this)] (const LibraryItem& item)
                        {
                            if (safeThis != nullptr)
                                safeThis->browser.place (item);
                        });
}

bool AppShell::placeItem (const LibraryItem& item)
{
    if (! projectContext.hasProject())
        return false;

    // A device goes onto the selected track (or a new one: phazite::addDeviceTo()), and so does a preset (a track's makes
    // a new track after it: loadPreset()); a sound goes at the play start.
    const auto tracks = projectContext.getSongSelection().getTracks();
    const auto track = tracks.empty() ? TrackID() : tracks.front();

    if (item.device.has_value())
        return addDeviceTo (projectContext, track, *item.device);

    if (item.kind == LibraryItem::Kind::preset)
        return loadPreset (projectContext, item.file, track);

    return playlist.placeFile (item.file);
}

//==============================================================================
void AppShell::setDetached (Panel which, bool detached)
{
    if (which == Panel::song)
        return; // the song's views stay in the main window

    auto& window = getWindow (which);

    if (detached == (window != nullptr))
    {
        if (window != nullptr)
            window->toFront (true);

        return;
    }

    auto& panel = getPanel (which);

    if (detached)
    {
        // As large as it shows here, or as it would beside the others while it fills the window.
        const auto width = ! panels.isMaximized (which) ? panel.getWidth()
                           : which == Panel::browser    ? panels.browserWidth
                           : which == Panel::inspector  ? panels.inspectorWidth
                                                        : centre.getWidth();
        const auto height =
            panels.isMaximized (which) && which == Panel::editor ? panels.getEditorHeight (centre.getHeight()) : panel.getHeight();
        const juce::Point<int> size (juce::jmax (width, 360), juce::jmax (height, 400));

        window = std::make_unique<PanelWindow> (panel, choosePanelWindowBounds (*this, size), shortcutKeys,
                                                [safeThis = juce::Component::SafePointer<AppShell> (this), which]
                                                {
                                                    // Closing deletes the window, so dock the panel once the click is over.
                                                    juce::MessageManager::callAsync (
                                                        [safeThis, which]
                                                        {
                                                            if (safeThis != nullptr)
                                                                safeThis->setDetached (which, false);
                                                        });
                                                });
        panel.setVisible (true);
    }
    else
    {
        window = nullptr; // gives the panel back
        auto& parent = which == Panel::editor ? static_cast<juce::Component&> (centre) : static_cast<juce::Component&> (*this);
        parent.addChildComponent (panel);
        getButtons (which).toFront (false);
    }

    panels.setDetached (which, detached);
    updateLayout();
}

bool AppShell::isDetached (Panel which) const
{
    return panels.isDetached (which);
}

juce::Component& AppShell::getPanel (Panel which)
{
    switch (which)
    {
        case Panel::browser:
            return browser;
        case Panel::song:
            return upperArea;
        case Panel::inspector:
            return inspector;
        case Panel::editor:
        default:
            return lowerArea;
    }
}

PanelButtons& AppShell::getButtons (Panel which)
{
    switch (which)
    {
        case Panel::browser:
            return browserButtons;
        case Panel::song:
            return songButtons;
        case Panel::inspector:
            return inspectorButtons;
        case Panel::editor:
        default:
            return editorButtons;
    }
}

std::unique_ptr<AppShell::PanelWindow>& AppShell::getWindow (Panel which)
{
    jassert (which != Panel::song);

    switch (which)
    {
        case Panel::browser:
            return browserWindow;
        case Panel::inspector:
            return inspectorWindow;
        case Panel::song:
        case Panel::editor:
        default:
            return editorWindow;
    }
}

void AppShell::togglePanel (Panel panel)
{
    panels.toggle (panel);
    updateLayout();
}

void AppShell::toggleMaximized (Panel panel)
{
    panels.toggleMaximized (panel);
    updateLayout();
}

void AppShell::maximizePanelInUse()
{
    // The one the keyboard focus was in last, if it still shows here; or the song's views.
    if (panels.getMaximized().has_value())
        panels.restore();
    else
        panels.maximize (takesRoom (panelInUse) ? panelInUse : Panel::song);

    updateLayout();
}

void AppShell::toggleMixer()
{
    // F9, as in FL Studio: the mixer, or the playlist again while the mixer shows in the area that holds both; in the lower
    // panel, as F7 there, the mixer or the panel hidden.
    auto& area = getAreaOf (ViewTab::mixer);

    if (area.holds (ViewTab::playlist) && takesRoom (area.getArea()) && area.getShownTab() == ViewTab::mixer)
        revealView (ViewTab::playlist);
    else if (area.getArea() == Panel::editor)
        showView (ViewTab::mixer);
    else
        revealView (ViewTab::mixer);
}

TimelineEditor* AppShell::getTimelineEditorInUse()
{
    // The editor used last, while it shows; or one that shows, the playlist first.
    if (activeEditor != nullptr && activeEditor->isActive() && activeEditor->isShowing())
        return activeEditor;

    if (! playlist.isShowing() && clipEditor.getEditor().isShowing())
        return &clipEditor.getEditor();

    return &playlist;
}

//==============================================================================
void AppShell::updateLayout()
{
    // A panel in its own window, or behind a maximized one, takes no room here; panels in their own windows stay visible
    // there.
    const auto browserDocked = panels.takesRoom (Panel::browser), songDocked = takesRoom (Panel::song);
    const auto editorDocked = takesRoom (Panel::editor), inspectorDocked = panels.takesRoom (Panel::inspector);
    const auto centreDocked = songDocked || editorDocked;

    if (browserWindow == nullptr)
        browser.setVisible (browserDocked);

    if (inspectorWindow == nullptr)
        inspector.setVisible (inspectorDocked);

    if (editorWindow == nullptr)
        lowerArea.setVisible (editorDocked);

    upperArea.setVisible (songDocked);
    centre.setVisible (centreDocked);
    browserDivider.setVisible (browserDocked && centreDocked);
    inspectorDivider.setVisible (inspectorDocked && centreDocked);
    editorDivider.setVisible (songDocked && editorDocked);

    for (const auto panel : allPanels)
    {
        auto& buttons = getButtons (panel);
        buttons.setVisible (takesRoom (panel));
        buttons.update (panels);
    }

    titleBar.showPanels (panels);

    // (A sound the browser previews stops as the browser goes out of sight: Browser::visibilityChanged().)
    resized();
    layOutCentre();
    settings.set (Settings::panelLayout, panels.toText());
}

void AppShell::dragDivider (Panel panel, int distance)
{
    // Where the drag began, moved by the distance, as far as the window has room: the centre keeps its least, and the
    // panels keep what is shown.
    const auto roomBeside = [this] (Panel other)
    {
        const auto otherWidth = panels.takesRoom (other) ? getPanel (other).getWidth() + Shell::dividerSize : 0;
        return getWidth() - otherWidth - Shell::dividerSize - Shell::minimumCentreWidth;
    };

    if (panel == Panel::browser)
    {
        const auto most = juce::jlimit (Shell::minimumBrowserWidth, Shell::maximumBrowserWidth, roomBeside (Panel::inspector));
        panels.browserWidth = juce::jlimit (Shell::minimumBrowserWidth, most, sizeAtDragStart + distance);
    }
    else if (panel == Panel::inspector)
    {
        const auto most = juce::jlimit (Shell::minimumInspectorWidth, Shell::maximumInspectorWidth, roomBeside (Panel::browser));
        panels.inspectorWidth = juce::jlimit (Shell::minimumInspectorWidth, most, sizeAtDragStart - distance);
    }
    else if (centre.getHeight() > 0)
    {
        const auto height = centre.getHeight();
        const auto most = juce::jmax (Shell::minimumCentreHeight, height - Shell::dividerSize - Shell::minimumCentreHeight);
        panels.editorShare = juce::jlimit (Shell::minimumEditorShare, Shell::maximumEditorShare,
                                           juce::jlimit (Shell::minimumCentreHeight, most, sizeAtDragStart - distance) / (double) height);
    }

    resized();
    layOutCentre();
    settings.set (Settings::panelLayout, panels.toText());
}

void AppShell::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.border); // shows through the dividers

    // The window's sides beside the panels (setEdgeInset()), as the title bar's background.
    g.setColour (p.background);
    g.fillRect (getLocalBounds().removeFromLeft (edgeInset));
    g.fillRect (getLocalBounds().removeFromRight (edgeInset));
}

void AppShell::updateAudioAlert()
{
    const auto output = engineContext.getAudioOutput();
    statusBar.showAudioOutput (output);

    if (output.isOpen)
        getHintBar().setAlert ({}, nullptr);
    else
        getHintBar().setAlert ("No audio output: nothing can be heard. Click here to choose one in the audio settings.",
                               [this] { commands.invoke ("options.audioSettings"); });
}

void AppShell::showProject (const juce::String& name, bool unsavedChanges, bool neverSaved)
{
    titleBar.setProjectName (name, unsavedChanges);
    statusBar.showSaveState (name.isEmpty()   ? StatusBar::SaveState::none
                             : neverSaved     ? StatusBar::SaveState::neverSaved
                             : unsavedChanges ? StatusBar::SaveState::unsaved
                                              : StatusBar::SaveState::saved);
}

void AppShell::setEdgeInset (int inset)
{
    if (inset == edgeInset)
        return;

    edgeInset = inset;
    resized();
    layOutCentre();
}

juce::Component::WindowControlKind AppShell::getWindowControlAt (juce::Point<int> point) const
{
    return titleBar.getControlAt (point - titleBar.getPosition());
}

void AppShell::resized()
{
    auto area = getLocalBounds();
    titleBar.setBounds (area.removeFromTop (Shell::titleBarHeight));
    topBar.setBounds (area.removeFromTop (Shell::transportBarHeight));
    statusBar.setBounds (area.removeFromBottom (Shell::statusBarHeight));
    area = area.reduced (edgeInset, 0);
    openingLabel.setBounds (area);

    // The side panels at the widths they keep, as far as the window has room, and the centre between them. A panel in its
    // own window is that window's to lay out.
    const auto sides = panels.getSideWidths (area.getWidth());
    const auto centreDocked = takesRoom (Panel::song) || takesRoom (Panel::editor);

    if (panels.takesRoom (Panel::browser))
    {
        browser.setBounds (area.removeFromLeft (sides.browser));

        if (centreDocked)
            browserDivider.setBounds (area.removeFromLeft (Shell::dividerSize));
    }

    if (panels.takesRoom (Panel::inspector))
    {
        inspector.setBounds (area.removeFromRight (sides.inspector));

        if (centreDocked)
            inspectorDivider.setBounds (area.removeFromRight (Shell::dividerSize));
    }

    if (centreDocked)
        centre.setBounds (area);

    placePanelButtons();

    // The lesson card in the bottom right corner, over the panels and above the status bar, as tall as its text; its ring
    // over the whole window.
    lessonCard.getRing().setBounds (getLocalBounds());
    const auto cardWidth = juce::jmin (LessonTint::width + 2 * LessonTint::shadow, getWidth() - 2 * edgeInset);
    const auto cardHeight = lessonCard.getHeightFor (cardWidth);
    lessonCard.setBounds (getWidth() - edgeInset - cardWidth, statusBar.getY() - cardHeight, cardWidth, cardHeight);
}

juce::Component* AppShell::findLessonTarget (const juce::String& target)
{
    // A component with that ID that shows, depth first; each part of a path inside the one before it.
    const std::function<juce::Component*(juce::Component&, const juce::String&)> findShowing =
        [&findShowing] (juce::Component& within, const juce::String& id) -> juce::Component*
    {
        for (auto* child : within.getChildren())
        {
            if (child->getComponentID() == id && child->isShowing())
                return child;

            if (auto* found = findShowing (*child, id))
                return found;
        }

        return nullptr;
    };

    juce::Component* found = this;

    for (const auto& id : juce::StringArray::fromTokens (target, "/", {}))
        if (found = findShowing (*found, id); found == nullptr)
            return nullptr;

    return found;
}

void AppShell::layOutCentre()
{
    // The lower area under the upper one, at its share of the height, or either alone.
    auto area = centre.getLocalBounds();
    const auto songDocked = takesRoom (Panel::song), editorDocked = takesRoom (Panel::editor);

    if (songDocked && editorDocked)
    {
        lowerArea.setBounds (area.removeFromBottom (panels.getEditorHeight (area.getHeight())));
        editorDivider.setBounds (area.removeFromBottom (Shell::dividerSize));
    }
    else if (editorDocked)
    {
        lowerArea.setBounds (area);
    }

    if (songDocked)
        upperArea.setBounds (area);

    placePanelButtons();
}

void AppShell::placePanelButtons()
{
    // In each docked panel's top right corner, in its parent's coordinates (the centre's, for the song's views and the
    // editor dock).
    for (const auto panel : allPanels)
    {
        if (panel != Panel::song && getWindow (panel) != nullptr)
            continue;

        auto& buttons = getButtons (panel);
        const auto bounds = getPanel (panel).getBounds();
        const auto width = buttons.getPreferredWidth();
        buttons.setBounds (bounds.getRight() - width - Space::xs / 2, bounds.getY() + Space::xs / 2, width, Shell::panelButtonSize);
    }
}

//==============================================================================
void AppShell::addCommands (CommandRegistry& registry)
{
    const auto add = [&registry] (const char* id, const char* name, const char* description, juce::StringArray synonyms,
                                  std::function<void()> perform, std::function<bool()> isEnabled = {})
    {
        registry.add ({ .id = id,
                        .name = name,
                        .category = "View",
                        .description = description,
                        .synonyms = std::move (synonyms),
                        .perform = std::move (perform),
                        .isEnabled = std::move (isEnabled) });
    };

    add ("view.commandPalette", "Search Commands", "Finds and runs any command by name.", { "palette", "find", "run" },
         [this] { showCommandPalette(); });

    add (
        "view.menus", "Open the Menus", "Opens the File menu in the title bar, for the arrow keys to move through the menus.",
        { "menu bar", "file menu", "alt" }, [this] { titleBar.showMenus(); }, [this] { return titleBar.hasMenus(); });

    add ("view.searchBrowser", "Search the Browser", "Shows the browser and puts the cursor in its search field.",
         { "find", "sounds", "samples", "plugins" },
         [this]
         {
             if (browserWindow != nullptr)
             {
                 browserWindow->toFront (true);
             }
             else if (! panels.takesRoom (Panel::browser))
             {
                 panels.show (Panel::browser);
                 updateLayout();
             }

             browser.focusSearch();
         });

    registry.add ({ .id = "device.add",
                    .name = "Add Instrument or Effect",
                    .category = "Device",
                    .description = "Finds an instrument or effect by name and adds it to the selected track, as FL Studio's plug-in "
                                   "picker does.",
                    .synonyms = { "plug-in picker", "plugin", "vst", "synth", "insert", "fx" },
                    .perform = [this] { showPluginPicker(); },
                    .isEnabled = [this] { return projectContext.hasProject(); } });

    browser.addCommands (registry); // saving presets, which the browser shows

    // Each panel shows and hides, and fills the window until the others are restored (PanelLayout.h). Showing and hiding
    // apply to the main window; a panel in its own window is closed there.
    struct PanelCommands
    {
        Panel panel;
        const char* description; // of showing or hiding it
        juce::StringArray synonyms;
    };

    for (const auto& command :
         { PanelCommands { Panel::browser, "Shows or hides the browser on the left.", { "files", "samples", "plugins", "sounds" } },
           PanelCommands { Panel::song,
                           "Shows or hides the upper panel, by default the playlist and the mixer; the lower panel takes its place.",
                           { "song", "arrangement", "centre", "tracks", "top" } },
           PanelCommands { Panel::editor,
                           "Shows or hides the lower panel, by default the clip editor and the selected track's devices.",
                           { "dock", "editor", "piano roll", "devices", "bottom" } },
           PanelCommands { Panel::inspector, "Shows or hides the inspector on the right.", { "properties", "info" } } })
    {
        const auto which = command.panel;
        registry.add ({ .id = getPanelCommandID ("toggle", which),
                        .name = "Show or Hide the " + getPanelName (which),
                        .category = "View",
                        .description = command.description,
                        .synonyms = command.synonyms,
                        .perform = [this, which] { togglePanel (which); },
                        .isEnabled = [this, which] { return panels.canToggle (which); },
                        .currentName = [this, which] { return panels.getToggleName (which); } });

        registry.add ({ .id = getPanelCommandID ("maximize", which),
                        .name = "Maximize the " + getPanelName (which),
                        .category = "View",
                        .description = "Fills the window with " + describePanel (which)
                                       + ", the other panels hidden until you restore them, or brings them back.",
                        .synonyms = { "full", "zoom panel", "only", "restore", "focus" },
                        .perform = [this, which] { toggleMaximized (which); },
                        .isEnabled = [this, which] { return panels.canMaximize (which); },
                        .currentName = [this, which] { return panels.getMaximizeName (which); } });
    }

    registry.add ({ .id = "view.maximizePanel",
                    .name = "Maximize the Panel in Use",
                    .category = "View",
                    .description = "Fills the window with the panel you are using (the one with the keyboard focus), the others hidden "
                                   "until you restore them, or brings them back.",
                    .synonyms = { "full", "zoom panel", "only", "restore", "focus" },
                    .perform = [this] { maximizePanelInUse(); },
                    .currentName = [this] { return panels.getMaximizeInUseName(); } });

    // The clip editor's tab, which edits the selected clip: named for the editor it shows. In the lower panel, these show a
    // view or hide the panel if it shows it, as FL Studio's F7 does; in the upper panel, they show it.
    registry.add ({ .id = "view.pianoRoll",
                    .name = "Piano Roll",
                    .category = "View",
                    .description = "Shows the selected clip's editor, or hides it if it is showing: the piano roll for MIDI clips, and "
                                   "the audio clip editor for audio clips.",
                    .synonyms = { "notes", "midi", "audio clip", "audio editor", "sample editor", "clip editor" },
                    .perform = [this] { showView (ViewTab::clipEditor); },
                    .currentName = [this] { return clipEditor.showsAudioClip() ? juce::String ("Audio Clip Editor") : juce::String(); } });

    add ("view.instrument", "Instrument", "Shows the selected track's instrument, or hides it if it is showing.",
         { "synth", "sampler", "device view", "channel rack" }, [this] { showView (ViewTab::instrument); });

    add ("view.effects", "Effects", "Shows the selected track's effects, or hides them if they are showing.",
         { "devices", "device view", "chain", "plug-ins", "fx" }, [this] { showView (ViewTab::effects); });

    registry.add ({ .id = "device.show",
                    .name = "Show Device",
                    .category = "Device",
                    .description = "Shows the selected device: an instrument in the Instrument tab, an effect in the Effects tab.",
                    .synonyms = { "edit", "open", "effect", "instrument", "plug-in" },
                    .perform = [this] { showSelectedDevice(); },
                    .isEnabled = [this] { return ! projectContext.getSongSelection().getDevices().empty(); } });

    add ("view.playlist", "Playlist", "Shows the playlist, in place of the view beside it.", { "arrange", "tracks", "clips", "timeline" },
         [this] { revealView (ViewTab::playlist); });

    add ("view.mixer", "Mixer", "Shows the mixer, or the playlist again if the mixer is showing in its place.", { "channels", "faders" },
         [this] { toggleMixer(); });

    add ("view.layoutBeat", "Beat Layout", "Arranges the panels for making beats: playlist, piano roll and browser.", { "layout" },
         [this] { setLayout (Layout::beat); });

    add ("view.layoutMix", "Mix Layout", "Arranges the panels for mixing: the mixer, the selected track's devices and the inspector.",
         { "layout" }, [this] { setLayout (Layout::mix); });

    add ("view.layoutFull", "Full Layout", "Shows every panel.", { "layout", "all" }, [this] { setLayout (Layout::full); });

    // A view's tab drags to the other panel, or moves there from its menu (Panels.h); this puts them back.
    registry.add ({ .id = "view.resetViewPlaces",
                    .name = "Reset View Places",
                    .category = "View",
                    .description = "Puts the playlist and the mixer back in the upper panel, and the clip editor and the devices in the "
                                   "lower one, as they start.",
                    .synonyms = { "layout", "views", "tabs", "panels", "move back", "restore" },
                    .perform = [this] { resetViewPlaces(); },
                    .isEnabled = [this] { return ! (viewPlaces == ViewPlaces()); } });

    struct DetachCommand
    {
        Panel panel;
        const char* id;
        const char* name;
    };

    for (const auto& command : { DetachCommand { Panel::browser, "view.detachBrowser", "Browser in Its Own Window" },
                                 DetachCommand { Panel::inspector, "view.detachInspector", "Inspector in Its Own Window" },
                                 DetachCommand { Panel::editor, "view.detachEditor", "Lower Panel in Its Own Window" } })
    {
        const auto which = command.panel;
        registry.add ({ .id = command.id,
                        .name = command.name,
                        .category = "View",
                        .description = "Moves the panel to a window of its own, for example on a second monitor, or back again.",
                        .synonyms = { "detach", "undock", "float", "second monitor", "window" },
                        .perform = [this, which] { setDetached (which, ! isDetached (which)); },
                        .isTicked = [this, which] { return isDetached (which); } });
    }

    addTimelineCommands (registry, [this] { return getTimelineEditorInUse(); });
    playlist.addCommands (registry);

    // The note commands use the piano roll's grid.
    addNoteCommands (registry, projectContext, [this] { return pianoRoll.getGridStepInBeats(); });
    pianoRoll.addCommands (registry);
    guide.addCommands (registry);
    lessons.addCommands (registry);
    refreshShortcutTexts();
}

void AppShell::refreshShortcutTexts()
{
    titleBar.refreshTooltips();
    topBar.refreshTooltips();
    upperArea.refreshTooltips (commands);
    lowerArea.refreshTooltips (commands);

    for (const auto panel : allPanels)
        getButtons (panel).refreshTooltips();

    playlist.refreshShortcutTexts();
    pianoRoll.refreshShortcutTexts();
    audioClipEditor.refreshShortcutTexts();
}

juce::String AppShell::getDefaultHint() const
{
    if (auto next = guide.getHint(); next.isNotEmpty() && ! lessons.isRunning()) // the lesson card says what to do then
        return next;

    const auto paletteKey = getKeyText (commands, "view.commandPalette");
    return paletteKey.isEmpty() ? juce::String ("Search for any command with \"Search commands\" at the top.")
                                : "Press " + paletteKey + " to search for any command.";
}
} // namespace phazite::ui
