// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in instance in a helper, as Phazite holds it: the channel Phazite made for its blocks, the helper's instance
// on it, opened from the plug-in's description and state and closed as it goes, what it is (PluginInfo.h), and what
// Phazite asks of it besides blocks: its state, its program, a value set by hand, how it writes a value, another
// channel for another sample rate, its editor. What the plug-in changes itself comes as the helper's notices for it,
// which the helper's owner hands it by its ID (handleNotice()), and it to its listener, on the pipe's thread.
#pragma once

#include "Helper.h"
#include "sandbox/core/BlockChannel.h"
#include "sandbox/core/PluginInfo.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace phazite::sandbox
{
class RemoteInstance final
{
public:
    struct Listener
    {
        virtual ~Listener() = default;

        virtual void parameterChanged (int index, float value) = 0;
        virtual void gestureChanged (int index, bool starting) = 0;
        virtual void latencyChanged (int samples) = 0;
        virtual void stateChanged() = 0;
        virtual void parametersChanged (const std::vector<ParameterInfo>&) = 0;
        virtual void editorResized (int width, int height) = 0;
        virtual void editorKey (bool isDown, int keyCode, int modifiers, int scanCode) = 0;
    };

    // Opened in the helper, from the plug-in's description (JUCE's XML for it) and state (empty for its own defaults), at
    // the sample rate for the channel's limits; nullptr, with why, if it wasn't.
    static std::unique_ptr<RemoteInstance> open (Helper&, const juce::String& descriptionXml, const juce::MemoryBlock& state,
                                                 double sampleRate, const BlockChannel::Limits&, juce::String& error,
                                                 int timeoutMilliseconds);
    ~RemoteInstance();

    std::int64_t getID() const noexcept { return id; }
    const PluginInfo& getInfo() const noexcept { return info; } // as it opened
    Helper& getHelper() noexcept { return helper; }

    // Its latency now: as it opened, then as the helper last told (any thread).
    int getLatencySamples() const noexcept { return latency.load (std::memory_order_relaxed); }

    // The channel its blocks go through; another after prepare(), which only the engine's preparing calls, while no block
    // plays.
    BlockChannel& getChannel() noexcept { return *channel; }
    bool prepare (double sampleRate, const BlockChannel::Limits&, int timeoutMilliseconds);

    // The same asked for without waiting (nullptr if the channel couldn't be made); then, once its reply is done (or never
    // to be: the helper went, or the asker gives up), the new channel taken in, if the helper opened it: false if it didn't,
    // and the old one stays. Only the engine's preparing calls them, while no block plays as the channel changes.
    struct Preparing
    {
        std::shared_ptr<Helper::Reply> reply;
        std::unique_ptr<BlockChannel> channel;
        double sampleRate = 0.0;
    };

    std::unique_ptr<Preparing> askToPrepare (double sampleRate, const BlockChannel::Limits&);
    bool finishPreparing (Preparing&);
    double getSampleRate() const noexcept { return channelRate; } // its channel's: as it opened, or was last prepared

    // Its state, as the plug-in keeps its settings (a VST3's as its format's preset file, which other hosts read:
    // sandbox/core/Vst3State.h), within the time; none if it didn't come. Given one, or a program (below), within the time,
    // or without waiting (askToSetState(), askToSetProgram()), the answer done once the plug-in has taken it, which holds
    // off the helper's hang while it is kept (Helper::send()): the helper takes what it is asked in turn, so that what is
    // asked after has its answer after.
    std::optional<juce::MemoryBlock> getState (int timeoutMilliseconds);
    bool setState (const juce::MemoryBlock&, int timeoutMilliseconds);
    std::shared_ptr<Helper::Reply> askToSetState (const juce::MemoryBlock&);

    // Its state asked for without waiting (Helper::send()), and read from the answer once it is done: none if none came.
    std::shared_ptr<Helper::Reply> askState();
    static std::optional<juce::MemoryBlock> readState (const Helper::Reply&);

    // Its program (one of getInfo().programs): as it opened, then as it was last set or the helper last told (any thread).
    int getProgram() const noexcept { return program.load (std::memory_order_relaxed); }
    bool setProgram (int index, int timeoutMilliseconds);
    std::shared_ptr<Helper::Reply> askToSetProgram (int index);

    // Its editor, a window of the helper's (getInfo().hasEditor): asked to be made without waiting (known bug 166), then read
    // from the answer once it is done, with its size (each side as readEditorSide() keeps it) and whether it can be resized,
    // or none; shown in a window of Phazite's (by its native handle) at a place in its pixels, or hidden with no window (no
    // waiting); and closed, without waiting, or asked to close with its answer to come once the helper has closed it (what a
    // window that holds the helper's waits for, a little at a time, before it goes). And the keys Phazite takes over it, by
    // their codes and modifiers (JUCE's), and by their places (scan codes) and modifiers, which the helper matches a key by
    // where it can tell its place, whatever its own layout; it hands them back as the listener's editorKey(), with the
    // place's scan code where it has one (0 for none), unless the plug-in gets
    // every key.
    struct EditorSize
    {
        int width = 0, height = 0;
        bool isResizable = false;
    };

    std::shared_ptr<Helper::Reply> askToOpenEditor();
    static std::optional<EditorSize> readEditor (const Helper::Reply&);
    void placeEditor (std::int64_t window, int x, int y, int width, int height);
    void closeEditor();
    std::shared_ptr<Helper::Reply> askToCloseEditor();
    void setEditorKeys (bool getsEveryKey, const std::vector<std::pair<int, int>>& keys, const std::vector<std::pair<int, int>>& places);

    // Told that a render has given it back: it plays live from now on (Phazite doesn't wait for it).
    void playLive();

    // A value set by hand, from 0 to 1 (Phazite doesn't wait for it), how the plug-in writes a value of a parameter (a text
    // of maxTextLength characters at most), and the value it reads text as ("-6 dB"), from 0 to 1 (none for what is no
    // number): each asked without waiting (known bug 166), and read from its answer once it is done (none if none came);
    // or, for the tests, within the time.
    void setParameter (int index, float value);
    std::shared_ptr<Helper::Reply> askParameterText (int index, float value);
    std::shared_ptr<Helper::Reply> askParameterValue (int index, const juce::String& text);
    static std::optional<juce::String> readParameterText (const Helper::Reply&);
    static std::optional<float> readParameterValue (const Helper::Reply&);
    std::optional<juce::String> getParameterText (int index, float value, int timeoutMilliseconds);
    std::optional<float> getParameterValue (int index, const juce::String& text, int timeoutMilliseconds);

    // Its listener (nullptr for none), set before it can tell anything.
    void setListener (Listener* newListener) noexcept { listener = newListener; }

    // One of the helper's notices for it, read past its instance's ID: on the pipe's thread. Each field checked as it comes
    // (PluginInfo.h; known bug 181): a value that is no number is no change, and a size, a key's modifiers, a latency or a
    // program is kept within what it can be.
    void handleNotice (MessageType, MessageReader&);

    // The instance a notice is for, read from it.
    static std::int64_t readNoticeInstance (MessageReader& notice) { return notice.readInt64(); }

    RemoteInstance (const RemoteInstance&) = delete;
    RemoteInstance& operator= (const RemoteInstance&) = delete;

private:
    RemoteInstance (Helper&, std::int64_t id, std::unique_ptr<BlockChannel>);

    Helper& helper;
    const std::int64_t id;
    std::unique_ptr<BlockChannel> channel;
    double channelRate = 0.0;
    PluginInfo info;
    std::atomic<int> latency { 0 }, program { 0 };
    std::atomic<Listener*> listener { nullptr };
};
} // namespace phazite::sandbox
