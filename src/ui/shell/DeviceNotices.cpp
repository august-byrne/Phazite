// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceNotices.h"
#include "ui/components/Controls.h"
#include "ui/theme/Icons.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <cmath>

namespace phazite::ui
{
namespace
{
    const auto closeSize = juce::roundToInt (TextSize::title) + Space::xs; // as the lesson card's
    constexpr int timerHz = 4;

    juce::String getMessageFor (const AudioDeviceName& device)
    {
        return device.name + (device.isInput ? " is connected. Record from it?" : " is connected. Play through it?");
    }

    // Whether the pointer is over a component or what it holds, as the mouse's events last found it. (Component::isMouseOver()
    // asks also where the system's pointer is now, which the tests' events don't move.)
    bool isUnderPointer (const juce::Component& component)
    {
        for (const auto& source : juce::Desktop::getInstance().getMouseSources())
            if (const auto* under = source.getComponentUnderMouse();
                under != nullptr && (under == &component || component.isParentOf (under)))
                return true;

        return false;
    }

    // How tall text is, wrapped at a width, with a little to spare, as a label breaks lines a little differently.
    int getTextHeight (const juce::String& text, const juce::FontOptions& font, int width)
    {
        if (text.isEmpty())
            return 0;

        juce::AttributedString attributed (text);
        attributed.setFont (juce::Font (font));
        attributed.setWordWrap (juce::AttributedString::byWord);
        juce::TextLayout layout;
        layout.createLayout (attributed, (float) juce::jmax (1, width));
        return (int) std::ceil (layout.getHeight() + font.getHeight() / 4.0f);
    }
} // namespace

//==============================================================================
class DeviceNotices::Notice final : public NoticeCard
{
public:
    Notice (DeviceNotices& notices, AudioDeviceName name)
        : device (std::move (name)),
          owner (notices),
          icon (device.isInput ? Icon::sound : Icon::preview)
    {
        setTitle (device.isInput ? "Audio Input Connected" : "Audio Output Connected");
        message.setFont (getMessageFont());
        message.setJustificationType (juce::Justification::topLeft);
        message.setMinimumHorizontalScale (1.0f);
        detail.setFont (getDetailFont());
        detail.setJustificationType (juce::Justification::topLeft);
        detail.setMinimumHorizontalScale (1.0f);
        setThemeColour (detail, juce::Label::textColourId, &Palette::textMuted);

        // A Bluetooth output says what it costs, as the audio settings do.
        if (isBluetoothDevice (device))
            detail.setText ("Bluetooth adds about 150 to 250 ms between a key and its sound: fine for listening, too slow for playing.",
                            juce::dontSendNotification);

        showMessage (getMessageFor (device));
        switchButton.setTooltip (device.isInput ? "Records from it from now on, as choosing it in the audio settings would"
                                                : "Plays through it from now on, as choosing it in the audio settings would");
        setPrimary (switchButton, true);
        switchButton.onClick = [this] { switchToIt(); };
        closeButton.setTooltip ("Closes the notice. The audio settings switch devices too");
        closeButton.onClick = [this] { owner.close (*this); };

        for (auto* label : { &message, &detail })
            label->setInterceptsMouseClicks (false, false);

        // A click on it leaves the keys where they were (the editor's, for Space to play), the notice going after it.
        for (auto* button : std::initializer_list<juce::Button*> { &switchButton, &closeButton })
            button->setMouseClickGrabsKeyboardFocus (false);

        for (auto* c : std::initializer_list<juce::Component*> { &message, &detail, &switchButton, &closeButton })
            addAndMakeVisible (c);

        detail.setVisible (detail.getText().isNotEmpty());
    }

    const AudioDeviceName device;
    double shown = 0.0; // seconds it has shown while Phazite was in front and the pointer wasn't over it

    juce::String getText() const { return message.getText(); }
    juce::Button& getSwitchButton() noexcept { return switchButton; }
    juce::Button& getCloseButton() noexcept { return closeButton; }

