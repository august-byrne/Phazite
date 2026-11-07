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
    constexpr std::array<Panel, 4> allPanels { Panel::left, Panel::song, Panel::editor, Panel::inspector };

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
      projectView (bridge, context),
      versionsView (bridge, context, s),
      playlist (bridge, context),
      inspector (bridge, context),
      pianoRoll (bridge, context),
      audioClipEditor (bridge, context),
      mixer (bridge, context),
      devices (bridge, context),
      statusBar (engine, bridge, context),
      activityBar (bridge, context),
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
        keepOverlaysInFront();
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

    // The views move between the areas by their tabs (Panels.h), and two areas side by side keep their shares of their row.
    for (auto* area : getAreas())
    {
        area->onMove = [this] (View view, ViewPlace to, int index) { moveView (view, to, index); };
        area->onPlaceBeside = [this] (View view, Panel row, bool left) { placeViewBeside (view, row, left); };
        area->onReset = [this] { commands.invoke ("view.resetViewPlaces"); };
    }

    for (auto* row : { &upperRow, &lowerRow })
    {
        row->onShareChanged = [this, row]
        {
            panels.rowShares[row == &upperRow ? 0 : 1] = row->share;
            settings.set (Settings::panelLayout, panels.toText());
        };
        row->onLayout = [this] { updateCards(); };
    }

    clipEditor.onEditorChanged = [this]
    {
        for (auto* area : getAreas())
            area->refreshTabs();
    };

    // Enter or a double-click in the browser, and the plug-in picker.
    browser.onPlace = [this] (const LibraryItem& item) { return placeItem (item); };
    browser.onShowSoundPack = [this] { commands.invoke ("help.phaziteSounds"); };
    devices.onShowPresets = [this] (DeviceID device, const juce::String& name, juce::Component& target)
    { browser.showDevicePresetMenu (device, name, target); };

    // Swap: a drum pad's Swap, or a device's in its Presets menu, tries the browser's sounds or presets in place; the browser
    // shows, with the keys.
    devices.onSwap = [this] (DeviceID device, int pad) { browser.startSwap (device, pad); };
    browser.onSwapStarted = [this] { revealBrowser(); };

    for (auto* c : std::initializer_list<juce::Component*> { &titleBar, &topBar, &activityBar, &leftPanel, &leftDivider, &centre,
                                                             &inspectorDivider, &inspector, &inspectorButtons, &statusBar })
        addAndMakeVisible (c);

    for (auto* c : std::initializer_list<juce::Component*> { &upperRow, &editorDivider, &lowerRow, &editorButtons })
        centre.addAndMakeVisible (c);

    centre.onResize = [this] { layOutCentre(); };

    // Dragged, the dividers size the panels beside them, or close them; double-clicked, they put them back at their sizes.
    for (const auto& [divider, panel] : { std::pair { &leftDivider, Panel::left }, std::pair { &inspectorDivider, Panel::inspector },
                                          std::pair { &editorDivider, Panel::editor } })
    {
        divider->onDragStart = [this, which = panel] { startDividerDrag (which); };
        divider->onDrag = [this, which = panel] (int distance) { dragDivider (which, distance); };
        divider->onDragEnd = [this] { endDividerDrag(); };
        divider->onDoubleClick = [this, which = panel] { resetPanelSize (which); };
    }

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
    setThemeColour (openingLabel, juce::Label::backgroundColourId, &Palette::surface);
    setThemeColour (openingLabel, juce::Label::textColourId, &Palette::textMuted);
    addChildComponent (openingLabel);
    openingLabel.setVisible (! projectContext.hasProject());

    // Over the panels, their cards' corners and edges; over everything while a lesson runs: the ring around a step's
    // control, which clicks pass through, and the card.
    addAndMakeVisible (cards);
    addChildComponent (lessonCard.getRing());
    addChildComponent (lessonCard);

    // How the editors follow playback, as it was left; the panels as they were left, or every panel, and the views where
    // they were left.
    playlist.setScrollingContinuously (settings.get (Settings::scrollContinuously));
    playlist.setHeaderWidth (juce::roundToInt (settings.get (Settings::playlistHeaderWidth)));
    playlist.onHeaderWidthChanged = [this] (int width) { settings.set (Settings::playlistHeaderWidth, (double) width); };
    pianoRoll.setScrollingContinuously (settings.get (Settings::scrollContinuously));
    panels.setFromText (settings.get (Settings::panelLayout));
    upperRow.share = panels.rowShares[0];
    lowerRow.share = panels.rowShares[1];
    viewPlaces = ViewPlaces::fromString (settings.get (Settings::viewPlaces));
    placeViews();
    updateLayout();
}

