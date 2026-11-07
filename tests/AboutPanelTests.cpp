// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's notices (resources/licenses/notices.json, model/engine/AppInfo.h), as the build writes them next to the app
// (cmake/PhaziteNotices.cmake: PHAZITE_NOTICES_DIR is that folder, the tests' build of it), and the About window that shows
// them (ui/shell/AboutPanel.h).
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/engine/AppInfo.h"
#include "ui/shell/AboutPanel.h"

namespace
{
juce::File getNoticesFolder()
{
    return juce::File (PHAZITE_NOTICES_DIR);
}

juce::File getLicencesFolder()
{
    return getNoticesFolder().getChildFile ("licenses");
}

const phazite::ThirdPartyNotice* findNotice (const std::vector<phazite::ThirdPartyNotice>& notices, const juce::String& name)
{
    for (const auto& notice : notices)
        if (notice.name == name)
            return &notice;

    return nullptr;
}
} // namespace

TEST_CASE ("The notices name everything from others that Phazite is built with, each with its license's text next to the app")
{
    const auto notices = phazite::getThirdPartyNotices();
    REQUIRE (notices.size() > 20);

    for (const auto* name :
         { "JUCE", "zlib", "HarfBuzz", "SheenBidi", "LunaSVG", "libpng", "libjpeg", "libwebp", "FLAC", "libvorbis", "Opus", "VST 3 SDK",
           "Rubber Band Library", "r8brain-free-src", "Ooura FFT", "libebur128", "Inter", "Lucide", "Code - OSS" })
    {
        INFO (name);
        CHECK (findNotice (notices, name) != nullptr);
    }

    // The ASIO SDK is compiled into Windows' builds only, and only they name it.
#if JUCE_WINDOWS
    CHECK (findNotice (notices, "ASIO SDK") != nullptr);
#else
    CHECK (findNotice (notices, "ASIO SDK") == nullptr);
#endif

    // Each says what it is, and its license's text is in the folder, which holds nothing else but Phazite's.
    juce::StringArray files { phazite::licenceFileName };

    for (const auto& notice : notices)
    {
        INFO (notice.name.toStdString());
        CHECK (notice.licence.isNotEmpty());
        CHECK (notice.copyright.isNotEmpty());
        CHECK (notice.use.isNotEmpty());
        CHECK (notice.url.startsWith ("http"));

        if (notice.file.isEmpty())
        {
            CHECK (notice.licence == "Public domain");
            continue;
        }

        CHECK (getLicencesFolder().getChildFile (notice.file).getSize() > 100);
        CHECK_FALSE (files.contains (notice.file)); // a file of its own
        files.add (notice.file);
    }

    for (const auto& file : getLicencesFolder().findChildFiles (juce::File::findFiles, false))
    {
        INFO (file.getFileName().toStdString());
        CHECK (files.contains (file.getFileName()));
    }

    CHECK (getLicencesFolder().getChildFile (phazite::licenceFileName).loadFileAsString().contains ("GNU AFFERO GENERAL PUBLIC LICENSE"));
}

TEST_CASE ("The licenses' texts are what their licenses ask to go with every copy")
{
    const auto read = [] (const juce::String& file) { return getLicencesFolder().getChildFile (file).loadFileAsString(); };

    // Rubber Band's GPL, version 2; Inter's OFL, with its copyright notice.
    CHECK (read ("RubberBand.txt").contains ("GNU GENERAL PUBLIC LICENSE"));
    CHECK (read ("RubberBand.txt").contains ("Version 2, June 1991"));
    CHECK (read ("Inter.txt").contains ("SIL OPEN FONT LICENSE Version 1.1"));
    CHECK (read ("Inter.txt").contains ("Copyright (c) 2016 The Inter Project Authors"));

    // JUCE's notice, with the AGPL's text; the IJG's README; the ASIO SDK's, with the GPL's text, version 3, it is used under.
    CHECK (read ("JUCE.txt").contains ("The JUCE Framework"));
    CHECK (read ("JUCE.txt").contains ("GNU AFFERO GENERAL PUBLIC LICENSE"));
    CHECK (read ("libjpeg.txt").contains ("Independent JPEG Group"));
    CHECK (read ("r8brain-free-src.txt").contains ("Aleksey Vaneev"));
    CHECK (read ("Ooura-FFT.txt").contains ("Takuya OOURA"));

#if JUCE_WINDOWS
    CHECK (read ("ASIO-SDK.txt").contains ("Steinberg ASIO License"));
    CHECK (read ("ASIO-SDK.txt").contains ("Version 3, 29 June 2007"));
#endif
}

