// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in instance in a helper, as Phazite holds it (docs/tracktion-migration.md, T3): the channel Phazite made for its
// blocks, the helper's instance on it, opened from the plug-in's description and state and closed as it goes, what it
// is (PluginInfo.h), and what Phazite asks of it besides blocks: its state, its program, a value set by hand, how it
// writes a value, another channel for another sample rate, its editor. What the plug-in changes itself comes as the helper's notices
// for it, which the helper's owner hands it by its ID (handleNotice()), and it to its listener, on the pipe's thread.
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
        virtual void editorKey (bool isDown, int keyCode, int modifiers) = 0;
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

    // Its state, as the plug-in keeps its settings, within the time; none if it didn't come.
    std::optional<juce::MemoryBlock> getState (int timeoutMilliseconds);
    bool setState (const juce::MemoryBlock&, int timeoutMilliseconds);

    // Its program (one of getInfo().programs): as it opened, then as it was set or the helper last told (any thread).
    int getProgram() const noexcept { return program.load (std::memory_order_relaxed); }
    bool setProgram (int index, int timeoutMilliseconds);

    // Its editor, a window of the helper's (getInfo().hasEditor): made, with its size and whether it can be resized, or
    // none; shown in a window of Phazite's (by its native handle) at a place in its pixels, or hidden with no window (no
    // waiting); and closed. And the keys Phazite takes over it, by their codes and modifiers (JUCE's), which the helper
    // hands back as the listener's editorKey(), unless the plug-in gets every key.
    struct EditorSize
    {
        int width = 0, height = 0;
        bool isResizable = false;
    };

    std::optional<EditorSize> openEditor (int timeoutMilliseconds);
    void placeEditor (std::int64_t window, int x, int y, int width, int height);
    void closeEditor (int timeoutMilliseconds);
    void setEditorKeys (bool getsEveryKey, const std::vector<std::pair<int, int>>& keys);

    // A value set by hand, from 0 to 1 (Phazite doesn't wait for it), and how the plug-in writes a value of a parameter.
    void setParameter (int index, float value);
    std::optional<juce::String> getParameterText (int index, float value, int timeoutMilliseconds);

    // Its listener (nullptr for none), set before it can tell anything.
    void setListener (Listener* newListener) noexcept { listener = newListener; }

    // One of the helper's notices for it, read past its instance's ID: on the pipe's thread.
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
    PluginInfo info;
    std::atomic<int> latency { 0 }, program { 0 };
    std::atomic<Listener*> listener { nullptr };
};
} // namespace phazite::sandbox