AppShell::~AppShell() // here, where PanelWindow is complete
{
    beingDeleted = true;
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
        panels.hide (Panel::left);
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
    return upperRow.holds (tab) ? upperRow.getAreaOf (tab) : lowerRow.getAreaOf (tab);
}

std::array<ViewArea*, 4> AppShell::getAreas()
{
    return { &upperRow.getArea (0), &upperRow.getArea (1), &lowerRow.getArea (0), &lowerRow.getArea (1) };
}

bool AppShell::takesRoom (Panel panel) const
{
    // A row without views takes none, as if it were hidden.
    if ((panel == Panel::song && upperRow.isEmpty()) || (panel == Panel::editor && lowerRow.isEmpty()))
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
    focusView (tab);
}

void AppShell::showPlaylist()
{
    revealView (ViewTab::playlist);
    focusView (ViewTab::playlist);
}

void AppShell::focusView (ViewTab tab)
{
    // The keys go where the command took the eyes, and a screen reader says the view's name; Tab goes on into its controls.
    // A time line's view is also the editor the time line's commands act on.
    juce::Component* view = nullptr;
    TimelineEditor* editor = nullptr;

    switch (tab)
    {
        case ViewTab::playlist:
            view = editor = &playlist;
            break;
        case ViewTab::mixer:
            view = &mixer;
            break;
        case ViewTab::clipEditor:
            view = editor = &clipEditor.getEditor();
            break;
        case ViewTab::instrument:
        case ViewTab::effects:
            view = &devices;
            break;
    }

    if (view == nullptr || ! view->isShowing())
        return;

    view->grabKeyboardFocus();

    if (editor != nullptr)
        setActiveEditor (editor);
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
    upperRow.setViews (viewPlaces.upper);
    lowerRow.setViews (viewPlaces.lower);

    // Where a view dragged over an area can go besides its tabs: beside it while its row has room, and into a row without
    // views (so that a row emptied can be filled again).
    for (auto* area : getAreas())
    {
        const auto row = area->getArea();
        area->setDropSides (viewPlaces.hasRoomBeside (row), viewPlaces.isEmpty (row == Panel::song ? Panel::editor : Panel::song));
    }
}

void AppShell::moveView (View view, ViewPlace place, int index)
{
    viewPlaces.move (view, place, index);
    settings.set (Settings::viewPlaces, viewPlaces.toString());
    placeViews();
    revealMoved (view);
}

void AppShell::placeViewBeside (View view, Panel row, bool left)
{
    viewPlaces.placeBeside (view, row, left);
    settings.set (Settings::viewPlaces, viewPlaces.toString());
    placeViews();
    revealMoved (view);
}

void AppShell::bringViewsBack (Panel row)
{
    // Its own views, as they start (the upper row's the playlist and the mixer, the lower one's the clip editor and the
    // devices), into its area, from wherever they went.
    if (! viewPlaces.isEmpty (row))
        return;

    const ViewPlaces start;

    for (const auto view : start.getViews ({ row, 0 }))
        viewPlaces.move (view, { row, 0 });

    settings.set (Settings::viewPlaces, viewPlaces.toString());
    placeViews();
}

