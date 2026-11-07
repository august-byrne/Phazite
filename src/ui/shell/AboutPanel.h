// SPDX-License-Identifier: AGPL-3.0-or-later
// The About window (Help > About Phazite; docs/design.md, section 22): Phazite's logo, name, version and copyright, the
// notice the GNU AGPL asks every interactive program under it to show (no warranty, free to share and change, where its text
// is), its source code's address, and every licence and notice it ships with: a list, Phazite first, then the code and fonts
// from others (model/engine/AppInfo.h: getThirdPartyNotices()), whose selected row's text shows beside it, read from the
// licences' folder next to the app; and the trademarks' sentences under them.
#pragma once

#include "model/engine/AppInfo.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class AboutPanel : public juce::Component,
                   private juce::ListBoxModel
{
public:
    // The licences' texts come from that folder: the app's, or the tests' build of it.
    explicit AboutPanel (juce::File licencesFolder = getLicencesFolder());

    static constexpr int width = 680, height = 620;

    // For tests: the rows (Phazite, then the notices), as screen readers say them; which one shows; what shows of it;
    // the legal notice and the trademarks' sentences; and the buttons.
    int getNumNotices() const noexcept { return (int) notices.size() + 1; }
    juce::String describeRow (int row) const;
    void showNotice (int row);
    int getShownNotice() const noexcept { return shown; }
    juce::String getShownText() const { return text.getText(); }
    juce::String getLegalNotice() const { return legal.getText(); }
    juce::String getTrademarks() const { return trademarks.getText(); }
    juce::String getVersionText() const { return version.getText(); }
    juce::ListBox& getList() noexcept { return list; }
    juce::Button& getSourceButton() noexcept { return sourceButton; }
    juce::Button& getCopyButton() noexcept { return copyButton; }

    // What Copy Version copies: Phazite's version, and the system it runs on, for a report of what went wrong.
    static juce::String describeVersion();

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;

private:
    const juce::File licences;
    const std::vector<ThirdPartyNotice> notices;
    std::unique_ptr<juce::Drawable> logo;
    juce::Label name, version, copyright, legal, licencesHeading, trademarks;
    juce::TextButton sourceButton { "Source Code" }, copyButton { "Copy Version" };
    juce::ListBox list;
    juce::TextEditor text;
    int shown = -1;

    juce::String getNoticeText (int row) const; // the row's heading lines, then its licence's text

    int getNumRows() override { return getNumNotices(); }
    void paintListBoxItem (int row, juce::Graphics&, int rowWidth, int rowHeight, bool selected) override;
    void selectedRowsChanged (int lastRowSelected) override;
    juce::String getNameForRow (int row) override { return describeRow (row); }

    JUCE_DECLARE_NON_COPYABLE (AboutPanel)
};
} // namespace phazite::ui
