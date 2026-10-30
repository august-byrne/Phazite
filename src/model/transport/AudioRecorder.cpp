// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/AudioRecorder.h"
#include "model/base/internal/Schema.h"
#include "model/engine/internal/LiveInput.h"
#include "model/mixer/internal/Mixer.h"

#include <map>

namespace phazite
{
namespace
{
    // What a recording's buffer holds before its thread writes it, in seconds: room for the thread to fall far behind.
    constexpr double bufferSeconds = 8.0;

    // How often the thread writes what has come in, in milliseconds.
    constexpr int writeEveryMilliseconds = 20;

    // How long a punch's edges cross-fade with what was there, in seconds.
    constexpr double crossfadeSeconds = 0.01;

    // A recording shorter than this keeps no clip, in seconds.
    constexpr double shortestRecording = 0.01;

    std::map<const te::Edit*, AudioRecorder*>& getRecorders()
    {
        static std::map<const te::Edit*, AudioRecorder*> recorders; // message thread only
        return recorders;
    }

    juce::String getInputOf (const te::Track& track)
    {
        return track.state.getProperty (schema::phaziteInput).toString();
    }

    // When the track hears its input: schema::monitorAlways, schema::monitorNever, or while it is armed (none).
    juce::String getMonitoringOf (const te::Track& track)
    {
        return track.state.getProperty (schema::phaziteMonitoring).toString();
    }

    int getNumDeviceInputs (te::Engine& engine)
    {
        auto* device = engine.getDeviceManager().deviceManager.getCurrentAudioDevice();
        return device != nullptr ? device->getInputChannelNames().size() : 0;
    }
} // namespace

std::vector<int> getInputChannels (const juce::String& inputID)
{
    std::vector<int> channels;

    for (const auto& part : juce::StringArray::fromTokens (inputID, "+", {}))
    {
        if (part.isEmpty() || ! part.containsOnly ("0123456789") || part.getIntValue() < 1)
            return {};

        channels.push_back (part.getIntValue() - 1);
    }

    // A channel alone, or a pair from an odd-numbered one (1+2, 3+4...).
    if (channels.size() > 2 || (channels.size() == 2 && (channels[0] % 2 != 0 || channels[1] != channels[0] + 1)))
        return {};

    return channels;
}

double getRecordingCorrection (te::Engine& engine)
{
    const auto inputs = engine.getDeviceManager().getWaveInputDevices();
    return inputs.empty() ? 0.0 : inputs.front()->getRecordAdjustmentMs();
}

void setRecordingCorrection (te::Engine& engine, double milliseconds)
{
    for (auto* input : engine.getDeviceManager().getWaveInputDevices())
        input->setRecordAdjustmentMs (milliseconds);
}

//==============================================================================
// One armed track's recording: the consumer that the audio thread hands each block of the track's input, the buffer it
// copies the track's channels into, and the WAV file the recorder's thread writes them into.
struct AudioRecorder::Recording : public te::InputDeviceInstance::Consumer
{
    Recording (te::AudioTrack& t, te::InputDeviceInstance& in, te::EditPlaybackContext& context, std::vector<int> channelsInInput,
               juce::File f, double rate, juce::int64 delay)
        : track (t),
          instance (&in),
          playback (context),
          channels (std::move (channelsInInput)),
          file (std::move (f)),
          sampleRate (rate),
          shift (delay),
          devices (t.edit.engine.getDeviceManager()),
          buffer ((int) channels.size(), (int) (bufferSeconds * rate)),
          fifo ((int) (bufferSeconds * rate))
    {
    }

    // The file, made empty, to write into; false if it can't be.
    bool open()
    {
        file.getParentDirectory().createDirectory();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

        if (! static_cast<juce::FileOutputStream&> (*stream).openedOk())
            return false;

        writer = juce::WavAudioFormat().createWriterFor (
            stream,
            juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels ((int) channels.size()).withBitsPerSample (24));
        return writer != nullptr;
    }

