// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AboutPanel.h"
#include "ui/theme/Icons.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int buttonWidth = 120; // as wide as "Copy Version" needs
    constexpr int listWidth = 220;   // the longest name, "PreSonus Plug-In Extensions"
    constexpr int logoSize = controlHeight * 2;

    juce::String withVersion (const juce::String& name, const juce::String& number)
    {
        return number.isEmpty() ? name : name + " " + number;
    }
} // namespace

AboutPanel::AboutPanel (juce::File licencesFolder)
    : licences (std::move (licencesFolder)),
      notices (getThirdPartyNotices()),
      logo (createLogo()),
      list ("Licenses and notices", this)
{
    setTitle ("About Phazite");

    name.setText (appName, juce::dontSendNotification);
    name.setFont (juce::FontOptions (TextSize::heading, juce::Font::bold));
    version.setText ("Version " + getVersionString(), juce::dontSendNotification);
    copyright.setText (getCopyrightNotice() + ".", juce::dontSendNotification);

    // What the GNU AGPL (section 0 and 5 d) asks every interactive program under it to show.
    legal.setText ("Phazite is free software: you may share it and change it under the GNU Affero General Public License, version 3 "
                   "or (at your option) any later version. It comes with no warranty, to the extent the law allows. The license "
                   "is first in the list below, and the code and fonts from others that Phazite is built with follow it, each with "
                   "its own.",
                   juce::dontSendNotification);
    legal.setJustificationType (juce::Justification::topLeft);
    licencesHeading.setText ("Licenses and notices", juce::dontSendNotification);
    licencesHeading.setFont (juce::FontOptions (TextSize::title, juce::Font::bold));
    trademarks.setText (getTrademarkNotices().joinIntoString (" "), juce::dontSendNotification);
    trademarks.setJustificationType (juce::Justification::topLeft);

    for (auto* label : { &version, &copyright, &legal, &trademarks })
        label->setFont (juce::FontOptions (label == &legal ? TextSize::body : TextSize::small));

    sourceButton.setTooltip ("Opens Phazite's source code on GitHub, in the browser: " + getSourceCodeAddress());
    sourceButton.onClick = [] { juce::URL (getSourceCodeAddress()).launchInDefaultBrowser(); };
    copyButton.setTooltip ("Copies Phazite's version and the system it runs on, for a report of what went wrong");
    copyButton.onClick = [] { juce::SystemClipboard::copyTextToClipboard (describeVersion()); };

    list.setTitle (list.getName());
    list.setRowHeight (ListRow::height);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setTooltip ("Phazite's license, then those of the code and fonts it is built with. Choose one to read it");

    text.setTitle ("License text");
    text.setReadOnly (true);
    text.setMultiLine (true, true);
    text.setScrollbarsShown (true);
    text.setCaretVisible (false);
    text.setFont (juce::FontOptions (TextSize::small));

    for (auto* component : std::initializer_list<juce::Component*> { &name, &version, &copyright, &legal, &sourceButton, &copyButton,
                                                                     &licencesHeading, &list, &text, &trademarks })
        addAndMakeVisible (component);

    lookAndFeelChanged();
    list.selectRow (0);
    showNotice (0);
    setSize (width, height);
}

juce::String AboutPanel::describeVersion()
{
    return juce::String (appName) + " " + getVersionString() + " (" + juce::SystemStats::getOperatingSystemName() + ", "
           + (juce::SystemStats::isOperatingSystem64Bit() ? "64-bit" : "32-bit") + ")";
}

juce::String AboutPanel::describeRow (int row) const
{
    if (row == 0)
        return juce::String (appName) + ", " + "AGPL-3.0-or-later";

    if (! juce::isPositiveAndBelow (row - 1, (int) notices.size()))
        return {};

    const auto& notice = notices[(size_t) row - 1];
    return withVersion (notice.name, notice.version) + ", " + notice.licence;
}