    int getHeightFor (int width) const
    {
        const auto textWidth = getTextWidth (width);
        auto height = std::max (closeSize, getTextHeight (message.getText(), getMessageFont(), textWidth));

        if (detail.getText().isNotEmpty())
            height += Space::xs + getTextHeight (detail.getText(), getDetailFont(), textWidth);

        if (switchButton.isVisible())
            height += Space::s + controlHeight;

        return 2 * (getMargin() + Space::m) + height;
    }

    void paint (juce::Graphics& g) override
    {
        NoticeCard::paint (g);

        // Its icon by its first line, in the focus colour, as VS Code's notifications show theirs in the information's.
        const auto first = getCardArea().reduced (Space::m).removeFromTop (closeSize);
        icon.draw (
            g, first.withWidth ((int) NoticeTint::iconSize).toFloat().withSizeKeepingCentre (NoticeTint::iconSize, NoticeTint::iconSize),
            getPalette (*this).focus);
    }

    void resized() override
    {
        auto area = getCardArea().reduced (Space::m);

        if (switchButton.isVisible())
        {
            auto footer = area.removeFromBottom (controlHeight);
            switchButton.setBounds (footer.removeFromRight (NoticeTint::buttonWidth));
            area.removeFromBottom (Space::s);
        }

        area.removeFromLeft ((int) NoticeTint::iconSize + Space::s);
        closeButton.setBounds (area.removeFromRight (closeSize).removeFromTop (closeSize));
        area.removeFromRight (Space::xs);
        const auto textWidth = area.getWidth();
        message.setBounds (area.removeFromTop (std::max (closeSize, getTextHeight (message.getText(), getMessageFont(), textWidth))));

        if (detail.getText().isNotEmpty())
        {
            area.removeFromTop (Space::xs);
            detail.setBounds (area);
        }
    }

private:
    DeviceNotices& owner;
    TintedIcon icon;
    juce::Label message, detail;
    juce::TextButton switchButton { "Switch" };
    IconButton closeButton { "Close the Notice", Icon::close };

    static juce::FontOptions getMessageFont() { return juce::FontOptions (TextSize::body); }
    static juce::FontOptions getDetailFont() { return juce::FontOptions (TextSize::small); }

    // The text's width in a card so wide: the card's, but its padding, the icon and the close button.
    static int getTextWidth (int width)
    {
        return width - 2 * (getMargin() + Space::m) - (int) NoticeTint::iconSize - Space::s - closeSize - Space::xs;
    }

    void showMessage (const juce::String& text)
    {
        message.setText (text, juce::dontSendNotification);
        setDescription (text); // what screen readers say of the notice
    }

