// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectContext.h"
#include "model/arrangement/internal/SongSelection.h"
#include "model/base/Schema.h"
#include "model/engine/EngineSetup.h"

namespace phazite
{
//==============================================================================
// Listens to the open Edit's state and tells the context's listeners which groups changed, once per turn of the message
// loop. What Tracktion writes as it plays (the transport, the input devices) is no change to the song.
struct ProjectContext::SongWatcher : private juce::ValueTree::Listener,
                                     private juce::AsyncUpdater
{
    explicit SongWatcher (ProjectContext& c)
        : context (c)
    {
    }

    ~SongWatcher() override { setState ({}); }

    void setState (juce::ValueTree newState)
    {
        state.removeListener (this);
        cancelPendingUpdate();
        pending = {};
        state = std::move (newState);
        state.addListener (this);
    }

private:
    ProjectContext& context;
    juce::ValueTree state;
    SongChanges pending;

    // The group a change to this element, or to something in it, belongs to.
    void mark (const juce::ValueTree& changed)
    {
        for (auto element = changed; element.isValid(); element = element.getParent())
        {
            if (element.hasType (te::IDs::TRANSPORT) || element.hasType (te::IDs::INPUTDEVICES))
                return;

            if (element.hasType (te::IDs::ARRANGERCLIP))
                return add (pending.sections);

            if (te::Clip::isClipState (element))
                return add (pending.clips);

            if (element.hasType (te::IDs::PLUGIN) || element.hasType (te::IDs::MASTERPLUGINS))
                return add (pending.devices);

            if (element.hasType (te::IDs::TEMPOSEQUENCE))
                return add (pending.tempo);

            if (te::TrackList::isTrack (element))
                return add (pending.tracks);
        }
    }

    void add (bool& group)
    {
        group = true;
        triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override
    {
        const auto changes = std::exchange (pending, {});
        context.listeners.call ([&changes] (ProjectContext::Listener& l) { l.songChanged (changes); });
    }

    // A child that is gone has no parent to say where it was: its own type does, or else its old parent's.
    void markRemoved (const juce::ValueTree& parent, const juce::ValueTree& child)
    {
        if (child.hasType (te::IDs::ARRANGERCLIP))
            add (pending.sections);
        else if (te::Clip::isClipState (child))
            add (pending.clips);
        else if (child.hasType (te::IDs::PLUGIN))
            add (pending.devices);
        else if (te::TrackList::isTrack (child))
            add (pending.tracks);
        else
            mark (parent);
    }

    // A track or a device that moved past another, which changes their order. Tracktion sorts a track's elements now and
    // then (after a clip is added, say), which changes no order that matters: clips, sections and the tempo's changes
    // have their places in time.
    void markMoved (const juce::ValueTree& parent, int oldIndex, int newIndex)
    {
        const auto moved = parent.getChild (newIndex);
        const auto isTrack = te::TrackList::isTrack (moved), isDevice = moved.hasType (te::IDs::PLUGIN);

        // The elements it passed: now between its new place and its old one.
        const auto first = newIndex < oldIndex ? newIndex + 1 : oldIndex;
        const auto last = newIndex < oldIndex ? oldIndex : newIndex - 1;

        for (int i = first; i <= last && (isTrack || isDevice); ++i)
        {
            const auto passed = parent.getChild (i);

            if (isTrack && te::TrackList::isTrack (passed))
                return add (pending.tracks);

            if (isDevice && passed.hasType (te::IDs::PLUGIN))
                return add (pending.devices);
        }
    }

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
    {
        // How tall a track shows, and the like, is view state (Schema.h), not the song.
        if (! (te::TrackList::isTrack (tree) && schema::isTrackViewProperty (property)))
            mark (tree);
    }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child) override { mark (child); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override { markRemoved (parent, child); }
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int oldIndex, int newIndex) override
    {
        markMoved (parent, oldIndex, newIndex);
    }
};

//==============================================================================
ProjectContext::ProjectContext (te::Engine& engine, UIBehaviour* tracktionFocus)
    : focus (tracktionFocus),
      selection (engine),
      songSelection (createSongSelection (selection)),
      songWatcher (std::make_unique<SongWatcher> (*this))
{
    if (focus != nullptr)
        focus->setSelectionManager (&selection);
}

ProjectContext::~ProjectContext()
{
    songWatcher = nullptr;

    if (focus != nullptr)
    {
        focus->setFocusedEdit (nullptr);
        focus->setSelectionManager (nullptr);
    }
}

void ProjectContext::setEdit (te::Edit* edit)
{
    if (focus != nullptr)
        focus->setFocusedEdit (nullptr);

    selection.deselectAll();
    selection.edit = nullptr;
    rangeSelection.clear();
    selection.edit = edit;
    songWatcher->setState (edit != nullptr ? edit->state : juce::ValueTree());

    if (focus != nullptr)
        focus->setFocusedEdit (edit);

    listeners.call ([] (Listener& l) { l.projectChanged(); });
}

void ProjectContext::addListener (Listener* listener)
{
    listeners.add (listener);
}

void ProjectContext::removeListener (Listener* listener)
{
    listeners.remove (listener);
}
} // namespace phazite
