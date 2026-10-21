// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Welcome.h"
#include "ui/components/Controls.h"
#include "ui/components/ValueText.h"
#include "ui/theme/LookAndFeel.h"

#include <juce_audio_utils/juce_audio_utils.h> // AudioDeviceSelectorComponent

namespace phazite::ui
{
namespace
{
    constexpr int titleHeight = controlHeight;
    constexpr int choiceDescriptionHeight = 40; // two lines
    constexpr int helpHeight = 56;              // three lines
    constexpr int testButtonWidth = 160;
    constexpr int footerButtonWidth = 96;

    struct ComingFromChoice
    {
        const char* name;
        const char* description;
        const char* keymap;
    };

    // clang-format off
    constexpr ComingFromChoice comingFromChoices[ComingFromPage::numChoices] {
        { "FL Studio",
          "FL Studio's shortcuts, and its playlist's mouse: a click places the last clip you used, right-click deletes, and "
          "Ctrl+drag selects a time range that loops.",
          "FL Studio" },
        { "Ableton Live, Logic Pro or another app",
          "Phazite's shortcuts, which follow what most music apps share: Space plays, R records, Ctrl+D duplicates.",
          "Phazite" },
        { "None: I'm new to making music",
          "Phazite's shortcuts. The hint bar along the bottom of the window suggests what to try next.",
          "Phazite" },
    };
    // clang-format on

    constexpr int comingFromGroup = 1; // the choices' radio group

    void makeTitle (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::FontOptions (TextSize::title, juce::Font::bold));
    }
} // namespace

//==============================================================================
ComingFromPage::ComingFromPage (const juce::String& keymapName, std::function<void (const juce::String&)> choose)
    : chooseKeymap (std::move (choose))
{
    setTitle ("Coming from another app?");
    makeTitle (title, "Coming from another music app?");
    about.setText ("Phazite can use the shortcuts you know. Options > Keymap changes them later.", juce::dontSendNotification);

    for (const auto& choice : comingFromChoices)
    {
        auto* button = choices.add (new juce::ToggleButton (choice.name));
        button->setRadioGroupId (comingFromGroup);
        button->setToggleState (keymapName == "FL Studio" && juce::String (choice.keymap) == keymapName, juce::dontSendNotification);
        // JUCE calls this for the choice a click turns off too.
        button->onClick = [this, button, keymap = juce::String (choice.keymap)]
        {
            if (button->getToggleState() && chooseKeymap != nullptr)
                chooseKeymap (keymap);
        };

        auto* description = descriptions.add (new juce::Label ({}, choice.description));
        description->setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (button);
        addAndMakeVisible (description);
    }

    addAndMakeVisible (title);
    addAndMakeVisible (about);
    lookAndFeelChanged();
}

int ComingFromPage::getChosen() const
{
    for (int i = 0; i < choices.size(); ++i)
        if (choices[i]->getToggleState())
            return i;

    return -1;
}

void ComingFromPage::lookAndFeelChanged()
{
    const auto& p = getPalette (*this);
    about.setColour (juce::Label::textColourId, p.textMuted);

    for (auto* description : descriptions)
        description->setColour (juce::Label::textColourId, p.textMuted);
}

void ComingFromPage::resized()
{
    auto area = getLocalBounds();
    title.setBounds (area.removeFromTop (titleHeight));
    about.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (Space::m);

    for (int i = 0; i < choices.size(); ++i)
    {
        choices[i]->setBounds (area.removeFromTop (controlHeight));
        descriptions[i]->setBounds (area.removeFromTop (choiceDescriptionHeight).withTrimmedLeft (Space::xl));
        area.removeFromTop (Space::s);
    }
}

//==============================================================================
SoundPage::SoundPage (juce::AudioDeviceManager& manager, std::function<bool()> isBluetooth)
    : deviceManager (manager),
      isOutputBluetooth (std::move (isBluetooth))
{
    setTitle ("Sound");
    makeTitle (title, "Hear Phazite");
    about.setText ("Turn your volume down a little, then click Play a Test Sound: you should hear a beep for a second. If you don't, "
                   "choose another output below.",
                   juce::dontSendNotification);
#if JUCE_WINDOWS
    help.setText ("With an audio interface, choose ASIO as the device type, then its driver: sound comes with less delay. Some ASIO "
                  "drivers play one app at a time, so others may go quiet while Phazite is open.",
                  juce::dontSendNotification);
#else
    help.setText ("With an audio interface, choose it as the output: sound comes with less delay.", juce::dontSendNotification);
#endif

    warning.setText ("This output is Bluetooth, which adds about 150 to 250 ms between a key and its sound: fine for listening, "
                     "too slow for playing. For playing and recording, plug in wired headphones or speakers and choose them below.",
                     juce::dontSendNotification);

    for (auto* label : { &about, &help, &warning })
        label->setJustificationType (juce::Justification::topLeft);

    testButton.setTooltip ("Plays a beep through the output chosen below");
    testButton.onClick = [this] { deviceManager.playTestSound(); };

    for (auto* c : std::initializer_list<juce::Component*> { &title, &about, &testButton, &help })
        addAndMakeVisible (c);

    addChildComponent (warning);
    deviceManager.addChangeListener (this);
    changeListenerCallback (nullptr);
    lookAndFeelChanged();
}

SoundPage::~SoundPage()
{
    deviceManager.removeChangeListener (this);
}

void SoundPage::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const auto bluetooth = isOutputBluetooth != nullptr && isOutputBluetooth();

    if (bluetooth != warning.isVisible())
    {
        warning.setVisible (bluetooth);
        resized();
    }
}

