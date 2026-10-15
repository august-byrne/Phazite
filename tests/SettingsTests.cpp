// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's settings service and the recent projects kept in it (src/services).
#include "TestHelpers.h"
#include "services/RecentProjects.h"
#include "services/Settings.h"
#include "ui/input/Keymap.h"

#include <juce_gui_extra/juce_gui_extra.h> // RecentlyOpenedFilesList, whose format the recent projects keep

using phazite::Settings;

namespace
{
// A settings file as the Engine keeps Settings.xml: XML, saved by its timer some milliseconds after a change.
std::unique_ptr<juce::PropertiesFile> makeSettingsFile (const juce::File& file, int millisecondsBeforeSaving = 2000)
{
    juce::PropertiesFile::Options options;
    options.millisecondsBeforeSaving = millisecondsBeforeSaving;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    return std::make_unique<juce::PropertiesFile> (file, options);
}

// What a settings file holds on disk for a key, read by a PropertiesFile of its own.
juce::String readFromDisk (const juce::File& file, const char* key)
{
    return makeSettingsFile (file)->getValue (key);
}

// An empty file, and the folders it is in.
juce::File createFile (const juce::File& file)
{
    REQUIRE (file.create().wasOk());
    return file;
}
} // namespace

TEST_CASE ("Every setting keeps its key")
{
    // The keys are Settings.xml's format: a renamed one would lose what the user set.
    CHECK (juce::String (Settings::theme.key) == "phaziteTheme");
    CHECK (juce::String (Settings::hoverHelp.key) == "phaziteHoverHelp");
    CHECK (juce::String (Settings::computerKeyboard.key) == "phaziteComputerKeyboard");
    CHECK (juce::String (Settings::interfaceScale.key) == "phaziteInterfaceScale");
    CHECK (juce::String (Settings::welcomeShown.key) == "phaziteWelcomeShown");
    CHECK (juce::String (Settings::keymap.key) == "phaziteKeymap");
    CHECK (juce::String (Settings::keyChangesPrefix) == "phaziteKeyChanges.");
    CHECK (juce::String (Settings::recentProjects.key) == "phaziteRecentProjects");
    CHECK (juce::String (Settings::browserFolders.key) == "phaziteBrowserFolders");
    CHECK (juce::String (Settings::browserFavorites.key) == "phaziteBrowserFavorites");
    CHECK (juce::String (Settings::browserRecent.key) == "phaziteBrowserRecent");
    CHECK (juce::String (Settings::browserPreview.key) == "phaziteBrowserPreview");
    CHECK (juce::String (Settings::browserPreviewVolume.key) == "phaziteBrowserPreviewVolume");
    CHECK (juce::String (Settings::browserOpenness.key) == "phaziteBrowserOpenness");
    CHECK (juce::String (Settings::exportFormat.key) == "phaziteExportFormat");
    CHECK (juce::String (Settings::exportSampleRate.key) == "phaziteExportSampleRate");
    CHECK (juce::String (Settings::exportBitDepth.key) == "phaziteExportBitDepth");
    CHECK (juce::String (Settings::exportNormalize.key) == "phaziteExportNormalize");
    CHECK (juce::String (Settings::exportLoudnessTarget.key) == "phaziteExportLoudnessTarget");
    CHECK (juce::String (Settings::exportTruePeakCeiling.key) == "phaziteExportTruePeakCeiling");
    CHECK (juce::String (Settings::guide.key) == "phaziteGuide");
    CHECK (juce::String (Settings::guideStepsDone.key) == "phaziteGuideStepsDone");
}

TEST_CASE ("Settings have their defaults, and are stored as before")
{
    Settings settings; // in memory

    CHECK (settings.get (Settings::theme) == "dark");
    CHECK (settings.get (Settings::hoverHelp));
    CHECK (settings.get (Settings::computerKeyboard));
    CHECK (settings.get (Settings::interfaceScale) == doctest::Approx (1.0));
    CHECK_FALSE (settings.get (Settings::welcomeShown));
    CHECK (settings.get (Settings::keymap) == "Phazite");
    CHECK (juce::String (Settings::keymap.defaultValue)
           == juce::String (phazite::ui::Keymap::defaultName)); // the keymap the UI loads first
    CHECK (settings.get (Settings::recentProjects).isEmpty());
    CHECK (settings.get (Settings::browserFolders).isEmpty());
    CHECK (settings.get (Settings::browserPreview));
    CHECK (settings.get (Settings::browserPreviewVolume, 0.7) == doctest::Approx (0.7));
    CHECK (settings.get (Settings::browserOpenness).isEmpty());
    CHECK (settings.get (Settings::exportFormat) == "wav");
    CHECK (settings.get (Settings::exportSampleRate) == doctest::Approx (44100.0));
    CHECK (settings.get (Settings::exportBitDepth) == doctest::Approx (24.0));
    CHECK_FALSE (settings.get (Settings::exportNormalize));
    CHECK (settings.get (Settings::exportLoudnessTarget) == doctest::Approx (-14.0));
    CHECK (settings.get (Settings::exportTruePeakCeiling) == doctest::Approx (-1.0));
    CHECK (settings.get (Settings::guide));
    CHECK (settings.get (Settings::guideStepsDone).isEmpty());
    CHECK (settings.getKeyChanges ("FL Studio").isEmpty());

    settings.set (Settings::theme, "light");
    settings.set (Settings::hoverHelp, false);
    settings.set (Settings::interfaceScale, 1.25);
    settings.set (Settings::browserPreviewVolume, 0.3);
    settings.set (Settings::browserFavorites, juce::StringArray { "Kick.wav", "", "Snare.wav" });
    settings.setKeyChanges ("FL Studio", "<KEYMAPPINGS/>");

    CHECK (settings.get (Settings::theme) == "light");
    CHECK_FALSE (settings.get (Settings::hoverHelp));
    CHECK (settings.get (Settings::interfaceScale) == doctest::Approx (1.25));
    CHECK (settings.get (Settings::browserPreviewVolume, 0.7) == doctest::Approx (0.3));
    CHECK (settings.get (Settings::browserFavorites) == juce::StringArray { "Kick.wav", "Snare.wav" }); // no empty lines
    CHECK (settings.getKeyChanges ("FL Studio") == "<KEYMAPPINGS/>");

    // In a file, as Phazite always wrote them: a list as its lines, a switch as 1 or 0.
    phazite::test::TemporaryFolder temp;
    auto file = makeSettingsFile (temp.folder.getChildFile ("Settings.xml"));
    Settings stored (file.get());
    stored.set (Settings::browserFolders, juce::StringArray { "a", "b" });
    stored.set (Settings::welcomeShown, true);
    stored.set (Settings::computerKeyboard, false);
    stored.set (Settings::interfaceScale, 1.5);
    stored.setKeyChanges ("Phazite", "<KEYMAPPINGS/>");

    CHECK (file->getValue (Settings::browserFolders.key) == "a\nb");
    CHECK (file->getValue (Settings::welcomeShown.key) == "1");
    CHECK (file->getValue (Settings::computerKeyboard.key) == "0");
    CHECK (file->getDoubleValue (Settings::interfaceScale.key) == doctest::Approx (1.5));
    CHECK (file->getValue (juce::String (Settings::keyChangesPrefix) + "Phazite") == "<KEYMAPPINGS/>");
}