void AppShell::revealMoved (View view)
{
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

void AppShell::showLeftView (LeftView view)
{
    // As VS Code's activity bar: the view that shows hides the panel; another one, or any while the panel is hidden, shows
    // there. In its own window, the panel just shows it.
    if (leftWindow != nullptr)
    {
        panels.leftView = view;
        leftWindow->toFront (true);
    }
    else if (panels.takesRoom (Panel::left) && panels.leftView == view)
    {
        panels.hide (Panel::left);
    }
    else
    {
        panels.leftView = view;
        panels.show (Panel::left);
    }

    updateLayout();

    // Showing a view gives it the keys, as the views in the centre get them: the view itself, never a text field in it, so that
    // the shortcuts work on (Space plays) until a field is clicked, or Search the Browser puts the cursor in the browser's
    // search (the owner, 2026-11-07).
    if (leftPanel.isShowing())
    {
        auto& shown = view == LeftView::versions  ? static_cast<juce::Component&> (versionsView)
                      : view == LeftView::project ? static_cast<juce::Component&> (projectView)
                                                  : static_cast<juce::Component&> (browser);
        shown.grabKeyboardFocus();
    }
}

void AppShell::startSwap (DeviceID device, int pad)
{
    browser.startSwap (device, pad);
}

void AppShell::revealBrowser()
{
    panels.leftView = LeftView::browser;

    if (leftWindow != nullptr)
        leftWindow->toFront (true);
    else if (! panels.takesRoom (Panel::left))
        panels.show (Panel::left);

    updateLayout();
}

AppShell::LeftPanel::LeftPanel (juce::Component& browserToShow, juce::Component& projectToShow, juce::Component& versionsToShow)
    : browserView (browserToShow),
      projectView (projectToShow),
      versionsView (versionsToShow)
{
    setTitle ("Left panel");
    addChildComponent (browserView);
    addChildComponent (projectView);
    addChildComponent (versionsView);
}

void AppShell::LeftPanel::show (LeftView view)
{
    browserView.setVisible (view == LeftView::browser);
    projectView.setVisible (view == LeftView::project);
    versionsView.setVisible (view == LeftView::versions);
}

void AppShell::LeftPanel::resized()
{
    for (auto* view : { &browserView, &projectView, &versionsView })
        view->setBounds (getLocalBounds());
}

void AppShell::showCommandPalette()
{
    CommandPalette::show (commands, titleBar.getSearchBox());
}

void AppShell::showBranchPicker (BranchPicker::Mode mode)
{
    BranchPicker::show (commands, projectContext, titleBar.getSearchBox(), mode);
}

void AppShell::toggleBounceEachVersion()
{
    const auto bounces = ! settings.get (Settings::versionBounce);
    settings.set (Settings::versionBounce, bounces);
    setBounceWithVersion (projectContext, bounces);
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
                           : which == Panel::left       ? panels.leftWidth
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
        editorButtons.toFront (false);
        inspectorButtons.toFront (false);
        keepOverlaysInFront();
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
        case Panel::left:
            return leftPanel;
        case Panel::song:
            return upperRow;
        case Panel::inspector:
            return inspector;
        case Panel::editor:
        default:
            return lowerRow;
    }
}

