// SPDX-License-Identifier: AGPL-3.0-or-later
// Song outlines (docs/design.md, section 13): templates that lay out a song's sections with their usual
// lengths, an energy level and a tip each, so newcomers see how songs are usually shaped. They are data
// files (resources/outlines/*.outline.json), so a new style needs no code: add its file and list it in
// CMakeLists.txt.
#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include <optional>
#include <vector>

namespace phazite
{
namespace te = tracktion;

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

// The id of the outline applied last, or an empty string.
juce::String getAppliedOutline (const te::Edit&);
} // namespace phazite
