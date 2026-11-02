// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PlaybackHost.h"
#include "internal/PlaybackSession.h"

#include <algorithm>

namespace phazite
{
PlaybackHost::PlaybackHost (te::Engine& e)
    : tracktion (e),
      taking (512)
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
        startTimerHz (4);
    }
    else
    {
        stopTimer();
        getPlayedEvents(); // what is left in the engine's queue too
        played.clear();
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

void PlaybackHost::prepare (double sampleRate, int maximumBlockSize, int deviceInputs, int deviceInputLatency)
{
    engine.prepare (sampleRate, maximumBlockSize);
    numInputs = deviceInputs;
    inputLatency = deviceInputLatency;
    outputLatency = 0;

    if (auto* session = getPlayingSession())
        session->update();
}

void PlaybackHost::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    // The device is stopped meanwhile, so the engine plays nothing. The song is given again for the new rate once the
    // message loop turns: until then a plan for another rate plays silence.
    engine.prepare (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());

    // Its inputs: the channel of each open one, in the order it hands them.
    const auto open = device->getActiveInputChannels();
    openInputs.clear();

    for (int channel = 0; channel <= open.getHighestBit(); ++channel)
        if (open[channel])
            openInputs.push_back (channel);

    numInputs = device->getInputChannelNames().size();
    inputLatency = device->getInputLatencyInSamples();
    outputLatency = device->getOutputLatencyInSamples();

    juce::MessageManager::callAsync (
        [host = juce::WeakReference<PlaybackHost> (this)]
        {
            if (host != nullptr)
                if (auto* session = host->getPlayingSession())
                    session->update();
        });
}

void PlaybackHost::audioDeviceIOCallbackWithContext (const float* const* open, int numOpen, float* const* outputs, int numOutputs,
                                                     int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    // The inputs by their channels: the device hands the open ones alone.
    inputs.fill (nullptr);
    auto count = 0;

    for (int i = 0; i < numOpen && i < (int) openInputs.size(); ++i)
    {
        if (const auto channel = openInputs[(size_t) i]; channel < (int) inputs.size())
        {
            inputs[(size_t) channel] = open[i];
            count = std::max (count, channel + 1);
        }
    }

    engine.process (inputs.data(), count, outputs, numOutputs, numSamples);
}

const std::deque<engine::Engine::PlayedEvent>& PlaybackHost::getPlayedEvents()
{
    for (auto taken = engine.takePlayed (taking); taken > 0; taken = engine.takePlayed (taking))
        played.insert (played.end(), taking.begin(), taking.begin() + taken);

    // The last 30 seconds by the engine's clock.
    auto oldest = engine.getClock() - (std::int64_t) (30.0 * engine.getSampleRate());

    if (keepFrom.has_value())
        oldest = std::min (oldest, *keepFrom);

    while (! played.empty() && played.front().clock < oldest)
        played.pop_front();

    return played;
}
} // namespace phazite
