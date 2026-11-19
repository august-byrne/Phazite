// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceMenus.h"
#include "model/arrangement/SongSelection.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/project/ProjectContext.h"
#include "ui/input/CommandBridge.h"

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace phazite::ui
{
namespace
{
    using Chosen = std::function<void (const DeviceType&)>;

    const juce::String noInstrumentPlugins = "No instrument plug-ins yet: Scan for Plug-ins, in the gear's menu at the bottom left, "
                                             "finds them";
    const juce::String noEffectPlugins = "No effect plug-ins yet: Scan for Plug-ins, in the gear's menu at the bottom left, finds them";

    // The device types of one kind, after what the menu holds already: the built-in ones, then the plug-ins in a submenu for
    // each maker. Each item calls chosen with its type, unless its target has gone (the app closes menus when another project
    // opens, whose items could have the IDs chosen captures), so the menu works as well inside another as a submenu.
    juce::PopupMenu makeMenu (ProjectContext& context, bool instruments, juce::Component& target, Chosen chosen, juce::PopupMenu menu = {})
    {
        const auto types = std::make_shared<const std::vector<DeviceType>> (getDeviceTypes (context));
        const auto onChosen = std::make_shared<const Chosen> (std::move (chosen));
        const juce::Component::SafePointer<juce::Component> safeTarget (&target);
        std::map<juce::String, juce::PopupMenu> plugins;

        for (size_t i = 0; i < types->size(); ++i)
        {
            const auto& type = (*types)[i];

            if (type.isInstrument != instruments)
                continue;

            auto item = juce::PopupMenu::Item (type.name).setAction (
                [types, onChosen, safeTarget, i]
                {
                    if (safeTarget != nullptr)
                        (*onChosen) ((*types)[i]);
                });

            if (type.isBuiltIn)
                menu.addItem (std::move (item));
            else
                plugins[type.description.manufacturerName.isNotEmpty() ? type.description.manufacturerName : juce::String ("Other")]
                    .addItem (std::move (item));
        }

        menu.addSectionHeader ("Plug-ins");

        if (plugins.empty())
            menu.addItem (instruments ? noInstrumentPlugins : noEffectPlugins, false, false, [] {});

        for (auto& [maker, list] : plugins)
            menu.addSubMenu (maker, list);

        return menu;
    }

    // Dropped from the button that asks for it.
    void show (juce::PopupMenu menu, juce::Component& target)
    {
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
    }
} // namespace

juce::PopupMenu makeInstrumentMenu (ProjectContext& context, TrackID track, juce::Component& target)
{
    // An instrument container first, as the effects' menu starts with a container of effects: it layers instruments, or
    // plays each on keys of its own; the instrument the track has goes into it, for others to be added beside it. Not while
    // the instrument is one: its chains take more instruments.
    const juce::Component::SafePointer<juce::Component> safeTarget (&target);
    juce::PopupMenu start;
    start.addItem ("Container", canAddInstrumentContainer (context, track), false,
                   [&context, track, safeTarget]
                   {
                       if (safeTarget != nullptr)
                           addInstrumentContainer (context, track);
                   });
    start.addSeparator();

    return makeMenu (
        context, true, target, [&context, track] (const DeviceType& type) { chooseInstrument (context, track, type); }, std::move (start));
}

void showInstrumentMenu (ProjectContext& context, TrackID track, juce::Component& target)
{
    show (makeInstrumentMenu (context, track, target), target);
}

juce::PopupMenu makeEffectMenu (ProjectContext& context, TrackID track, juce::Component& target)
{
    const juce::Component::SafePointer<juce::Component> safeTarget (&target);

    // An empty container first, as Ableton Live's Audio Effect Rack comes first among its effects, for effects to be added
    // to; the Devices view shows it (grouping effects already there makes one too: "Group into Container").
    juce::PopupMenu start;
    start.addItem ("Container",
                   [&context, track, safeTarget]
                   {
                       if (safeTarget != nullptr)
                           addContainer (context, track);
                   });
    start.addSeparator();

    auto menu = makeMenu (
        context, false, target, [&context, track] (const DeviceType& type) { addEffect (context, track, type); }, std::move (start));

    // Effects change a sound. A track whose notes have no instrument makes none for them to change, so the menu
    // offers the instrument first, and the effects after it.
    if (const auto channel = getChannel (context, track); channel.has_value() && channel->needsInstrument)
    {
        auto effects = std::move (menu);
        menu = juce::PopupMenu();
        menu.addSectionHeader ("Notes make no sound without an instrument");
        menu.addSubMenu ("Choose Instrument", makeInstrumentMenu (context, track, target));
        menu.addSeparator();
        menu.addSubMenu ("Effects, Which Change the Sound", effects);
    }

    return menu;
}

void showEffectMenu (ProjectContext& context, TrackID track, juce::Component& target)
{
    show (makeEffectMenu (context, track, target), target);
}

juce::PopupMenu makeReplaceEffectMenu (ProjectContext& context, DeviceID effect, juce::Component& target)
{
    return makeMenu (context, false, target, [&context, effect] (const DeviceType& type) { replaceEffect (context, effect, type); });
}

void showNewReturnMenu (ProjectContext& context, TrackID sender, juce::Component& target)
{
    show (makeMenu (context, false, target, [&context, sender] (const DeviceType& type) { addReturnWith (context, sender, type); }),
          target);
}

void showChainEffectMenu (ProjectContext& context, DeviceID chain, juce::Component& target)
{
    show (makeMenu (context, false, target, [&context, chain] (const DeviceType& type) { addEffectToChain (context, chain, type); }),
          target);
}

juce::PopupMenu makeChainInstrumentMenu (ProjectContext& context, DeviceID chain, juce::Component& target)
{
    return makeMenu (context, true, target, [&context, chain] (const DeviceType& type) { chooseChainInstrument (context, chain, type); });
}

void showChainInstrumentMenu (ProjectContext& context, DeviceID chain, juce::Component& target)
{
    show (makeChainInstrumentMenu (context, chain, target), target);
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