std::unique_ptr<AppShell::PanelWindow>& AppShell::getWindow (Panel which)
{
    jassert (which != Panel::song);

    switch (which)
    {
        case Panel::left:
            return leftWindow;
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
    // A row left without views shows with its own views brought back to it, rather than its switch doing nothing.
    if ((panel == Panel::song || panel == Panel::editor) && viewPlaces.isEmpty (panel))
    {
        bringViewsBack (panel);
        panels.show (panel);
    }
    else
    {
        panels.toggle (panel);
    }

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

    if (const auto shown = area.getShownTab())
        focusView (*shown);
}

void AppShell::setScrollingContinuously (bool on)
{
    settings.set (Settings::scrollContinuously, on);
    playlist.setScrollingContinuously (on);
    pianoRoll.setScrollingContinuously (on); // while it follows playback (View > Follow Playback)
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
    const auto leftDocked = panels.takesRoom (Panel::left), songDocked = takesRoom (Panel::song);
    const auto editorDocked = takesRoom (Panel::editor), inspectorDocked = panels.takesRoom (Panel::inspector);
    const auto centreDocked = songDocked || editorDocked;

    if (leftWindow == nullptr)
        leftPanel.setVisible (leftDocked);

    // The left panel's view, and its icon marked while it shows, here or in its own window.
    leftPanel.show (panels.leftView);
    activityBar.showLeftPanel (leftDocked || leftWindow != nullptr, panels.leftView);

    if (inspectorWindow == nullptr)
        inspector.setVisible (inspectorDocked);

    if (editorWindow == nullptr)
        lowerRow.setVisible (editorDocked);

    upperRow.setVisible (songDocked);
    centre.setVisible (centreDocked);

    // A divider whose drag closed its panel stays until the drag ends, to open it again.
    leftDivider.setVisible ((leftDocked || dividerDragged == Panel::left) && centreDocked);
    inspectorDivider.setVisible ((inspectorDocked || dividerDragged == Panel::inspector) && centreDocked);
    editorDivider.setVisible ((songDocked && editorDocked) || (centreDocked && dividerDragged == Panel::editor));
    editorButtons.setVisible (editorDocked);
    editorButtons.update (panels);
    inspectorButtons.setVisible (inspectorDocked);
    inspectorButtons.update (panels);
    titleBar.showPanels (panels);

    // (A sound the browser previews stops as the browser goes out of sight: Browser::visibilityChanged().)
    resized();
    layOutCentre();
    settings.set (Settings::panelLayout, panels.toText());
}

void AppShell::startDividerDrag (Panel panel)
{
    dividerDragged = panel;
    panelsAtDragStart = panels;
    sizeAtDragStart = panel == Panel::left ? leftPanel.getWidth() : panel == Panel::inspector ? inspector.getWidth() : lowerRow.getHeight();
}

void AppShell::dragDivider (Panel panel, int distance)
{
    // Where the drag began, moved by the distance, as far as the window has room: the centre keeps its least, and the
    // panels keep what is shown. As VS Code's sashes: dragged until the panel would be under half its least, the panel
    // closes, keeping the size it had for when it opens again (Shell::dividerSnap); dragged back, it opens again.
    const auto snapsShut = [] (int wanted, int least) { return wanted < juce::roundToInt (least * Shell::dividerSnap); };

    // Closes a panel the drag took past its least (it is shown at the size it had), or opens it again: whether it changed.
    const auto close = [this] (Panel which, bool shut)
    {
        if (shut == ! takesRoom (which))
            return false;

        if (shut)
            panels.hide (which);
        else
            panels.show (which);

        return true;
    };

    if (panel == Panel::left || panel == Panel::inspector)
    {
        const auto isLeft = panel == Panel::left;
        const auto least = isLeft ? Shell::minimumLeftPanelWidth : Shell::minimumInspectorWidth;
        const auto wanted = sizeAtDragStart + (isLeft ? distance : -distance);
        auto& width = isLeft ? panels.leftWidth : panels.inspectorWidth;
        const auto shut = snapsShut (wanted, least);

        if (shut)
        {
            width = isLeft ? panelsAtDragStart.leftWidth : panelsAtDragStart.inspectorWidth;
        }
        else
        {
            // As far as the window has room beside the other side panel and the centre at its least.
            const auto other = isLeft ? Panel::inspector : Panel::left;
            const auto otherWidth = panels.takesRoom (other) ? getPanel (other).getWidth() + Shell::dividerSize : 0;
            const auto room = getWidth() - otherWidth - Shell::dividerSize - Shell::minimumCentreWidth;
            const auto most = juce::jlimit (least, isLeft ? Shell::maximumLeftPanelWidth : Shell::maximumInspectorWidth, room);
            width = juce::jlimit (least, most, wanted);
        }

        if (close (panel, shut))
        {
            updateLayout();
            return;
        }
    }
    else if (centre.getHeight() > 0)
    {
        // The lower panel's height, each panel keeping its least; past either least, that panel closes.
        const auto height = centre.getHeight();
        const auto wanted = sizeAtDragStart - distance;
        const auto lowest = juce::jmax (Shell::minimumCentreHeight, juce::roundToInt (std::ceil (height * Shell::minimumEditorShare)));
        const auto highest = juce::jmax (lowest, juce::jmin (height - Shell::dividerSize - Shell::minimumCentreHeight,
                                                             juce::roundToInt (std::floor (height * Shell::maximumEditorShare))));
        const auto lowerShut = snapsShut (wanted, lowest);
        const auto upperShut = snapsShut (height - Shell::dividerSize - wanted, height - Shell::dividerSize - highest);

        panels.editorShare = lowerShut || upperShut ? panelsAtDragStart.editorShare
                                                    : juce::jlimit (Shell::minimumEditorShare, Shell::maximumEditorShare,
                                                                    juce::jlimit (lowest, highest, wanted) / (double) height);

        // Shown again before the other closes, so that the centre always shows one.
        auto changed = close (lowerShut ? Panel::song : Panel::editor, false);
        changed = (lowerShut ? close (Panel::editor, true) : close (Panel::song, upperShut)) || changed;

        if (changed)
        {
            updateLayout();
            return;
        }
    }

    resized();
    layOutCentre();
    settings.set (Settings::panelLayout, panels.toText());
}

void AppShell::endDividerDrag()
{
    // The divider of a panel the drag closed goes with it.
    dividerDragged.reset();
    updateLayout();
}

void AppShell::resetPanelSize (Panel panel)
{
    if (panel == Panel::left)
        panels.leftWidth = Shell::leftPanelWidth;
    else if (panel == Panel::inspector)
        panels.inspectorWidth = Shell::inspectorWidth;
    else
        panels.editorShare = Shell::editorShare;

    updateLayout();
}

void AppShell::paint (juce::Graphics& g)
{
    // The window's frame, as VS Code's Modern UI's shell: behind the bars, and between the panels' cards (PanelCards).
    g.fillAll (getPalette (*this).surface);
}

void AppShell::updateCards()
{
    // The activity bar and the left panel beside it as one card, as VS Code joins its activity bar to its side bar; the
    // inspector; and each area of views that shows here.
    juce::Array<juce::Rectangle<int>> shown;
    auto rail = activityBar.getBounds();

    if (leftPanel.isVisible() && leftPanel.getParentComponent() == this)
        rail = rail.getUnion (leftPanel.getBounds());

    shown.add (rail);

    if (inspector.isVisible() && inspector.getParentComponent() == this)
        shown.add (inspector.getBounds());

    if (centre.isVisible())
        for (auto* area : getAreas())
            if (area->isVisible() && area->getParentComponent() != nullptr && area->getParentComponent()->isVisible() && isParentOf (area))
                shown.add (getLocalArea (area, area->getLocalBounds()));

    cards.setBounds (getLocalBounds());
    cards.setCards (shown);
}

void AppShell::keepOverlaysInFront()
{
    cards.toFront (false);
    lessonCard.getRing().toFront (false);
    lessonCard.toFront (false);
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

void AppShell::showProject (const juce::String& name, bool unsavedChanges, bool neverSaved, std::optional<juce::Time> lastAutosave)
{
    titleBar.setProjectName (name, unsavedChanges);
    statusBar.showSaveState (name.isEmpty()   ? StatusBar::SaveState::none
                             : neverSaved     ? StatusBar::SaveState::neverSaved
                             : unsavedChanges ? StatusBar::SaveState::unsaved
                                              : StatusBar::SaveState::saved,
                             lastAutosave);
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

    // The panels' cards, so far apart and from the window's sides, or the band along a restored window's sides that resizes
    // it, if that is wider.
    const auto sideMargin = juce::jmax (edgeInset, Shell::cardGap);
    area = area.withTrimmedTop (Shell::cardGap).withTrimmedBottom (Shell::cardGap).reduced (sideMargin, 0);

    // The activity bar down the left edge, whatever shows beside it; the left panel right beside it, in the same card.
    activityBar.setBounds (area.removeFromLeft (Shell::activityBarWidth));
    openingLabel.setBounds (area);

    // The side panels at the widths they keep, as far as the window has room, and the centre between them. A panel in its
    // own window is that window's to lay out.
    const auto sides = panels.getSideWidths (area.getWidth());
    const auto centreDocked = takesRoom (Panel::song) || takesRoom (Panel::editor);

    if (panels.takesRoom (Panel::left))
    {
        leftPanel.setBounds (area.removeFromLeft (sides.left));

        if (centreDocked)
            leftDivider.setBounds (area.removeFromLeft (Shell::dividerSize));
    }
    else if (centreDocked)
    {
        // The activity bar's card apart from the centre's: the divider there while its drag has closed the panel.
        const auto gap = area.removeFromLeft (Shell::cardGap);

        if (dividerDragged == Panel::left)
            leftDivider.setBounds (gap.withWidth (Shell::dividerSize));
    }

    if (panels.takesRoom (Panel::inspector))
    {
        inspector.setBounds (area.removeFromRight (sides.inspector));

        if (centreDocked)
            inspectorDivider.setBounds (area.removeFromRight (Shell::dividerSize));
    }
    else if (centreDocked && dividerDragged == Panel::inspector)
    {
        inspectorDivider.setBounds (area.removeFromRight (Shell::dividerSize));
    }

    if (centreDocked)
        centre.setBounds (area);

    placePanelButtons();
    updateCards();

    // The lesson card in the bottom right corner, over the panels and above the status bar, as tall as its text; its ring
    // over the whole window.
    lessonCard.getRing().setBounds (getLocalBounds());
    const auto cardWidth = juce::jmin (LessonTint::width + 2 * LessonTint::shadow, getWidth() - 2 * edgeInset);
    const auto cardHeight = lessonCard.getHeightFor (cardWidth);
    lessonCard.setBounds (getWidth() - edgeInset - cardWidth, statusBar.getY() - cardHeight, cardWidth, cardHeight);
}

std::unique_ptr<juce::ComponentTraverser> AppShell::createKeyboardFocusTraverser()
{
    struct Traverser : public juce::KeyboardFocusTraverser
    {
        explicit Traverser (AppShell& s)
            : shell (s)
        {
        }

        juce::Component* getDefaultComponent (juce::Component* parent) override
        {
            // (Not while the shell's parts are being deleted: a part deleted with the keys asks for this too.)
            if (parent == &shell && ! shell.beingDeleted)
                for (auto* view : std::initializer_list<juce::Component*> { shell.getTimelineEditorInUse(), &shell.playlist, &shell.mixer,
                                                                            &shell.clipEditor.getEditor(), &shell.devices })
                    if (view != nullptr && view->isShowing() && view->getWantsKeyboardFocus())
                        return view;

            return juce::KeyboardFocusTraverser::getDefaultComponent (parent);
        }

        AppShell& shell;
    };

    return std::make_unique<Traverser> (*this);
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

    // The divider between them stays, at the edge, while its drag has closed either.
    const auto keepsDivider = dividerDragged == Panel::editor;

    if (songDocked && editorDocked)
    {
        lowerRow.setBounds (area.removeFromBottom (panels.getEditorHeight (area.getHeight())));
        editorDivider.setBounds (area.removeFromBottom (Shell::dividerSize));
    }
    else if (editorDocked)
    {
        if (keepsDivider)
            editorDivider.setBounds (area.removeFromTop (Shell::dividerSize));

        lowerRow.setBounds (area);
    }
    else if (songDocked && keepsDivider)
    {
        editorDivider.setBounds (area.removeFromBottom (Shell::dividerSize));
    }

    if (songDocked)
        upperRow.setBounds (area);

    placePanelButtons();
    updateCards();
}

void AppShell::placePanelButtons()
{
    // In the lower panel's top right corner while it is docked, in the centre's coordinates.
    if (editorWindow == nullptr)
    {
        const auto bounds = lowerRow.getBounds();
        const auto width = editorButtons.getPreferredWidth();
        editorButtons.setBounds (bounds.getRight() - width - Space::xs, bounds.getY() + Space::xs / 2, width, Shell::panelButtonSize);
    }

    // At the right end of the inspector's title while it is docked, as far from its edge, the title short of it.
    if (inspectorWindow == nullptr)
    {
        const auto row = inspector.getTitleRow() + inspector.getPosition();
        const auto width = inspectorButtons.getPreferredWidth();
        inspectorButtons.setBounds (row.getRight() - width - Space::xs, row.getCentreY() - Shell::panelButtonSize / 2, width,
                                    Shell::panelButtonSize);
    }

    inspector.setTitleRoom (inspectorWindow == nullptr ? inspectorButtons.getPreferredWidth() + 2 * Space::xs : 0);
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
             revealBrowser();
             browser.focusSearch();
         });

    // The left panel's views, as the activity bar's icons show them (ActivityBar.h): each shows or hides it, as a view's
    // tab does in the lower panel.
    add ("view.browser", "Browser",
         "Shows the browser in the left panel: sounds, instruments, effects and presets; or hides the panel if it shows it.",
         { "library", "sounds", "samples", "plugins", "presets", "sidebar" }, [this] { showLeftView (LeftView::browser); });

    add ("view.project", "Project",
         "Shows what the project holds in the left panel: its notes, the audio files the song plays and its plug-ins, with any "
         "that are missing or crashed; or hides the panel if it shows them.",
         { "notes", "files", "audio files", "missing", "plug-ins used", "pool", "contents", "sidebar" },
         [this] { showLeftView (LeftView::project); });

    add ("view.versions", "Source Control",
         "Shows the project's source control in the left panel: its changes, staged or not, the commit and the graph of its "
         "branches; or hides the panel if it shows them.",
         { "versions", "history", "branches", "git", "commits", "snapshots", "sidebar" }, [this] { showLeftView (LeftView::versions); });

    // The branch picker, as the Versions view's branch button and the status bar's branch open it, and Bounce Each Version,
    // as Save Version's menu shows it.
    registry.add (
        { .id = "version.pickBranch",
          .name = "Checkout to...",
          .category = "Project",
          .description = "Lists the project's branches to check out, or creates a new one from the current commit, named as you type.",
          .synonyms = { "branch", "checkout", "git", "switch", "idea", "new branch" },
          .perform = [this] { showBranchPicker (BranchPicker::Mode::pick); },
          .isEnabled = [this] { return getCurrentBranch (projectContext).has_value(); } });

    registry.add ({ .id = "version.renameBranch",
                    .name = "Rename Branch...",
                    .category = "Project",
                    .description = "Names the branch the song is on anew.",
                    .synonyms = { "branch", "name", "rename", "title" },
                    .perform = [this] { showBranchPicker (BranchPicker::Mode::rename); },
                    .isEnabled = [this] { return getCurrentBranch (projectContext).has_value(); } });

    registry.add ({ .id = "version.bounceEach",
                    .name = "Bounce Each Commit",
                    .category = "Project",
                    .description = "Renders the song into each commit as Commit makes it, as Export Song renders it, to hear that "
                                   "commit later against the song.",
                    .synonyms = { "bounce", "render", "audio", "with a bounce", "mixdown" },
                    .perform = [this] { toggleBounceEachVersion(); },
                    .isTicked = [this] { return settings.get (Settings::versionBounce); } });

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
         { PanelCommands { Panel::left,
                           "Shows or hides the left panel, with the browser, the project or source control; the icons beside it stay.",
                           { "browser", "versions", "sidebar", "files", "samples", "plugins", "sounds" } },
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
         [this] { showPlaylist(); });

    add ("view.mixer", "Mixer", "Shows the mixer, or the playlist again if the mixer is showing in its place.", { "channels", "faders" },
         [this] { toggleMixer(); });

    add ("view.layoutBeat", "Beat Layout", "Arranges the panels for making beats: playlist, piano roll and browser.", { "layout" },
         [this] { setLayout (Layout::beat); });

    add ("view.layoutMix", "Mix Layout", "Arranges the panels for mixing: the mixer, the selected track's devices and the inspector.",
         { "layout" }, [this] { setLayout (Layout::mix); });

    add ("view.layoutFull", "Full Layout", "Shows every panel.", { "layout", "all" }, [this] { setLayout (Layout::full); });

    registry.add ({ .id = "view.scrollContinuously",
                    .name = "Scroll Continuously",
                    .category = "View",
                    .description = "While the song plays, keeps the playhead in the middle and scrolls the song under it, rather "
                                   "than turning the page at the edge.",
                    .synonyms = { "smooth scrolling", "stationary playhead", "centered playhead", "scroll in play", "follow" },
                    .perform = [this] { setScrollingContinuously (! settings.get (Settings::scrollContinuously)); },
                    .isTicked = [this] { return settings.get (Settings::scrollContinuously); } });

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

    for (const auto& command : { DetachCommand { Panel::left, "view.detachLeftPanel", "Left Panel in Its Own Window" },
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

    // The note commands use the piano roll's grid, and Legato the gap its Legato... was given last.
    addNoteCommands (
        registry, projectContext, [this] { return pianoRoll.getGridStepInBeats(); }, [this] { return pianoRoll.getLegatoGapBeats(); });
    pianoRoll.addCommands (registry);
    guide.addCommands (registry);
    lessons.addCommands (registry);
    refreshShortcutTexts();
}

void AppShell::refreshShortcutTexts()
{
    titleBar.refreshTooltips();
    topBar.refreshTooltips();
    activityBar.refreshTooltips();
    projectView.refreshTooltips();
    versionsView.refreshTooltips();
    for (auto* area : getAreas())
        area->refreshTooltips (commands);

    editorButtons.refreshTooltips();
    inspectorButtons.refreshTooltips();
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
