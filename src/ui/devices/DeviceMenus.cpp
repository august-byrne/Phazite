// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceMenus.h"
#include "model/arrangement/SongSelection.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/project/ProjectContext.h"
#include "ui/input/CommandBridge.h"

#include <map>

namespace phazite::ui
{
namespace
{
    // The device types of one kind: the built-in ones, then the plug-ins in a submenu for each maker. An item's ID
    // is its type's index in types, plus one.
    juce::PopupMenu makeMenu (const std::vector<DeviceType>& types, bool instruments, const juce::String& noPluginsText)
    {
        juce::PopupMenu menu;
        std::map<juce::String, juce::PopupMenu> plugins;

        for (size_t i = 0; i < types.size(); ++i)
        {
            const auto& type = types[i];

            if (type.isInstrument != instruments)
                continue;

            if (type.isBuiltIn)
                menu.addItem ((int) i + 1, type.name);
            else
                plugins[type.description.manufacturerName.isNotEmpty() ? type.description.manufacturerName : juce::String ("Other")]
                    .addItem ((int) i + 1, type.name);
        }

        menu.addSectionHeader ("Plug-ins");

        if (plugins.empty())
            menu.addItem (noPluginsText, false, false, [] {});

        for (auto& [maker, list] : plugins)
            menu.addSubMenu (maker, list);

        return menu;
    }

    // Calls chosen with the type picked, unless the menu was dismissed or its target has gone. (The app closes menus
    // when another project opens, whose items could have the IDs chosen captures.)
    void show (juce::PopupMenu menu, juce::Component& target, std::vector<DeviceType> types, std::function<void (const DeviceType&)> chosen)
    {
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target),
                            [safeTarget = juce::Component::SafePointer<juce::Component> (&target), choices = std::move (types),
                             onChosen = std::move (chosen)] (int result)
                            {
                                if (safeTarget != nullptr && result > 0 && result <= (int) choices.size()) // 0: dismissed
                                    onChosen (choices[(size_t) result - 1]);
                            });
    }
} // namespace

void showInstrumentMenu (ProjectContext& context, TrackID track, juce::Component& target)
{
    auto types = getDeviceTypes (context);
    auto menu = makeMenu (types, true, "No instrument plug-ins yet: Options > Scan for Plug-ins finds them");

    show (std::move (menu), target, std::move (types),
          [&context, track] (const DeviceType& type) { chooseInstrument (context, track, type); });
}

void showEffectMenu (ProjectContext& context, TrackID track, juce::Component& target)
{
    auto types = getDeviceTypes (context);
    auto menu = makeMenu (types, false, "No effect plug-ins yet: Options > Scan for Plug-ins finds them");

    // Effects change a sound. A track whose notes have no instrument makes none for them to change, so the menu
    // offers the instrument first, and the effects after it.
    if (const auto channel = getChannel (context, track); channel.has_value() && channel->needsInstrument)
    {
        auto effects = std::move (menu);
        menu = juce::PopupMenu();
        menu.addSectionHeader ("Notes make no sound without an instrument");
        menu.addItem ("Choose Instrument...",
                      [&context, track, safeTarget = juce::Component::SafePointer<juce::Component> (&target)]
                      {
                          if (safeTarget != nullptr)
                              showInstrumentMenu (context, track, *safeTarget);
                      });
        menu.addSeparator();
        menu.addSubMenu ("Effects, Which Change the Sound", effects);
    }

    show (std::move (menu), target, std::move (types), [&context, track] (const DeviceType& type) { addEffect (context, track, type); });
}

void showNewReturnMenu (ProjectContext& context, TrackID sender, juce::Component& target)
{
    auto types = getDeviceTypes (context);
    auto menu = makeMenu (types, false, "No effect plug-ins yet: Options > Scan for Plug-ins finds them");

    show (std::move (menu), target, std::move (types),
          [&context, sender] (const DeviceType& type) { addReturnWith (context, sender, type); });
}

void showContainerEffectMenu (ProjectContext& context, DeviceID container, juce::Component& target)
{
    auto types = getDeviceTypes (context);
    auto menu = makeMenu (types, false, "No effect plug-ins yet: Options > Scan for Plug-ins finds them");

    show (std::move (menu), target, std::move (types),
          [&context, container] (const DeviceType& type) { addEffectToContainer (context, container, type); });
}

void showDevice (ProjectContext& context, CommandBridge& commands, const DeviceSlot& device)
{
    context.getSongSelection().selectOnly (device.id);

    // A plug-in's window, or the window one of Phazite's own devices is in, comes to the front.
    if (device.opensWindow || isDeviceWindowOpen (context, device.id))
        showDeviceWindow (context, device.id);

    commands.invoke ("device.show");
}
} // namespace phazite::ui
