// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Welcome.h"
#include "LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    juce::String getShowText()
    {
#if JUCE_MAC
        return "Show in Finder";
#elif JUCE_WINDOWS
        return "Show in Explorer";
#else
        return "Show in File Manager";
#endif
    }

    void setPrimary (juce::TextButton& button, bool primary, const Palette& p)
    {
        button.setColour (juce::TextButton::buttonColourId, primary ? p.accent : p.surfaceRaised);
        button.setColour (juce::TextButton::textColourOffId, primary ? p.textOnAccent : p.text);
    }
} // namespace

//==============================================================================
SoundPackPage::SoundPackPage (SoundPack& pack)
    : soundPack (pack)
{
    setTitle ("Phazite Sounds");

    title.setText ("Phazite Sounds", juce::dontSendNotification);
    title.setFont (juce::FontOptions (TextSize::title, juce::Font::bold));

    about.setText ("85 free sounds to start with: acoustic and electronic drums, 808s, loops from 90 to 174 BPM, risers, impacts "
                   "and foley. They are public domain (CC0), so you can use them in any song without credit. Phazite works "
                   "without them, and its browser shows your own folders of sounds too.",
                   juce::dontSendNotification);

    for (auto* label : { &about, &status })
        label->setJustificationType (juce::Justification::topLeft);

    mainButton.onClick = [this] { mainButtonClicked(); };
    fileButton.setTooltip ("Installs them from " + soundPack.getRelease().zip.getFileName() + ", downloaded in a browser");
    fileButton.onClick = [this] { chooseFile(); };
    showButton.setButtonText (getShowText());
    showButton.setTooltip ("Shows the folder they are in");
    showButton.onClick = [this] { soundPack.getFolder().revealToUser(); };
    progressBar.setTitle ("Download progress");

    for (auto* c : std::initializer_list<juce::Component*> { &title, &about, &mainButton, &fileButton, &showButton, &progressBar, &status })
        addAndMakeVisible (c);

    soundPack.addChangeListener (this);
    update();
}

SoundPackPage::~SoundPackPage()
{
    soundPack.removeChangeListener (this);
}

void SoundPackPage::update()
{
    using State = SoundPack::State;
    const auto& p = getPalette (*this);
    const auto& release = soundPack.getRelease();
    const auto state = soundPack.getState();
    const auto busy = state == State::downloading || state == State::installing;
    const auto installed = ! busy && soundPack.isInstalled();
    const auto size = juce::File::descriptionOfSizeInBytes (release.size);
    juce::String mainText, statusText;
    auto statusColour = p.textMuted;

    if (installed)
    {
        statusText = "Installed in " + soundPack.getFolder().getFullPathName() + ". The browser lists them under Phazite Sounds.";
    }
    else if (state == State::downloading)
    {
        mainText = "Cancel";
        statusText = "Downloading... " + juce::String (juce::roundToInt (soundPack.getProgress() * 100.0)) + "% of " + size;
    }
    else if (state == State::installing)
    {
        statusText = "Installing...";
    }
    else
    {
        if (SoundPack::canDownload())
            mainText = state == State::failed ? "Try Again" : "Download (" + size + ")";
        else
            mainText = "Open the Download Page";

        if (state == State::failed)
        {
            statusText = soundPack.getError();
            statusColour = p.danger;
        }
        else if (SoundPack::canDownload())
        {
            statusText = "From " + release.zip.getDomain() + ", into " + soundPack.getFolder().getParentDirectory().getFullPathName() + ".";
        }
        else
        {
            statusText = "This build of Phazite can't download them itself. Download " + release.zip.getFileName()
                         + " from their page in a browser, then install it from the file.";
        }
    }

    mainButton.setButtonText (mainText);
    mainButton.setVisible (mainText.isNotEmpty());
    setPrimary (mainButton, state != State::downloading, p);
    fileButton.setVisible (! busy && ! installed);
    showButton.setVisible (installed);
    progress = state == State::installing ? -1.0 : soundPack.getProgress();
    progressBar.setVisible (busy);
    status.setText (statusText, juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, statusColour);
    resized();
}

