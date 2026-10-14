// SPDX-License-Identifier: AGPL-3.0-or-later
// Golden files: what Phazite makes (saved projects, and the levels of renders), compared with files committed in
// tests/golden, so that a change to what Phazite saves or plays, or to how it opens projects saved by earlier versions,
// fails a test that lists each difference. A refactoring must leave them as they are. A change meant to alter them
// regenerates them: run the golden tests (--test-suite=golden) with the environment variable PHAZITE_UPDATE_GOLDEN=1,
// never in CI, and read git diff tests/golden in full (CLAUDE.md).
//
// Saved projects are compared after normaliseProject(), which leaves out only what changes from run to run or from one
// machine to another:
//   N1  the root's appVersion, creationTime and projectID become "(varies)", and lastSignificantChange goes;
//       modifiedBy stays (it must be empty: Phazite never saves the user's name)
//   N2  INPUTDEVICES loses its children: the inputs of the machine that saved the project
//   N3  linkID values (shared by a clip and its aliases) become link-1, link-2... in document order
//   N4  backslashes in 'source' paths become '/', and the project folder's absolute path becomes $PROJECT
//   N5  decimal numbers are written with 7 significant digits (std::chars_format::general), -0 as 0; never the
//       values of id, colour, name, source and linkID
//   N6  each element's children are sorted, stably, by tag (tracks of every kind count as TRACK, clips of every kind
//       as CLIP): only siblings with different keys change places, so the order of tracks, clips, plug-ins, notes
//       and points within them is always compared
//   N7  attributes are sorted by name
//   N8  an empty element (no attributes, no children) goes only if its tag is in getDroppableEmptyTags(): any other
//       empty element appearing or vanishing is a difference
#pragma once

#include "TestHelpers.h"
#include "UiTestHelpers.h"

#include <functional>
#include <memory>

namespace phazite::test
{
// <repository>/tests/golden: found at run time from the test executable, which every preset builds under
// build/<preset>/ in the source tree, so that no path is compiled in.
juce::File getGoldenFolder();

// True if the environment variable PHAZITE_UPDATE_GOLDEN is 1: golden files that don't match are rewritten instead
// of failing. Fails the test if the environment variable CI is set too: golden files are never updated in CI.
bool isUpdatingGoldenFiles();

// <temp>/PhaziteGoldenActual: where the golden tests write what Phazite made, to compare with a golden file that
// doesn't match it. Other users may own it (on Linux, every user shares the temporary folder), so a copy that can't be
// written there is reported instead of failing the test.
juce::File getActualFolder();

// Delivers the messages and runs the timers that are due for some milliseconds, as the app's message loop would, so
// that what Tracktion and Phazite do asynchronously is done.
void settle (int milliseconds = 250);

// An XML file, which must be a document.
std::unique_ptr<juce::XmlElement> readXml (const juce::File&);

//==============================================================================
// The tags of empty elements that Tracktion itself adds or drops between a save and the next one of the reopened
// project. Never a tag of Phazite's (PHAZITE, PIANOROLL, LASTKEY): a read that adds an empty one of those must show.
const juce::StringArray& getDroppableEmptyTags();

// A saved project without what varies between runs and machines (N1 to N8 above). projectFolder is the folder of the
// project the save belongs to, whose path becomes $PROJECT.
std::unique_ptr<juce::XmlElement> normaliseProject (const juce::XmlElement& saved, const juce::File& projectFolder,
                                                    const juce::StringArray& droppableEmptyTags = getDroppableEmptyTags());

// A saved project file, read and normalised (its folder is the project's).
std::unique_ptr<juce::XmlElement> readNormalisedProject (const juce::File& projectFile);

//==============================================================================
struct XmlComparison
{
    bool actualMayAdd = false; // attributes and children only the actual one has aren't differences
    int maxDifferences = 40;   // the most lines findDifferences() returns
};

// The differences between two normalised projects, one per line, at most maxDifferences, each with its path:
//   EDIT/FOLDERTRACK#1015/TRACK#1004@phaziteHeight: expected "96", got "64"
//   EDIT/PHAZITE/PIANOROLL: missing
// An element in a path is TAG#id if it has an id, otherwise TAG[n] among its siblings of that tag without one, or
// just TAG if it is the only one. Children are matched by those keys; two values are the same if their texts are, or
// if both are decimal numbers (N5) that differ by at most a millionth of the larger.
juce::StringArray findDifferences (const juce::XmlElement& expected, const juce::XmlElement& actual, XmlComparison = {});

// Shortens a list of more than maxDifferences differences (at least one) to that many lines: the first ones, and a line
// saying that more weren't listed, which a failure report counts as "more than ... differences".
void limitDifferences (juce::StringArray& differences, int maxDifferences);

// A normalised project as a golden file: an XML header, a comment saying what it is and how to regenerate it, and each
// attribute on a line of its own, with "\n" line ends and one at the end.
juce::String toGoldenText (const juce::XmlElement& normalised, const juce::String& about);

//==============================================================================
// Checks (one CHECK) that two normalised saves are the same project, listing the differences if not. "what" says how
// the actual one came about: "reopened and saved".
void checkSameProject (const juce::String& what, const juce::XmlElement& expected, const juce::XmlElement& actual, XmlComparison = {});

// Checks (one CHECK) that a normalised project matches the golden file tests/golden/<relativePath>, listing the
// differences and where Phazite's output went if not. Phazite's output is always written to
// <temp>/PhaziteGoldenActual/<relativePath>, or the report says it couldn't be. While updating, a golden file that is
// missing or differs is rewritten.
void checkProjectGolden (const juce::String& relativePath, const juce::XmlElement& normalised, const juce::String& about);

// The same for a golden text file: compareTexts lists the differences between the golden text and Phazite's (both with
// "\n" line ends).
void checkGoldenText (const juce::String& relativePath, const juce::String& actualText,
                      const std::function<juce::StringArray (const juce::String& expected, const juce::String& actual)>& compareTexts);

// Keeps a project as an earlier version saved it (tests/golden/legacy), to check that later versions still open it as
// the same song. Written only while updating and only if the file is missing (it is never regenerated), and only from
// a save by that version: 'source' paths get '/' separators, so that it opens on every OS, and INPUTDEVICES loses the
// machine's inputs.
void freezeLegacyInput (const juce::String& relativePath, const juce::XmlElement& rawSave, const juce::String& requiredAppVersion);

//==============================================================================
// One of Tracktion's devices that the device list offers (phazite::getDeviceTypes()), such as
// te::ReverbPlugin::xmlTypeName, made as the browser makes it.
te::Plugin::Ptr createBuiltInDevice (te::Edit&, const char* xmlTypeName);
} // namespace phazite::test
