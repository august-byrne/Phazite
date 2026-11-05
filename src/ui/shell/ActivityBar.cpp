// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ActivityBar.h"
#include "model/versions/VersionsModel.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
juce::String getLeftViewCommandID (LeftView view)
{
    return view == LeftView::versions ? "view.versions" : "view.browser";
}

juce::String getLeftViewName (LeftView view)
{
    return view == LeftView::versions ? "Versions" : "Browser";
}

//==============================================================================
// An icon for one of the left panel's views: a switch, on while its view shows, which screen readers say so of ("Versions,
// switch, on").
class ActivityBar::Item : public juce::ToggleButton
{
public:
    Item (CommandBridge& bridge, LeftView shownView, Icon icon)
        : juce::ToggleButton (getLeftViewName (shownView)),
          commands (bridge),
          view (shownView),
          drawable (createIcon (icon))
    {
        setTitle (getLeftViewName (view));
        setComponentID (getLeftViewCommandID (view)); // what lessons point at (Lessons::Step::target)
        setClickingTogglesState (false);              // the command decides what shows
        setToggleable (true);
        setMouseClickGrabsKeyboardFocus (false);
        onClick = [this] { commands.invoke (getLeftViewCommandID (view)); };
    }

    void setMarked (bool marked)
    {
        if (marked != getToggleState())
            setToggleState (marked, juce::dontSendNotification);
    }

    void setDot (bool shown)
    {
        if (std::exchange (dot, shown) != shown)
            repaint();
    }

    bool hasDot() const noexcept { return dot; }

    void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override
    {
        const auto& p = getPalette (*this);
        auto area = getLocalBounds();

        // The view that shows: a bar in the accent color along its left edge.
        if (getToggleState())
        {
            g.setColour (p.accent);
            g.fillRect (area.removeFromLeft (Shell::activityMarkerWidth));
        }

        // Its icon in full color while its view shows or the pointer is on it, otherwise dimmed (still readable).
        const auto colour = getToggleState() || isHighlighted || isDown ? p.text : p.textMuted;

        if (drawable != nullptr)
        {
            if (colour != drawableColour)
            {
                drawable->replaceColour (drawableColour, colour);
                drawableColour = colour;
            }

            const auto iconArea =
                getLocalBounds().toFloat().withSizeKeepingCentre ((float) Shell::activityIconSize, (float) Shell::activityIconSize);
            drawable->drawWithin (g, iconArea, juce::RectanglePlacement::centred, 1.0f);

            if (dot)
            {
                g.setColour (p.accent);
                g.fillEllipse (juce::Rectangle<float> (Shell::activityDotSize, Shell::activityDotSize)
                                   .withCentre (iconArea.getTopRight().translated (-1.0f, 1.0f)));
            }
        }
    }

    void refreshTooltip()
    {
        const auto about = view == LeftView::versions
                               ? juce::String ("Versions: save the song as it is now, see what changed since, and go back. Click again "
                                               "to hide the left panel")
                               : juce::String ("Browser: sounds, instruments, effects and presets. Click again to hide the left panel");
        setTooltip (withShortcut (commands, about, getLeftViewCommandID (view)));
    }

private:
    CommandBridge& commands;
    const LeftView view;
    std::unique_ptr<juce::Drawable> drawable;
    juce::Colour drawableColour { juce::Colours::black }; // what the drawing is in now
    bool dot = false;
};

//==============================================================================
ActivityBar::ActivityBar (CommandBridge& bridge, ProjectContext& projectContext)
    : commands (bridge),
      context (projectContext),
      browserItem (std::make_unique<Item> (bridge, LeftView::browser, Icon::library)),
      versionsItem (std::make_unique<Item> (bridge, LeftView::versions, Icon::versions))
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Activity bar");

    for (auto* item : { browserItem.get(), versionsItem.get() })
        addAndMakeVisible (item);

    refreshTooltips();
    context.addListener (this);
    updateDot();
}

ActivityBar::~ActivityBar()
{
    context.removeListener (this);
}

ActivityBar::Item& ActivityBar::getItem (LeftView view) const
{
    return view == LeftView::versions ? *versionsItem : *browserItem;
}

void ActivityBar::showLeftPanel (bool shows, LeftView view)
{
    browserItem->setMarked (shows && view == LeftView::browser);
    versionsItem->setMarked (shows && view == LeftView::versions);
}

void ActivityBar::refreshTooltips()
{
    browserItem->refreshTooltip();
    versionsItem->refreshTooltip();
}

bool ActivityBar::isMarked (LeftView view) const
{
    return getItem (view).getToggleState();
}

bool ActivityBar::showsVersionsDot() const
{
    return versionsItem->hasDot();
}

juce::Button& ActivityBar::getButton (LeftView view)
{
    return getItem (view);
}

void ActivityBar::updateDot()
{
    versionsItem->setDot (hasChangesSinceLastVersion (context));
}

void ActivityBar::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).background);
}

void ActivityBar::resized()
{
    auto area = getLocalBounds();

    for (auto* item : { browserItem.get(), versionsItem.get() })
        item->setBounds (area.removeFromTop (getWidth()));
}
} // namespace phazite::ui