TEST_CASE ("NOTICES.txt gives Phazite's notice, every notice's license and copyright, and the trademarks")
{
    const auto text = getNoticesFolder().getChildFile ("NOTICES.txt").loadFileAsString();
    CHECK (text.contains ("Phazite " + phazite::getVersionString()));
    CHECK (text.contains (phazite::getCopyrightNotice()));
    CHECK (text.contains ("GNU Affero"));
    CHECK (text.contains ("WITHOUT ANY WARRANTY"));
    CHECK (text.contains (phazite::getSourceCodeAddress()));
    CHECK (text.contains ("This software is based in part on the work of the Independent JPEG Group")); // as the IJG asks

    for (const auto& notice : phazite::getThirdPartyNotices())
    {
        INFO (notice.name.toStdString());
        CHECK (text.contains (notice.name));
        CHECK (text.contains ("(" + notice.licence + ")"));
        CHECK (text.contains (notice.copyright));
    }

    for (const auto& sentence : phazite::getTrademarkNotices())
        CHECK (text.contains (sentence));
}

TEST_CASE ("The trademarks' notices say what Steinberg asks, in the words it asks for")
{
    const auto sentences = phazite::getTrademarkNotices();
    CHECK (sentences.contains ("VST is a registered trademark of Steinberg Media Technologies GmbH."));

#if JUCE_WINDOWS
    CHECK (sentences.contains ("ASIO is a registered trademark of Steinberg Media Technologies GmbH."));
#else
    CHECK_FALSE (sentences.joinIntoString (" ").contains ("ASIO"));
#endif

    // The first mention of each in the About window comes with its sign (the notices before the trademarks).
    const auto notices = phazite::getThirdPartyNotices();
    CHECK (findNotice (notices, "VST 3 SDK")->use.contains (juce::CharPointer_UTF8 ("VST\xc2\xae 3")));
}

TEST_CASE ("The About window shows Phazite's version, copyright and license notice, and the text of each license chosen")
{
    phazite::ui::AboutPanel panel (getLicencesFolder());
    phazite::test::TestWindow window (panel, phazite::ui::AboutPanel::width, phazite::ui::AboutPanel::height);

    CHECK (panel.getVersionText() == "Version " + phazite::getVersionString());
    CHECK (panel.getLegalNotice().contains ("GNU Affero General Public License"));
    CHECK (panel.getLegalNotice().contains ("no warranty"));
    CHECK (panel.getTrademarks().contains ("VST is a registered trademark of Steinberg Media Technologies GmbH."));

    // Phazite's own first, its license showing.
    const auto notices = phazite::getThirdPartyNotices();
    CHECK (panel.getNumNotices() == (int) notices.size() + 1);
    CHECK (panel.getShownNotice() == 0);
    CHECK (panel.describeRow (0) == "Phazite, AGPL-3.0-or-later");
    CHECK (panel.getShownText().contains (phazite::getCopyrightNotice()));
    CHECK (panel.getShownText().contains ("GNU AFFERO GENERAL PUBLIC LICENSE"));

    // A click on another shows its notice and its license.
    auto row = -1;

    for (int i = 0; i < (int) notices.size(); ++i)
        if (notices[(size_t) i].name == "Rubber Band Library")
            row = i + 1;

    REQUIRE (row > 0);
    CHECK (panel.describeRow (row) == "Rubber Band Library 4.0, GPL-2.0-or-later");
    auto& list = panel.getList();
    list.scrollToEnsureRowIsOnscreen (row);
    phazite::test::Mouse mouse (list);
    mouse.click (list.getRowPosition (row, true).getCentre());
    CHECK (panel.getShownNotice() == row);
    CHECK (panel.getShownText().startsWith ("Rubber Band Library 4.0\nGPL-2.0-or-later\nCopyright 2007-2024 Particular Programs Ltd."));
    CHECK (panel.getShownText().contains ("GNU GENERAL PUBLIC LICENSE"));

    // What is in the public domain says so; the list and the text have names for screen readers.
    for (int i = 0; i < (int) notices.size(); ++i)
        if (notices[(size_t) i].file.isEmpty())
            panel.showNotice (i + 1);

    CHECK (panel.getShownText().contains ("public domain"));
    CHECK (list.getTitle().isNotEmpty());
    CHECK (panel.getSourceButton().getTooltip().contains (phazite::getSourceCodeAddress()));
    CHECK (panel.getCopyButton().getTooltip().isNotEmpty());
    CHECK (phazite::ui::AboutPanel::describeVersion().startsWith ("Phazite " + phazite::getVersionString() + " ("));
}

TEST_CASE ("A license's text that isn't next to the app says where it should be")
{
    phazite::test::TemporaryFolder empty;
    phazite::ui::AboutPanel panel (empty.folder);
    CHECK (panel.getShownText().contains ("should be in " + empty.folder.getChildFile (phazite::licenceFileName).getFullPathName()));
}
