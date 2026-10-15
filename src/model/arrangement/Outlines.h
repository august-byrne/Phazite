// SPDX-License-Identifier: AGPL-3.0-or-later
// Song outlines (docs/design.md, section 13): templates that lay out a song's sections with their usual
// lengths, an energy level and a tip each, so newcomers see how songs are usually shaped. They are data
// files (resources/outlines/*.outline.json), so a new style needs no code: add its file and list it in
// CMakeLists.txt.
#pragma once

#include "model/base/Tracktion.h"

#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;

struct Outline
{
    struct Section
    {
        juce::String name {}, role {}, tip {};
        int bars = 8;
        float energy = 0.5f;
        std::optional<juce::Colour> colour {}; // from the file (a palette color's name), or else the role's
    };

    juce::String id, name, description;
    int order = 100; // where it comes in lists
    std::vector<Section> sections;

    int getLengthInBars() const;

    // Reads an outline file. Returns nothing if it has no id, name or sections, or a section without a
    // name or with no bars.
    static std::optional<Outline> fromJSON (const juce::String&);
};

// The outlines that ship with Phazite, in their order: pop, rock, electronic, and hip-hop and trap.
const std::vector<Outline>& getBuiltInOutlines();
const Outline* findOutline (const juce::String& id);

// Replaces the song's sections with the outline's, from the start of the song, in bars of the song's time
// signature (so they last as long as the tempo makes them). What is on the tracks stays where it is.
void applyOutline (te::Edit&, const Outline&);

// Applies the built-in outline with that id (findOutline()). Returns false, and changes nothing, if there is none.
bool applyOutline (te::Edit&, const juce::String& outlineID);

// The id of the outline applied last, or an empty string.
juce::String getAppliedOutline (const te::Edit&);

// Whether the outline with that id was applied last and the song still has sections.
bool isOutlineApplied (const te::Edit&, const juce::String& outlineID);

// The id of the command that applies an outline: "section.outline.<outline's id>".
juce::String getOutlineCommandID (const juce::String& outlineID);

// The outline commands: one for each built-in outline, so the palette finds them by name, each one undo step and
// ticked while its outline is applied (isOutlineApplied()). The app registers them right after the section commands.
void addOutlineCommands (CommandRegistry&, te::SelectionManager&);
} // namespace phazite
