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
    // As many threads as Tracktion's (model/engine/internal/EngineSetup.h), which plays nothing of a project.
    engine.setNumThreads (tracktion.getEngineBehaviour().getNumberOfCPUsToUseForAudio());
    tracktion.getDeviceManager().deviceManager.addAudioCallback (this); // which prepares the engine at once, if a device is open
    startTimerHz (4);
}

PlaybackHost::~PlaybackHost()
{
    jassert (sessions.empty()); // each project's session goes before the app's engine
    stopTimer();
    tracktion.getDeviceManager().deviceManager.removeAudioCallback (this);
    engine.stop();
    engine.dropPlans();
}

void PlaybackHost::add (PlaybackSession& session)
{
    forgetPlayed();
    sessions.push_back (&session);
}

void PlaybackHost::remove (PlaybackSession& session)
{
    const auto wasPlaying = getPlayingSession() == &session;
    std::erase (sessions, &session);

    if (wasPlaying)
    {
        engine.stop();
        forgetPlayed();
    }

    letGoOfPlans();

    if (auto* next = getPlayingSession())
        next->update();
}

void PlaybackHost::letGoOfPlans()
{
    // While a device plays the engine, the audio thread may be in a block of a plan it was handed: once it has finished
    // two blocks since it was handed none, it plays none of them. It lets go of them on the message thread.
    engine.setPlan (nullptr);

    if (running)
    {
        const auto from = engine.getBlocksPlayed().load();
        const auto until = juce::Time::getMillisecondCounter() + 500;

        while (running && engine.getBlocksPlayed().load() < from + 2 && juce::Time::getMillisecondCounter() < until)
            juce::Thread::sleep (1);
    }

    engine.dropPlans();
}

void PlaybackHost::forgetPlayed()
{
    getPlayedEvents(); // what is left in the engine's queue too
    played.clear();
}

PlaybackSession* PlaybackHost::getPlayingSession() const noexcept
{
    return sessions.empty() ? nullptr : sessions.back();
}

void PlaybackHost::prepare (double sampleRate, int maximumBlockSize, int deviceInputs, int deviceInputLatency)
{
    running = false; // the tests call the engine from the message thread
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
    running = true;

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