juce::String AboutPanel::getNoticeText (int row) const
{
    // Each row's heading lines, then its licence's text as the build put it next to the app.
    const auto readFile = [this] (const juce::String& fileName)
    {
        const auto file = licences.getChildFile (fileName);
        return file.existsAsFile() ? file.loadFileAsString()
                                   : "The license's text should be in " + file.getFullPathName() + ", but that file isn't there.";
    };

    if (row == 0)
        return withVersion (appName, getVersionString()) + "\nAGPL-3.0-or-later\n" + getCopyrightNotice() + "\n" + getSourceCodeAddress()
               + "\n\n" + readFile (licenceFileName);

    if (! juce::isPositiveAndBelow (row - 1, (int) notices.size()))
        return {};

    const auto& notice = notices[(size_t) row - 1];
    auto said = withVersion (notice.name, notice.version) + "\n" + notice.licence + "\n" + notice.copyright + "\n" + notice.use + ".\n"
                + notice.url + "\n\n";
    return said + (notice.file.isEmpty() ? juce::String ("In the public domain: there is no license to show.") : readFile (notice.file));
}

void AboutPanel::showNotice (int row)
{
    if (! juce::isPositiveAndBelow (row, getNumNotices()) || row == shown)
        return;

    shown = row;
    text.setText (getNoticeText (row), false);
    text.moveCaretToTop (false);

    if (list.getSelectedRow() != row)
        list.selectRow (row);
}

void AboutPanel::selectedRowsChanged (int lastRowSelected)
{
    showNotice (lastRowSelected);
}

void AboutPanel::paintListBoxItem (int row, juce::Graphics& g, int rowWidth, int rowHeight, bool selected)
{
    if (! juce::isPositiveAndBelow (row, getNumNotices()))
        return;

    const auto& p = getPalette (*this);
    auto area = juce::Rectangle<int> (rowWidth, rowHeight);

    if (selected)
    {
        g.setColour (p.accent.withAlpha (selectionWash));
        g.fillRoundedRectangle (area.toFloat().reduced (ListRow::inset, 0.0f), cornerRadius);
    }

    area.reduce (Space::s, 0);
    g.setFont (juce::FontOptions (TextSize::body));
    g.setColour (p.text);
    g.drawText (row == 0 ? juce::String (appName) : notices[(size_t) row - 1].name, area, juce::Justification::centredLeft, true);
}

void AboutPanel::lookAndFeelChanged()
{
    for (auto* label : { &version, &copyright, &trademarks })
        setThemeColour (*label, juce::Label::textColourId, &Palette::textMuted);

    repaint();
}

void AboutPanel::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);

    if (logo != nullptr)
        logo->drawWithin (g, juce::Rectangle<float> ((float) Space::xl, (float) Space::xl, (float) logoSize, (float) logoSize),
                          juce::RectanglePlacement::centred, 1.0f);
}

void AboutPanel::resized()
{
    auto area = getLocalBounds().reduced (Space::xl);

    // Beside the logo: the name, the version and the copyright, one under another.
    auto top = area.removeFromTop (logoSize);
    top.removeFromLeft (logoSize + Space::m);
    name.setBounds (top.removeFromTop (top.getHeight() / 2));
    version.setBounds (top.removeFromTop (top.getHeight() / 2));
    copyright.setBounds (top);

    area.removeFromTop (Space::m);
    legal.setBounds (area.removeFromTop (controlHeight * 2)); // three lines
    area.removeFromTop (Space::s);
    auto buttons = area.removeFromTop (controlHeight);
    sourceButton.setBounds (buttons.removeFromLeft (buttonWidth));
    buttons.removeFromLeft (Space::s);
    copyButton.setBounds (buttons.removeFromLeft (buttonWidth));

    area.removeFromTop (Space::l);
    licencesHeading.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (Space::xs);
    trademarks.setBounds (area.removeFromBottom (controlHeight * 2)); // up to three lines
    area.removeFromBottom (Space::s);
    list.setBounds (area.removeFromLeft (listWidth));
    area.removeFromLeft (Space::s);
    text.setBounds (area);
}
} // namespace phazite::ui