TEST_CASE ("Settings that change often are saved within seconds, the others at once")
{
    phazite::test::TemporaryFolder temp;
    const auto xml = temp.folder.getChildFile ("Settings.xml");
    auto file = makeSettingsFile (xml);
    Settings settings (file.get());

    // Saved soon: the file's timer writes it later (no message loop runs here, so it doesn't yet).
    settings.set (Settings::browserOpenness, "Drums");
    CHECK (readFromDisk (xml, Settings::browserOpenness.key).isEmpty());

    // Saved now, with the change that was waiting.
    settings.set (Settings::theme, "light");
    CHECK (readFromDisk (xml, Settings::theme.key) == "light");
    CHECK (readFromDisk (xml, Settings::browserOpenness.key) == "Drums");

    // saveNow() writes what is waiting.
    settings.set (Settings::browserFavorites, juce::StringArray { "Kick.wav" });
    CHECK (readFromDisk (xml, Settings::browserFavorites.key).isEmpty());
    settings.saveNow();
    CHECK (readFromDisk (xml, Settings::browserFavorites.key) == "Kick.wav");
}

TEST_CASE ("Recent projects read and write the format JUCE's list used")
{
    phazite::test::TemporaryFolder temp;
    const auto a = createFile (temp.folder.getChildFile ("A/A.phazite"));
    const auto b = createFile (temp.folder.getChildFile ("B/B.phazite"));
    const auto c = createFile (temp.folder.getChildFile ("C/C.phazite"));

    auto file = makeSettingsFile (temp.folder.getChildFile ("Settings.xml"));
    Settings settings (file.get());
    phazite::RecentProjects recent (settings);

    // What earlier versions stored with RecentlyOpenedFilesList reads back.
    juce::RecentlyOpenedFilesList earlier;
    earlier.addFile (b);
    earlier.addFile (a);
    file->setValue (Settings::recentProjects.key, earlier.toString());
    CHECK (recent.getProjects() == juce::Array<juce::File> { a, b });

    // What is stored now restores into one.
    recent.add (c);
    juce::RecentlyOpenedFilesList restored;
    restored.restoreFromString (file->getValue (Settings::recentProjects.key));
    REQUIRE (restored.getNumFiles() == 3);
    CHECK (restored.getFile (0) == c);
    CHECK (restored.getFile (1) == a);
    CHECK (restored.getFile (2) == b);
}

TEST_CASE ("Recent projects list the newest first and skip missing ones")
{
    phazite::test::TemporaryFolder temp;
    const auto a = createFile (temp.folder.getChildFile ("A/A.phazite"));
    const auto b = createFile (temp.folder.getChildFile ("B/B.phazite"));
    const auto c = createFile (temp.folder.getChildFile ("C/C.phazite"));

    Settings settings;
    phazite::RecentProjects recent (settings);

    for (const auto& file : { a, b, c, a })
        recent.add (file);

    CHECK (recent.getProjects() == juce::Array<juce::File> { a, c, b });

    recent.remove (c);
    CHECK (recent.getProjects() == juce::Array<juce::File> { a, b });

    b.getParentDirectory().deleteRecursively();
    CHECK (recent.getProjects() == juce::Array<juce::File> { a });

    // The newest ten are kept.
    juce::Array<juce::File> newestFirst;

    for (int i = 0; i < phazite::RecentProjects::maxNumProjects + 1; ++i)
    {
        const auto project = createFile (temp.folder.getChildFile ("P" + juce::String (i) + "/P.phazite"));
        recent.add (project);
        newestFirst.insert (0, project);
    }

    const auto projects = recent.getProjects();
    REQUIRE (projects.size() == phazite::RecentProjects::maxNumProjects);
    CHECK (projects.getFirst() == newestFirst.getFirst());
    CHECK_FALSE (projects.contains (newestFirst.getLast())); // the oldest of them
    CHECK_FALSE (projects.contains (a));
}