void SoundPage::visibilityChanged()
{
    // Made on first showing: it scans the audio devices. No inputs (the audio settings have those), outputs as stereo
    // pairs, and the sample rate and buffer size behind a button.
    if (isVisible() && deviceChooser == nullptr)
    {
        deviceChooser = std::make_unique<juce::AudioDeviceSelectorComponent> (deviceManager, 0, 0, 1, 2, false, false, true, true);
        deviceChooser->setTitle ("Audio output");
        addAndMakeVisible (*deviceChooser);
        resized();
    }
}

void SoundPage::lookAndFeelChanged()
{
    const auto& p = getPalette (*this);
    about.setColour (juce::Label::textColourId, p.textMuted);
    help.setColour (juce::Label::textColourId, p.textMuted);
    warning.setColour (juce::Label::textColourId, p.warning);
    setPrimary (testButton, true);
}

void SoundPage::resized()
{
    auto area = getLocalBounds();
    title.setBounds (area.removeFromTop (titleHeight));
    about.setBounds (area.removeFromTop (choiceDescriptionHeight));
    area.removeFromTop (Space::s);
    testButton.setBounds (
        area.removeFromTop (controlHeight).removeFromLeft (juce::jmax (testButtonWidth, testButton.getBestWidthForHeight (controlHeight))));
    area.removeFromTop (Space::m);
    help.setBounds (area.removeFromBottom (helpHeight));
    area.removeFromBottom (Space::s);

    if (warning.isVisible())
    {
        warning.setBounds (area.removeFromBottom (helpHeight));
        area.removeFromBottom (Space::s);
    }

    if (deviceChooser != nullptr)
        deviceChooser->setBounds (area);
}

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
    showButton.setButtonText (getShowInFolderText());
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
    setPrimary (mainButton, state != State::downloading);
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
WelcomePanel::WelcomePanel (SoundPack& pack, juce::AudioDeviceManager& deviceManager, const juce::String& keymapName,
                            std::function<void (const juce::String&)> chooseKeymap, std::function<bool()> isOutputBluetooth)
    : soundPack (pack),
      comingFromPage (keymapName, std::move (chooseKeymap)),
      soundPage (deviceManager, std::move (isOutputBluetooth)),
      soundPackPage (pack)
{
    setTitle ("Welcome to Phazite");

    heading.setText ("Welcome to Phazite", juce::dontSendNotification);
    heading.setFont (juce::FontOptions (TextSize::heading, juce::Font::bold));
    intro.setText ("Three things to set up, all of them optional. Help > Welcome brings this window back.", juce::dontSendNotification);

    closeButton.onClick = [this]
    {
        if (onClose != nullptr)
            onClose();

        if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState (0); // which deletes it
    };
    backButton.onClick = [this] { showPage ((Page) juce::jmax (0, (int) page - 1)); };
    nextButton.onClick = [this] { showPage ((Page) juce::jmin (numPages - 1, (int) page + 1)); };

    for (auto* c : std::initializer_list<juce::Component*> { &heading, &intro, &stepLabel, &backButton, &closeButton, &nextButton })
        addAndMakeVisible (c);

    for (auto* c : std::initializer_list<juce::Component*> { &comingFromPage, &soundPage, &soundPackPage })
        addChildComponent (c);

    soundPack.addChangeListener (this);
    lookAndFeelChanged();
    showPage (comingFrom);
}

WelcomePanel::~WelcomePanel()
{
    soundPack.removeChangeListener (this);
}

void WelcomePanel::showPage (Page newPage)
{
    page = newPage;
    comingFromPage.setVisible (page == comingFrom);
    soundPage.setVisible (page == sound);
    soundPackPage.setVisible (page == sounds);
    stepLabel.setText (juce::String ((int) page + 1) + " of " + juce::String (numPages), juce::dontSendNotification);
    updateButtons();
}

void WelcomePanel::updateButtons()
{
    // Not Now until the last page, and on it until something has been done about Phazite Sounds.
    const auto state = soundPack.getState();
    const auto untouched = (state == SoundPack::State::notInstalled || state == SoundPack::State::failed) && ! soundPack.isInstalled();
    const auto last = page == numPages - 1;
    closeButton.setButtonText (last && ! untouched ? "Done" : "Not Now");
    backButton.setVisible (page > 0);
    nextButton.setVisible (! last);
    resized();
}

void WelcomePanel::lookAndFeelChanged()
{
    const auto& p = getPalette (*this);
    intro.setColour (juce::Label::textColourId, p.textMuted);
    stepLabel.setColour (juce::Label::textColourId, p.textMuted);
    setPrimary (nextButton, true);
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
    area.removeFromBottom (Space::l);
    const auto buttonWidth = [] (juce::TextButton& button)
    { return juce::jmax (footerButtonWidth, button.getBestWidthForHeight (controlHeight)); };

    if (nextButton.isVisible())
    {
        nextButton.setBounds (footer.removeFromRight (buttonWidth (nextButton)));
        footer.removeFromRight (Space::s);
    }

    closeButton.setBounds (footer.removeFromRight (buttonWidth (closeButton)));
    backButton.setBounds (footer.removeFromLeft (buttonWidth (backButton)));
    footer.removeFromLeft (Space::m);
    stepLabel.setBounds (footer.removeFromLeft (footerButtonWidth));

    for (auto* c : std::initializer_list<juce::Component*> { &comingFromPage, &soundPage, &soundPackPage })
        c->setBounds (area);
}
} // namespace phazite::ui
