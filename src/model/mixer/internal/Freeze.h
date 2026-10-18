// SPDX-License-Identifier: AGPL-3.0-or-later
// Freezing's Tracktion side (model/mixer/FreezeModel.h), for the model, the app and tests. Phazite freezes a track with
// Tracktion's own parts (its freeze point plug-in, the frozen switch of the devices before it, and the track's switch,
// which makes it play its frozen file), but sets them without undo steps, and renders the file itself, in the background:
// Tracktion's own freeze renders on the message thread, records its changes as undo steps, and keeps the file in a
// temporary folder that it empties when it is full. Phazite keeps it in the project's Cache folder (the Edit's
// temporary folder, model/project/internal/Project.h), so that a frozen track opens frozen.
#pragma once

#include "model/base/internal/Tracktion.h"

#include <map>

namespace phazite
{
// Where a track's frozen part ends, as an index in its plug-in list: its fader, or an earlier device that must hear its
// sound live (a device that listens to a sidechain, a send before the fader, or a container other tracks play too).
int getFreezePointIndex (const te::AudioTrack&);

// Freezes a track with a file rendered from it up to its freeze point, which becomes its frozen file in the Edit's
// temporary folder: the freeze point goes in, the devices before it stop playing, and the track plays the file. Not an
// undo step.
void freezeFromFile (te::AudioTrack&, const juce::File& rendered);

// Plays a frozen track live again. Not an undo step. Its frozen file stays until the cache is purged.
void unfreeze (te::AudioTrack&);

// Whether a plug-in's sound is in its track's frozen file.
bool isFrozenDevice (const te::Plugin&);

// The audio tracks whose frozen file plays.
juce::Array<te::AudioTrack*> getFrozenTracks (const te::Edit&);

//==============================================================================
// Opening a project's frozen tracks. Tracktion looks for a frozen track's file in its own temporary folder while it makes
// the Edit, before the Edit can be told about the project's Cache folder, and unfreezes the track if it isn't there. So
// takeFrozenTracks() turns off the frozen switch of the tracks whose file is in the Cache folder before the Edit is made
// (and unfreezes those whose file isn't: they open unfrozen), and restoreFrozenTracks() turns it on again once the Edit's
// temporary folder is the Cache folder. Neither is a change.
juce::Array<te::EditItemID> takeFrozenTracks (juce::ValueTree& state, const juce::File& cacheFolder);
void restoreFrozenTracks (te::Edit&, const juce::Array<te::EditItemID>&);

// Deletes from an Edit's temporary folder what nothing in the Edit uses: renders of clips that have changed since, and
// the frozen files of tracks that aren't frozen, or are gone. When a project closes (when it opens, its clips haven't yet
// asked for the renders they use).
void purgeCache (te::Edit&);

//==============================================================================
// Keeps a project's frozen tracks honest while the project context holds their Edit: a frozen track whose clips, notes or
// frozen devices change (by undo or redo too) is unfrozen, so that what plays is always what is shown. It compares what
// the freeze holds with a copy taken when the track was frozen (or came back frozen, by undo), or the project opened, so
// that notifications that change nothing (Tracktion sends some while it plays) leave it frozen.
class FreezeKeeper : private juce::ValueTree::Listener
{
public:
    explicit FreezeKeeper (te::Edit&);
    ~FreezeKeeper() override;

private:
    te::SafeSelectable<te::Edit> edit;
    juce::ValueTree state;                            // the Edit's, which outlives it
    std::map<te::EditItemID, juce::ValueTree> frozen; // what each frozen track's file holds
    bool checking = false;

    void check (juce::ValueTree& changed);

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override { check (tree); }
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&) override { check (parent); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int) override { check (parent); }
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override { check (parent); }

    JUCE_DECLARE_NON_COPYABLE (FreezeKeeper)
};
} // namespace phazite
