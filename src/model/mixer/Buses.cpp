// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Buses.h"
#include "model/base/internal/Schema.h"

namespace phazite
{
bool isBus (const te::Track& track)
{
    auto* folder = dynamic_cast<const te::FolderTrack*> (&track);
    return folder != nullptr && folder->isSubmixFolder();
}

void makeBus (te::FolderTrack& folder)
{
    if (isBus (folder))
        return;

    // The bus's fader starts where the folder's VCA was. Its volume and meter go straight into the folder's
    // state: Tracktion's insertPlugin() warns that the folder becomes a bus, which is what this is for.
    auto& edit = folder.edit;
    auto* undoManager = &edit.getUndoManager();
    auto* vca = folder.getVCAPlugin();
    const auto volumeDb = vca != nullptr ? vca->getVolumeDb() : 0.0f;

    auto volume = edit.getPluginCache().createNewPlugin (te::VolumeAndPanPlugin::create());
    auto meter = edit.getPluginCache().createNewPlugin (te::LevelMeterPlugin::create());
    folder.state.addChild (volume->state, -1, undoManager);
    folder.state.addChild (meter->state, -1, undoManager);

    if (vca != nullptr)
        vca->deleteFromParent();

    if (auto* fader = dynamic_cast<te::VolumeAndPanPlugin*> (volume.get()))
        fader->setVolumeDb (volumeDb);
}

void makePlainFolder (te::FolderTrack& folder)
{
    if (! isBus (folder))
        return;

    auto* volume = folder.getVolumePlugin();
    const auto volumeDb = volume != nullptr ? volume->getVolumeDb() : 0.0f;

    for (auto* plugin : folder.pluginList.getPlugins()) // a copy that holds them while they are deleted
        plugin->deleteFromParent();

    folder.pluginList.insertPlugin (te::VCAPlugin::create(), -1);

    if (auto* vca = folder.getVCAPlugin())
        vca->setVolumeDb (volumeDb);
}

void mixAsBus (te::FolderTrack& folder)
{
    makeBus (folder);
    folder.state.setProperty (schema::phaziteBus, true, &folder.edit.getUndoManager());
}
} // namespace phazite
