// SPDX-License-Identifier: AGPL-3.0-or-later
// A file written whole or not at all (src/model/base/WholeFile.h): what the project file and the versions' files are
// written with.
#include "TestHelpers.h"
#include "model/base/WholeFile.h"

namespace
{
using phazite::test::TemporaryFolder;

juce::StringArray getFileNames (const juce::File& folder)
{
    juce::StringArray names;

    for (const auto& file : folder.findChildFiles (juce::File::findFilesAndDirectories, false)) // hidden ones too
        names.add (file.getFileName());

    names.sort (false);
    return names;
}
} // namespace

TEST_CASE ("A file is written whole: a new one made and an old one replaced, with nothing left beside it")
{
    TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Song.phazite");

    const juce::String first ("the first");
    REQUIRE (phazite::writeWholeFile (file, first.toRawUTF8(), first.getNumBytesAsUTF8()).wasOk());
    CHECK (file.loadFileAsString() == first);

    // Through a stream, as text is written; and longer than the stream's buffer, which goes to the file in pieces.
    juce::String second;

    for (int i = 0; i < 20000; ++i)
        second << "line " << i << "\n";

    REQUIRE (
        phazite::writeWholeFile (file, [&second] (juce::OutputStream& out) { return out.writeText (second, false, false, "\n"); }).wasOk());
    CHECK (file.loadFileAsString() == second);

    // Nothing at all is a file too.
    REQUIRE (phazite::writeWholeFile (file, nullptr, 0).wasOk());
    CHECK (file.existsAsFile());
    CHECK (file.getSize() == 0);

    CHECK (getFileNames (temp.folder) == juce::StringArray { "Song.phazite" });
}

// What writes it says it failed, here. That the stream's own failure (a full disk) and a file shorter on the disk than what
// was written fail it the same way isn't tested: no test fills a disk.
TEST_CASE ("A write that fails midway leaves the file as it was, says so, and leaves nothing beside it")
{
    TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Song.phazite");
    const juce::String saved ("what was saved before");
    REQUIRE (file.replaceWithText (saved));

    // As a disk that fills up: some of it written, then a write that doesn't work.
    const auto result = phazite::writeWholeFile (file,
                                                 [] (juce::OutputStream& out)
                                                 {
                                                     out << "half of what";
                                                     return false;
                                                 });

    CHECK (result.failed());
    CHECK (result.getErrorMessage().isNotEmpty());
    CHECK (file.loadFileAsString() == saved);
    CHECK (getFileNames (temp.folder) == juce::StringArray { "Song.phazite" });

    // And none is made where there was none.
    const auto other = temp.folder.getChildFile ("Other.phazite");
    CHECK (phazite::writeWholeFile (other, [] (juce::OutputStream&) { return false; }).failed());
    CHECK_FALSE (other.exists());
    CHECK (getFileNames (temp.folder) == juce::StringArray { "Song.phazite" });
}

TEST_CASE ("A file that can't be written beside, or can't be replaced, fails and leaves what is there")
{
    TemporaryFolder temp;
    const juce::String text ("a song");

    // A folder that isn't there, and a file where the folder goes.
    const auto missing = temp.folder.getChildFile ("Gone").getChildFile ("Song.phazite");
    CHECK (phazite::writeWholeFile (missing, text.toRawUTF8(), text.getNumBytesAsUTF8()).failed());
    CHECK_FALSE (missing.getParentDirectory().exists());

    const auto notAFolder = temp.folder.getChildFile ("File");
    REQUIRE (notAFolder.replaceWithText ("in the way"));
    CHECK (phazite::writeWholeFile (notAFolder.getChildFile ("Song.phazite"), text.toRawUTF8(), text.getNumBytesAsUTF8()).failed());
    CHECK (notAFolder.loadFileAsString() == "in the way");

    // A folder of the file's name, which a file can't take the place of.
    const auto folder = temp.folder.getChildFile ("Song.phazite");
    REQUIRE (folder.createDirectory().wasOk());
    REQUIRE (folder.getChildFile ("Kept").replaceWithText ("kept"));
    CHECK (phazite::writeWholeFile (folder, text.toRawUTF8(), text.getNumBytesAsUTF8()).failed());
    CHECK (folder.isDirectory());
    CHECK (folder.getChildFile ("Kept").existsAsFile());
    CHECK (getFileNames (temp.folder) == juce::StringArray { "File", "Song.phazite" });
}