void SoundPackPage::mainButtonClicked()
{
    if (soundPack.getState() == SoundPack::State::downloading)
        soundPack.cancel();
    else if (SoundPack::canDownload())
        soundPack.download();
    else
        soundPack.getRelease().page.launchInDefaultBrowser();
}

void SoundPackPage::chooseFile()
{
    const auto downloads = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads");
    chooser = std::make_unique<juce::FileChooser> ("Install Phazite Sounds from a file", downloads.isDirectory() ? downloads : juce::File(),
                                                   "*.zip");

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safeThis = juce::Component::SafePointer<SoundPackPage> (this)] (const juce::FileChooser& c)
                          {
                              if (safeThis != nullptr && c.getResult().existsAsFile())
                                  safeThis->soundPack.installFromFile (c.getResult());
                          });
}

void SoundPackPage::resized()
{
    auto area = getLocalBounds();
    title.setBounds (area.removeFromTop (controlHeight));
    about.setBounds (area.removeFromTop (88));
    area.removeFromTop (Space::m);

    auto buttons = area.removeFromTop (controlHeight);

    for (auto* button : { &mainButton, &fileButton, &showButton })
    {
        if (button->isVisible())
        {
            button->setBounds (buttons.removeFromLeft (juce::jmax (120, button->getBestWidthForHeight (controlHeight))));
            buttons.removeFromLeft (Space::s);
        }
    }

    area.removeFromTop (Space::m);

    if (progressBar.isVisible())
    {
        progressBar.setBounds (area.removeFromTop (controlHeight));
        area.removeFromTop (Space::s);
    }

    status.setBounds (area.removeFromTop (64));
}

//==============================================================================
WelcomePanel::WelcomePanel (SoundPack& pack)
    : soundPack (pack),
      soundPackPage (pack)
{
    setTitle ("Welcome to Phazite");

    heading.setText ("Welcome to Phazite", juce::dontSendNotification);
    heading.setFont (juce::FontOptions (TextSize::heading, juce::Font::bold));
    intro.setText ("Everything here is optional, and Help > Welcome brings this window back.", juce::dontSendNotification);

    closeButton.onClick = [this]
    {
        if (onClose != nullptr)
            onClose();

        if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState (0); // which deletes it
    };

    for (auto* c : std::initializer_list<juce::Component*> { &heading, &intro, &soundPackPage, &closeButton })
        addAndMakeVisible (c);

    soundPack.addChangeListener (this);
    updateCloseButton();
    lookAndFeelChanged();
}

WelcomePanel::~WelcomePanel()
{
    soundPack.removeChangeListener (this);
}

void WelcomePanel::showPage (Page page)
{
    soundPackPage.setVisible (page == sounds);
}

void WelcomePanel::updateCloseButton()
{
    // Not Now, until something has been done about the page.
    const auto state = soundPack.getState();
    const auto untouched = (state == SoundPack::State::notInstalled || state == SoundPack::State::failed) && ! soundPack.isInstalled();
    closeButton.setButtonText (untouched ? "Not Now" : "Done");
    resized();
}

void WelcomePanel::lookAndFeelChanged()
{
    intro.setColour (juce::Label::textColourId, getPalette (*this).textMuted);
}

void WelcomePanel::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void WelcomePanel::resized()
{
    auto area = getLocalBounds().reduced (Space::xl);
    heading.setBounds (area.removeFromTop (32));
    intro.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (Space::l);

    auto footer = area.removeFromBottom (controlHeight);
    closeButton.setBounds (footer.removeFromRight (juce::jmax (96, closeButton.getBestWidthForHeight (controlHeight))));
    area.removeFromBottom (Space::l);

    soundPackPage.setBounds (area);
}
} // namespace phazite::ui
