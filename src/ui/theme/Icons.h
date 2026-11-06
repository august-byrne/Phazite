// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's icons: line drawings from Lucide (ISC licence; a few, from Feather, MIT; see
// resources/icons/lucide/LICENSE), compiled into the app. They take any color, so they follow the theme.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
enum class Icon
{
    play,
    stop,
    record,
    capture,
    keyboard,
    search,
    detach,
    plus,
    magnet,
    loop,
    metronome,
    expanded,    // a chevron pointing down: something open that can fold
    folded,      // pointing right: folded
    collapseAll, // two chevrons pointing together: every row made a thin one
    expandAll,   // pointing apart: every row at its full height
    // The playlist's tools
    smartTool,
    draw,
    paint,
    slice,
    slip,
    mute,
    erase,
    select,
    zoom,
    // The browser
    folder,
    openFolder, // a folder in the playlist whose tracks show
    addFolder,
    returnTrack, // a return track, which takes back what the sends to it feed it: the Return key's arrow
    favorite,
    recent,
    sound,
    midi,
    instrument,
    effect,
    preview,
    download,
    close,      // closes something
    frozen,     // a frozen track: it plays a file of its sound
    lowLatency, // the Low Latency switch: a lightning bolt
    // The lower panel's corner (ui/shell/PanelLayout.h), with detach and close
    maximizePanel,
    restorePanels,
    // The activity bar (ui/shell/ActivityBar.h): the left panel's views, then the account and the settings at its foot
    library,  // the browser
    project,  // what the project holds: a clipboard's list
    versions, // the project's versions: a branch
    account,
    settings,
    reload, // a crashed plug-in loaded again (ui/project/ProjectView.h): an arrow going round
    // The Source Control view (ui/versions/VersionsView.h)
    check,       // Commit, and the branch the song is on
    moreActions, // a view's menu of what else it does: three dots
    song,        // a change to the song's own: its tempo, sections, chords
    minus,       // a line unstaged
    // The title bar (ui/shell/TitleBar.h)
    menu, // the menus, folded
    undo,
    redo,
    layout, // the Layout menu
    minimiseWindow,
    maximiseWindow,
    restoreWindow
};

// The icon, drawn in black (IconButton recolors it), or nullptr if it couldn't be read. A filled icon has its shape
// filled as well as outlined, such as a favorite's star.
std::unique_ptr<juce::Drawable> createIcon (Icon, bool filled = false);

// Phazite's logo (resources/logo/phazite.svg), its own drawing in its own colours, which the theme doesn't tint: the title
// bar's, and the window's icon where the system takes it from the window (Linux). Nullptr if it couldn't be read.
std::unique_ptr<juce::Drawable> createLogo();

// The logo drawn on a transparent square so many pixels across, or an invalid image if it couldn't be read.
juce::Image createLogoImage (int size);

// An icon drawn in any color, as a list's rows draw theirs: recolored only when the color changes.
class TintedIcon
{
public:
    explicit TintedIcon (Icon);
    void draw (juce::Graphics&, juce::Rectangle<float> area, juce::Colour);

private:
    std::unique_ptr<juce::Drawable> drawable;
    juce::Colour colour { juce::Colours::black }; // the color the drawing is in now
};

//==============================================================================
// A button that shows an icon, with its text beside it if it has any, in the theme's colors.
class IconButton : public juce::Button
{
public:
    // The title is what screen readers say; it matters most for buttons that show only an icon.
    IconButton (const juce::String& title, Icon);

    void setIcon (Icon);

    // A quiet button shows only its icon, without a background until the mouse is over it: for small buttons
    // inside other controls, such as the chevron that collapses a track.
    void setQuiet (bool);

    // An accented button is a view's main one, in the accent color, as Commit is.
    void setAccented (bool);

    void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;

private:
    Icon icon;
    bool quiet = false, accented = false;
    std::unique_ptr<juce::Drawable> drawable;
    juce::Colour drawableColour { juce::Colours::black }; // the color the drawing is in now
};
} // namespace phazite::ui