    void switchToIt()
    {
        // It closes, done; or it stays, with what went wrong, and the device stays as it was.
        if (const auto error = switchToAudioDevice (owner.devices, device); error.isNotEmpty())
        {
            showMessage ("Phazite couldn't switch to " + device.name + ": " + error);
            switchButton.setVisible (false);
            owner.noticesChanged();
            return;
        }

        owner.close (*this); // which deletes it
    }
};

//==============================================================================
DeviceNotices::DeviceNotices (juce::AudioDeviceManager& manager)
    : devices (manager),
      watch (manager)
{
    setTitle ("Notices");
    setInterceptsMouseClicks (false, true); // between the cards, what is under them takes clicks
    watch.onChange = [this] (const auto& plugged, const auto& gone) { devicesChanged (plugged, gone); };
    setVisible (false);
}

DeviceNotices::~DeviceNotices() = default;

void DeviceNotices::devicesChanged (const std::vector<AudioDeviceName>& plugged, const std::vector<AudioDeviceName>& gone)
{
    // A device gone takes its notice with it; one plugged in shows one, the latest at the bottom, the oldest going first
    // where too many show.
    std::erase_if (notices, [&gone] (const std::unique_ptr<Notice>& notice)
                   { return std::find (gone.begin(), gone.end(), notice->device) != gone.end(); });

    for (const auto& device : plugged)
    {
        std::erase_if (notices, [&device] (const std::unique_ptr<Notice>& notice) { return notice->device == device; });
        auto& notice = *notices.emplace_back (std::make_unique<Notice> (*this, device));
        addAndMakeVisible (notice);
    }

    for (auto shown = getShown(); (int) shown.size() > NoticeTint::mostShown; shown = getShown())
        std::erase_if (notices, [oldest = shown.front()] (const std::unique_ptr<Notice>& notice) { return notice.get() == oldest; });

    noticesChanged();
}

void DeviceNotices::close (const Notice& closed)
{
    // Gone from the column at once; deleted once the click that closed it has finished with it.
    for (auto& notice : notices)
        if (notice.get() == &closed)
            notice->setVisible (false);

    noticesChanged();
    juce::MessageManager::callAsync (
        [safe = juce::Component::SafePointer<DeviceNotices> (this)]
        {
            if (safe != nullptr)
                std::erase_if (safe->notices, [] (const std::unique_ptr<Notice>& notice) { return ! notice->isVisible(); });
        });
}

std::vector<DeviceNotices::Notice*> DeviceNotices::getShown() const
{
    std::vector<Notice*> shown;

    for (const auto& notice : notices)
        if (notice->isVisible())
            shown.push_back (notice.get());

    return shown;
}

int DeviceNotices::getNumNotices() const
{
    return (int) getShown().size();
}

void DeviceNotices::noticesChanged()
{
    const auto any =
        std::any_of (notices.begin(), notices.end(), [] (const std::unique_ptr<Notice>& notice) { return notice->isVisible(); });
    setVisible (any);

    if (any && ! isTimerRunning())
    {
        lastTick = juce::Time::getMillisecondCounterHiRes();
        startTimerHz (timerHz);
    }
    else if (! any)
    {
        stopTimer();
    }

    resized();

    if (onChange != nullptr)
        onChange();
}

int DeviceNotices::getHeightFor (int width) const
{
    // One over another, so far apart, their shadows' room overlapping.
    auto height = 0, shownCount = 0;

    for (const auto& notice : notices)
    {
        if (! notice->isVisible())
            continue;

        height += notice->getHeightFor (width);
        ++shownCount;
    }

    return shownCount == 0 ? 0 : height - (shownCount - 1) * (2 * NoticeCard::getMargin() - NoticeTint::gap);
}

void DeviceNotices::resized()
{
    // From the bottom up, the latest at the bottom.
    auto bottom = getHeight();

    for (auto it = notices.rbegin(); it != notices.rend(); ++it)
    {
        auto& notice = **it;

        if (! notice.isVisible())
            continue;

        const auto height = notice.getHeightFor (getWidth());
        notice.setBounds (0, bottom - height, getWidth(), height);
        bottom = notice.getY() + 2 * NoticeCard::getMargin() - NoticeTint::gap;
    }
}

juce::String DeviceNotices::getText (int index) const
{
    return getShown()[(size_t) index]->getText();
}

juce::Button& DeviceNotices::getSwitchButton (int index)
{
    return getShown()[(size_t) index]->getSwitchButton();
}

juce::Button& DeviceNotices::getCloseButton (int index)
{
    return getShown()[(size_t) index]->getCloseButton();
}

void DeviceNotices::countShown (double seconds)
{
    // Those under the pointer wait; the others go once they have shown long enough.
    auto closed = false;

    for (auto it = notices.begin(); it != notices.end();)
    {
        auto& notice = **it;

        if (notice.isVisible() && ! isUnderPointer (notice))
            notice.shown += seconds;

        if (notice.shown >= NoticeTint::shownSeconds)
        {
            it = notices.erase (it);
            closed = true;
        }
        else
        {
            ++it;
        }
    }

    if (closed)
        noticesChanged();
}

void DeviceNotices::timerCallback()
{
    // Counted while Phazite is in front, as VS Code counts while its window has the focus.
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto seconds = (now - lastTick) / 1000.0;
    lastTick = now;

    if (juce::Process::isForegroundProcess())
        countShown (seconds);
}
} // namespace phazite::ui