    // Audio thread: the input's block, its track's channels copied into the buffer if there is room (lost otherwise, and
    // said so), once the song plays; and at the second, where in the song (as if nothing looped) the first frame came in.
    void acceptInputBuffer (choc::buffer::ChannelArrayView<float> block) override
    {
        if (! capturing.load (std::memory_order_acquire) || ! playback.isPlaying())
            return;

        const auto frames = (int) block.getNumFrames();

        // As Tracktion's own recording finds it, from the device's clock, which puts a block that comes in where the song
        // played the block before; but taken at the second block. In the first block of playing from a stop, Tracktion
        // syncs its playhead to where it starts only after the inputs come in, so the clock maps that block a block late:
        // later than the blocks after it, and than a recording started while the song plays.
        if (! hasStart.load (std::memory_order_relaxed) && copied > 0)
        {
            startTime =
                playback.globalStreamTimeToEditTimeUnlooped (devices.getCurrentStreamTime()).inSeconds() - (double) copied / sampleRate;
            hasStart.store (true, std::memory_order_release);
        }

        if (fifo.getFreeSpace() < frames)
        {
            overflowed.store (true, std::memory_order_relaxed);
            return;
        }

        int start1, size1, start2, size2;
        fifo.prepareToWrite (frames, start1, size1, start2, size2);

        for (size_t c = 0; c < channels.size(); ++c)
        {
            if ((uint32_t) channels[c] >= block.getNumChannels())
            {
                buffer.clear ((int) c, start1, size1);
                buffer.clear ((int) c, start2, size2);
                continue;
            }

            const auto* in = block.data.channels[channels[c]] + block.data.offset;
            buffer.copyFrom ((int) c, start1, in, size1);

            if (size2 > 0)
                buffer.copyFrom ((int) c, start2, in + size1, size2);
        }

        fifo.finishedWrite (size1 + size2);
        copied += frames;
    }

    // The recorder's thread, then the message thread at the end: what has come in, into the file.
    void write()
    {
        for (auto ready = fifo.getNumReady(); ready > 0; ready = fifo.getNumReady())
        {
            int start1, size1, start2, size2;
            fifo.prepareToRead (ready, start1, size1, start2, size2);
            writeFrames (start1, size1);
            writeFrames (start2, size2);
            fifo.finishedRead (size1 + size2);
        }
    }

    void writeFrames (int start, int count)
    {
        if (count <= 0 || writer == nullptr)
            return;

        const float* data[] = { buffer.getReadPointer (0, start), channels.size() > 1 ? buffer.getReadPointer (1, start) : nullptr };
        writer->writeFromFloatArrays (data, (int) channels.size(), count);
        written += count;
    }

    void close() { writer = nullptr; } // the file whole

    te::AudioTrack& track;
    te::InputDeviceInstance* instance; // compared only: it may be gone
    te::EditPlaybackContext& playback; // the Edit's, which lives while it records
    const std::vector<int> channels;   // the track's, in its input's
    const juce::File file;
    const double sampleRate;
    const juce::int64 shift; // how many frames late the input's sound comes in
    te::DeviceManager& devices;
    juce::AudioBuffer<float> buffer;
    juce::AbstractFifo fifo;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    juce::int64 written = 0;
    juce::int64 copied = 0; // into the buffer, by the audio thread
    std::atomic<bool> capturing { false }, overflowed { false };
    std::atomic<bool> hasStart { false };
    double startTime = 0.0; // where in the song, as if nothing looped, the first frame copied came in, once hasStart is set
};

//==============================================================================
// The thread that writes what the recordings' buffers hold into their files, a few dozen times a second.
class AudioRecorder::Writer : public juce::Thread
{
public:
    explicit Writer (std::vector<Recording*> written)
        : juce::Thread ("Phazite audio recorder"),
          recordings (std::move (written))
    {
        startThread (juce::Thread::Priority::high);
    }

    ~Writer() override { stopThread (2000); }

