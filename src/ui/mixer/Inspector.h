// SPDX-License-Identifier: AGPL-3.0-or-later
// The inspector, the panel on the right (docs/design.md, section 4; roadmap step 2.5): the properties of what is
// selected, as Bitwig's, Studio Pro's, Logic Pro's and Cubase's inspectors show them, a name and a value on each row so
// that it uses its width. Clips (or the clip of the notes selected), a section, or a track (also the track of a selected
// device or automation point); with nothing selected, the song. It isn't a copy of a mixer channel: the mixer shows those.
#pragma once

#include "model/project/ProjectContext.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace phazite::ui
{
class CommandBridge;

class Inspector : public juce::Component,
                  private ProjectContext::Listener,
                  private juce::ChangeListener,
                  private juce::Timer
{
public:
    Inspector (CommandBridge&, ProjectContext&);
    ~Inspector() override;

    // What it shows: "Track", "Clip", "Clips", "Section" or "Song"; nothing without a project. For tests.
    juce::String getShown() const;

    void paint (juce::Graphics&) override;
    void resized() override;

    // What is shown for one kind of thing: a title, then rows of a name and a value.
    struct Page;

    // What the selection points to, to show it again only when that changes.
    struct Target
    {
        enum class Kind
        {
            none,
            song,
            track,
            clips,
            section
        };

        Kind kind = Kind::none;
        TrackID track {};
        std::vector<ClipID> clips {};
        SectionID section {};

        bool operator== (const Target&) const = default;
    };

private:
    CommandBridge& commands;
    ProjectContext& context;
    juce::Viewport view; // scrolls the page up and down when the panel is shorter than it
    std::unique_ptr<Page> page;
    Target shown;

    Target getTarget() const;
    void showTarget(); // a new page if the selection points to something else
    void layOutPage();

    void projectChanged() override;
    void songChanged (const SongChanges&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection changed
    void timerCallback() override;                                   // the faders' and switches' values
};
} // namespace phazite::ui
