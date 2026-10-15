// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceMenus.h"
#include "model/mixer/Mixer.h"

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

            if (te::PluginManager::isBuiltInPlugin (type.description))
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

    // Calls chosen with the type picked, unless the menu was dismissed or its target has gone.
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

void showInstrumentMenu (te::AudioTrack& track, juce::Component& target)
{
    auto types = getDeviceTypes (track.edit.engine);
    auto menu = makeMenu (types, true, "No instrument plug-ins yet: Options > Scan for Plug-ins finds them");

    show (std::move (menu), target, std::move (types),
          [safeTrack = te::SafeSelectable<te::AudioTrack> (&track)] (const DeviceType& type)
          {
              if (auto* t = safeTrack.get())
                  chooseInstrument (*t, type);
          });
}

void showEffectMenu (te::Edit& edit, te::Track* track, juce::Component& target)
{
    auto types = getDeviceTypes (edit.engine);
    auto menu = makeMenu (types, false, "No effect plug-ins yet: Options > Scan for Plug-ins finds them");

    // Effects change a sound. A track whose notes have no instrument makes none for them to change, so the menu
    // offers the instrument first, and the effects after it.
    if (auto* audioTrack = dynamic_cast<te::AudioTrack*> (track); audioTrack != nullptr && needsInstrument (*audioTrack))
    {
        auto effects = std::move (menu);
        menu = juce::PopupMenu();
        menu.addSectionHeader ("Notes make no sound without an instrument");
        menu.addItem ("Choose Instrument...",
                      [safeTrack = te::SafeSelectable<te::AudioTrack> (audioTrack),
                       safeTarget = juce::Component::SafePointer<juce::Component> (&target)]
                      {
                          if (auto* t = safeTrack.get(); t != nullptr && safeTarget != nullptr)
                              showInstrumentMenu (*t, *safeTarget);
                      });
        menu.addSeparator();
        menu.addSubMenu ("Effects, Which Change the Sound", effects);
    }

    show (std::move (menu), target, std::move (types),
          [safeEdit = te::SafeSelectable<te::Edit> (&edit), safeTrack = te::SafeSelectable<te::Track> (track),
           forMaster = track == nullptr] (const DeviceType& type)
          {
              auto* e = safeEdit.get();
              auto* t = safeTrack.get();

              if (e == nullptr || (t == nullptr && ! forMaster)) // the track was deleted meanwhile
                  return;

              addEffect (*e, forMaster ? nullptr : t, type);
          });
}

void showNewReturnMenu (te::Track& sender, juce::Component& target)
{
    auto types = getDeviceTypes (sender.edit.engine);
    auto menu = makeMenu (types, false, "No effect plug-ins yet: Options > Scan for Plug-ins finds them");

    show (std::move (menu), target, std::move (types),
          [safeSender = te::SafeSelectable<te::Track> (&sender)] (const DeviceType& type)
          {
              if (auto* t = safeSender.get())
                  addReturnWith (*t, type);
          });
}
} // namespace phazite::ui