    void run() override
    {
        while (! threadShouldExit())
        {
            for (auto* recording : recordings)
                recording->write();

            wait (writeEveryMilliseconds);
        }
    }

private:
    const std::vector<Recording*> recordings;
};

//==============================================================================
AudioRecorder::AudioRecorder (te::Edit& e)
    : edit (e),
      engine (e.engine),
      alive (e),
      key (&e),
      editState (e.state)
{
    getRecorders()[key] = this;
    editState.addListener (this);
    engine.getDeviceManager().addChangeListener (this);
    edit.getTransport().addListener (this);
    update();
}

AudioRecorder::~AudioRecorder()
{
    finishRecordings ({}, true);

    if (alive.get() != nullptr) // otherwise its transport went with it
        edit.getTransport().removeListener (this);

    engine.getDeviceManager().removeChangeListener (this);
    editState.removeListener (this);
    armed.clear();
    update(); // what was opened for its tracks closes

    if (const auto found = getRecorders().find (key); found != getRecorders().end() && found->second == this)
        getRecorders().erase (found);
}

AudioRecorder* AudioRecorder::findFor (const te::Edit& edit)
{
    const auto found = getRecorders().find (&edit);
    return found != getRecorders().end() && found->second->alive.get() == &edit ? found->second : nullptr;
}

bool AudioRecorder::isArmed (const te::AudioTrack& track) const
{
    return armed.count (track.itemID) > 0;
}

void AudioRecorder::setArmed (te::AudioTrack& track, bool shouldArm)
{
    if (shouldArm && getInputChannels (getInputOf (track)).empty())
        return;

    if (shouldArm ? armed.insert (track.itemID).second : armed.erase (track.itemID) > 0)
        update();
}

juce::Array<te::AudioTrack*> AudioRecorder::getArmedTracks() const
{
    juce::Array<te::AudioTrack*> tracks;

    for (auto* track : te::getAudioTracks (edit))
        if (isArmed (*track))
            tracks.add (track);

    return tracks;
}

bool AudioRecorder::canRecord() const
{
    const auto numInputs = getNumDeviceInputs (edit.engine);

    for (auto* track : getArmedTracks())
        if (const auto channels = getInputChannels (getInputOf (*track)); ! channels.empty() && channels.back() < numInputs)
            return true;

    return false;
}

//==============================================================================
void AudioRecorder::update()
{
    // While recording, the inputs stay as they are: changing them remakes them, and the recordings would lose theirs.
    if (updating || isRecording())
        return;

    const juce::ScopedValueSetter<bool> svs (updating, true);
    auto& devices = engine.getDeviceManager();
    const auto numInputs = getNumDeviceInputs (engine);
    auto* liveEdit = alive.get(); // once the Edit is gone, its tracks need nothing

    // What the tracks need now, by the first channel of each input: whether it is a pair, and the tracks that hear it.
    struct Need
    {
        bool pair = false;
        juce::Array<te::EditItemID> hearers;
    };

    std::map<int, Need> needs;
    juce::BigInteger wanted;

    for (auto* track : liveEdit != nullptr ? te::getAudioTracks (*liveEdit) : juce::Array<te::AudioTrack*>())
    {
        const auto channels = getInputChannels (getInputOf (*track));

        if (channels.empty() || channels.back() >= numInputs)
            continue;

        const auto monitoring = getMonitoringOf (*track);
        const auto isOn = isArmed (*track) || monitoring == schema::monitorAlways;

        if (! isOn)
            continue;

        auto& need = needs[channels.front()];
        need.pair = need.pair || channels.size() == 2;

        for (auto channel : channels)
            wanted.setBit (channel);

        if (monitoring == schema::monitorAlways || (isArmed (*track) && monitoring.isEmpty()))
            need.hearers.add (track->itemID);
    }

    // The audio device's inputs: those the tracks need opened, and those opened for them before and no longer needed
    // closed again; the ones the user opened in the audio settings stay. Only a change opens the device again: then its
    // channels are named, or JUCE would put back its default ones (none: Phazite opens no microphone by itself) whenever
    // it opened again.
    if (auto* device = devices.deviceManager.getCurrentAudioDevice(); device != nullptr && numInputs > 0)
    {
        auto setup = devices.deviceManager.getAudioDeviceSetup();
        auto channels = setup.inputChannels;

        for (int channel = 0; channel < numInputs; ++channel)
        {
            if (wanted[channel])
                channels.setBit (channel);
            else if (openedForTracks[channel])
                channels.clearBit (channel);
        }

        if (channels != setup.inputChannels)
        {
            for (int channel = 0; channel < numInputs; ++channel)
                openedForTracks.setBit (channel, wanted[channel] && (openedForTracks[channel] || ! setup.inputChannels[channel]));

            setup.inputChannels = channels;
            setup.useDefaultInputChannels = false;
            devices.deviceManager.setAudioDeviceSetup (setup, true);
            return; // the device opens again, and Tracktion remakes its inputs: then this runs again
        }
    }

    // Tracktion's inputs: a channel alone or a pair as the tracks need it, enabled while needed, heard by the tracks that
    // hear it now, and by no others.
    auto* playback = liveEdit != nullptr ? liveEdit->getCurrentPlaybackContext() : nullptr;

    for (auto* input : devices.getWaveInputDevices())
    {
        const auto& channels = input->getChannels();

        if (channels.size() == 0)
            continue;

        const auto first = channels[0].indexInDevice;
        const auto need = needs.find (first);
        const auto isNeeded = need != needs.end();

        if (isNeeded && need->second.pair != (channels.size() == 2) && regroupings < 3)
        {
            ++regroupings;
            devices.setDeviceNumChannels (*input, need->second.pair ? 2u : 1u);
            return; // remade: then this runs again
        }

        if (input->isEnabled() != isNeeded)
            devices.setDeviceEnabled (*input, isNeeded);

        const auto hearers = isNeeded ? need->second.hearers : juce::Array<te::EditItemID>();
        const auto mode = hearers.isEmpty() ? te::InputDevice::MonitorMode::off : te::InputDevice::MonitorMode::on;

        if (input->getMonitorMode() != mode)
            input->setMonitorMode (mode);

        if (auto* instance = playback != nullptr ? playback->getInputFor (input) : nullptr)
        {
            for (auto target : instance->getTargets())
                if (! hearers.contains (target))
                    (void) instance->removeTarget (target, nullptr);

            for (auto hearer : hearers)
                if (! instance->getTargets().contains (hearer))
                    (void) instance->setTarget (hearer, false, nullptr);
        }
    }

    regroupings = 0;
}

//==============================================================================
void AudioRecorder::recordingStarted (te::SyncPoint start, std::optional<te::TimeRange> punch)
{
    prepareRecordings(); // prepared already when Phazite started recording; otherwise from now, missing what played before
    startRecordings (start, punch);
}

void AudioRecorder::recordingStopped (te::SyncPoint stopped, bool discard)
{
    finishRecordings (stopped, discard);
}

void AudioRecorder::prepareRecordings()
{
    if (isRecording())
        return;

    edit.getTransport().ensureContextAllocated(); // as recording would: the inputs that the recordings take in
    auto* playback = edit.getCurrentPlaybackContext();
    auto& devices = edit.engine.getDeviceManager();
    const auto rate = devices.getSampleRate();

    if (playback == nullptr || rate <= 0.0)
        return;

    for (auto* track : getArmedTracks())
    {
        const auto wanted = getInputChannels (getInputOf (*track));

        // The input that holds the track's channels, and where they are in it.
        for (auto* instance : playback->getAllInputs())
        {
            auto* input = dynamic_cast<te::WaveInputDevice*> (&instance->getInputDevice());

            if (input == nullptr || ! input->isEnabled())
                continue;

            std::vector<int> inInput;
            const auto& channels = input->getChannels();

            for (auto channel : wanted)
                for (size_t i = 0; i < channels.size(); ++i)
                    if (channels[i].indexInDevice == channel)
                        inInput.push_back ((int) i);

            if (inInput.size() != wanted.size())
                continue;

            // How late the input's sound comes in: the delays the audio device reports for its input and output, the
            // song's delay compensation, and the audio settings' own correction.
            const auto delay = devices.getRecordAdjustmentSamples() + playback->getLatencySamples()
                               + (juce::int64) std::llround (input->getRecordAdjustmentMs() * rate / 1000.0);

            auto recording = std::make_unique<Recording> (*track, *instance, *playback, inInput, getTakeFile (*track), rate, delay);

            if (recording->open())
            {
                recording->capturing.store (true, std::memory_order_release);
                instance->addConsumer (recording.get());
                recordings.push_back (std::move (recording));
            }

            break;
        }
    }
}

void AudioRecorder::discardRecordings()
{
    finishRecordings ({}, true);
}

void AudioRecorder::startRecordings (te::SyncPoint start, std::optional<te::TimeRange> punch)
{
    if (recordings.empty() || started.has_value())
        return;

    started = start;
    punchRange = punch;
    auto& transport = edit.getTransport();
    loopRange = transport.looping.get() && ! punch.has_value() ? std::optional (transport.getLoopRange()) : std::nullopt;

    std::vector<Recording*> written;

    for (auto& recording : recordings)
        written.push_back (recording.get());

    writer = std::make_unique<Writer> (std::move (written));
}

void AudioRecorder::removeConsumers()
{
    auto* liveEdit = alive.get();
    auto* playback = liveEdit != nullptr ? liveEdit->getCurrentPlaybackContext() : nullptr;
    const auto inputs = playback != nullptr ? playback->getAllInputs() : juce::Array<te::InputDeviceInstance*>();

    // Once removed, under the lock the audio thread holds while it hands a consumer a block, a consumer gets no more. An
    // input that is gone took its consumers with it.
    for (auto& recording : recordings)
        if (inputs.contains (recording->instance))
            recording->instance->removeConsumer (recording.get());
}

void AudioRecorder::finishRecordings (te::SyncPoint stopped, bool discard)
{
    if (! isRecording())
        return;

    for (auto& recording : recordings)
        recording->capturing.store (false, std::memory_order_release);

    removeConsumers();
    writer = nullptr; // its thread ends

    for (auto& recording : recordings)
    {
        recording->write(); // what was left
        recording->close();
    }

    for (auto& recording : recordings)
    {
        if (! discard && started.has_value() && alive.get() != nullptr
            && te::findTrackForID (edit, recording->track.itemID) == &recording->track)
            makeClips (*recording, stopped);
        else
            recording->file.deleteFile();
    }

    recordings.clear();
    started.reset();
    punchRange.reset();
    loopRange.reset();
    update(); // what was armed meanwhile
}

juce::File AudioRecorder::getTakeFile (const te::AudioTrack& track) const
{
    const auto folder = edit.engine.getEngineBehaviour().getDefaultFolderForAudioRecordings (edit);
    const auto name = juce::File::createLegalFileName (track.getName().isNotEmpty() ? track.getName() : juce::String ("Recording"));

    for (int take = 1;; ++take)
        if (const auto file = folder.getChildFile (name + " Take " + juce::String (take) + ".wav"); ! file.exists())
            return file;
}

//==============================================================================
void AudioRecorder::makeClips (Recording& recording, te::SyncPoint stopped)
{
    if (! recording.hasStart.load (std::memory_order_acquire) || recording.written <= 0)
    {
        recording.file.deleteFile();
        return;
    }

    lastShift = recording.shift;
    const auto rate = recording.sampleRate;
    const auto& start = *started;

    // Where in the song, as if nothing looped, each frame of the file was played: where it came in, less the delay the input
    // comes in with; and the frame played at a time.
    const auto timeOf = [&] (double frame) { return recording.startTime + (frame - (double) recording.shift) / rate; };
    const auto frameOf = [&] (double seconds) { return (seconds - recording.startTime) * rate + (double) recording.shift; };

    const auto from = std::max (start.unloopedTime.inSeconds(), timeOf (0.0));
    const auto to = std::min (stopped.unloopedTime.inSeconds(), timeOf ((double) recording.written));

    if (to - from < shortestRecording)
    {
        recording.file.deleteFile();
        return;
    }

    auto& track = recording.track;
    const auto name = track.getName();
    const auto place = [&] (double clipStart, double clipEnd, double fileStart) -> te::WaveAudioClip*
    {
        const te::ClipPosition position { { te::TimePosition::fromSeconds (clipStart), te::TimePosition::fromSeconds (clipEnd) },
                                          te::TimeDuration::fromSeconds (std::max (0.0, fileStart)) };
        return track.insertWaveClip (name, recording.file, position, false).get();
    };

    // Punched in: only what falls in the range, in place of what was there, cross-fading at the edges: the new clip
    // reaches half a fade past each, fading in and out over what was there, which fades out and in where it is cut.
    if (punchRange.has_value())
    {
        const auto in = std::max (from, punchRange->getStart().inSeconds());
        const auto out = std::min (to, punchRange->getEnd().inSeconds());

        if (out - in < shortestRecording)
        {
            recording.file.deleteFile();
            return;
        }

        const auto half = crossfadeSeconds / 2.0;
        const auto clipStart = std::max (from, in - half), clipEnd = std::min (to, out + half);
        track.deleteRegion (
            { te::TimePosition::fromSeconds (clipStart + crossfadeSeconds), te::TimePosition::fromSeconds (clipEnd - crossfadeSeconds) },
            nullptr);

        for (auto* clip : track.getClips())
            if (auto* audio = dynamic_cast<te::AudioClipBase*> (clip))
            {
                const auto range = audio->getPosition().time;

                if (std::abs (range.getEnd().inSeconds() - (clipStart + crossfadeSeconds)) < 1.0e-6)
                    audio->setFadeOut (te::TimeDuration::fromSeconds (crossfadeSeconds));

                if (std::abs (range.getStart().inSeconds() - (clipEnd - crossfadeSeconds)) < 1.0e-6)
                    audio->setFadeIn (te::TimeDuration::fromSeconds (crossfadeSeconds));
            }

        if (auto* clip = place (clipStart, clipEnd, frameOf (clipStart) / rate))
        {
            clip->setFadeIn (te::TimeDuration::fromSeconds (crossfadeSeconds));
            clip->setFadeOut (te::TimeDuration::fromSeconds (crossfadeSeconds));
        }

        return;
    }

    // Round a looped range: a take for each time round that began within the recording, in one clip over the range, the
    // last whole one playing (or the last one, if none was whole). Once round or less, a clip of what was played.
    if (loopRange.has_value() && loopRange->getLength().inSeconds() > 0.0)
    {
        const auto loopStart = loopRange->getStart().inSeconds(), length = loopRange->getLength().inSeconds();
        const auto firstLoopStart =
            start.unloopedTime.inSeconds() - (start.time.inSeconds() - loopStart); // where the first time round began
        const auto passOf = [&] (double seconds) { return (int) std::floor ((seconds - firstLoopStart) / length + 1.0e-9); };

        if (passOf (to - 1.0e-6) > passOf (from))
        {
            std::vector<int> takes; // the times round that began within the recording

            for (auto pass = passOf (from); pass <= passOf (to - 1.0e-6); ++pass)
                if (frameOf (firstLoopStart + pass * length) >= 0.0 && firstLoopStart + pass * length < to)
                    takes.push_back (pass);

            if (! takes.empty())
            {
                auto playing = takes.back();

                for (auto pass : takes)
                    if (firstLoopStart + (pass + 1) * length <= to + 1.0e-6)
                        playing = pass;

                const auto passStart = firstLoopStart + playing * length;

                if (auto* clip = place (loopStart, loopStart + length, frameOf (passStart) / rate))
                {
                    clip->state.setProperty (schema::phaziteTakes, (int) takes.size(), &edit.getUndoManager());
                    clip->state.setProperty (schema::phaziteTake,
                                             (int) (std::find (takes.begin(), takes.end(), playing) - takes.begin()) + 1,
                                             &edit.getUndoManager());
                    clip->state.setProperty (schema::phaziteTakeLength, length, &edit.getUndoManager());
                }

                return;
            }
        }

        // Not round once: where it was played in the range.
        const auto offsetInLoop = loopStart - firstLoopStart - passOf (from) * length;
        place (from + offsetInLoop, to + offsetInLoop, frameOf (from) / rate);
        return;
    }

    place (from, to, frameOf (from) / rate);
}

//==============================================================================
void AudioRecorder::changeListenerCallback (juce::ChangeBroadcaster*)
{
    update(); // Tracktion remade its inputs, or the audio device changed
}

void AudioRecorder::playbackContextChanged()
{
    update();
}

void AudioRecorder::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if ((property == schema::phaziteInput || property == schema::phaziteMonitoring) && tree.hasType (te::IDs::TRACK)
        && alive.get() != nullptr)
    {
        // A track left without an input can't stay armed.
        if (auto* track = dynamic_cast<te::AudioTrack*> (te::findTrackForState (edit, tree));
            track != nullptr && getInputChannels (getInputOf (*track)).empty())
            armed.erase (track->itemID);

        update();
    }
}

void AudioRecorder::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    if (child.hasType (te::IDs::TRACK))
    {
        armed.erase (te::EditItemID::fromID (child));
        update();
    }
}
} // namespace phazite
