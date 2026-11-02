// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PlaybackHost.h"
#include "internal/PlaybackSession.h"

#include <algorithm>

namespace phazite
{
PlaybackHost::PlaybackHost (te::Engine& e)
    : tracktion (e)
{
    // As many threads as Tracktion's (model/engine/internal/EngineSetup.h), which plays nothing while Phazite's does.
    engine.setNumThreads (tracktion.getEngineBehaviour().getNumberOfCPUsToUseForAudio());
}

PlaybackHost::~PlaybackHost()
{
    jassert (sessions.empty()); // each project's session goes before the app's engine
    setOn (false);
}

void PlaybackHost::setOn (bool shouldBeOn)
{
    if (on == shouldBeOn)
        return;

    on = shouldBeOn;
    auto& devices = tracktion.getDeviceManager().deviceManager;

    if (on)
    {
        // Tracktion's transport stops: what it played, Phazite's plays from now on. Its playback context stays, for what
        // only it plays yet (live notes, recording).
        if (auto* session = getPlayingSession())
        {
            auto& transport = session->getEdit().getTransport();

            if (transport.isPlaying())
                transport.stop (false, false);
        }

        devices.addAudioCallback (this); // which prepares the engine at once, if a device is open
    }
    else
    {
        // The device calls the engine no more: what it played goes at once (plug-ins among it, which their Edits own).
        devices.removeAudioCallback (this);
        engine.stop();
        engine.dropPlans();
    }

    if (auto* session = getPlayingSession())
        session->update();
}

void PlaybackHost::add (PlaybackSession& session)
{
    sessions.push_back (&session);
}

void PlaybackHost::remove (PlaybackSession& session)
{
    const auto wasPlaying = getPlayingSession() == &session;
    std::erase (sessions, &session);

    // Off, what the engine played last goes with the project (plug-ins among it, which its Edit owns).
    if (! on)
        engine.dropPlans();

    if (! wasPlaying)
        return;

    engine.stop();
    engine.setPlan (nullptr);

    if (auto* next = getPlayingSession())
        next->update();
}

PlaybackSession* PlaybackHost::getPlayingSession() const noexcept
{
    return sessions.empty() ? nullptr : sessions.back();
}

void PlaybackHost::prepare (double sampleRate, int maximumBlockSize)
{
    engine.prepare (sampleRate, maximumBlockSize);

    if (auto* session = getPlayingSession())
        session->update();
}

void PlaybackHost::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    // The device is stopped meanwhile, so the engine plays nothing. The song is given again for the new rate once the
    // message loop turns: until then a plan for another rate plays silence.
    engine.prepare (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());

    juce::MessageManager::callAsync (
        [host = juce::WeakReference<PlaybackHost> (this)]
        {
            if (host != nullptr)
                if (auto* session = host->getPlayingSession())
                    session->update();
        });
}

void PlaybackHost::audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputs, int numOutputs, int numSamples,
                                                     const juce::AudioIODeviceCallbackContext&)
{
    engine.process (outputs, numOutputs, numSamples);
}
} // namespace phazite
