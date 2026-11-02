// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in's editor as the helper shows it (docs/tracktion-migration.md, T3 step 5): the plug-in's own, made when
// Phazite asks, in a window of the helper's that is a child of a window of Phazite's, where Phazite places it (in that
// window's pixels, as the screen counts them), so that it shows inside Phazite's window as a plug-in in Phazite's process
// would. It is as large as the plug-in makes it, which it tells Phazite, and as Phazite makes it, if the plug-in can be
// resized. Phazite never calls into its window: everything it asks goes through the helper's message thread. The keys
// Phazite takes over a plug-in (play, stop, the computer keyboard's) it catches on their way to the editor and hands to
// Phazite, unless the plug-in gets every key: on Windows by a hook on the message thread's messages, which sees the keys
// of a plug-in's own native windows too, but for a native text field's; elsewhere as its components hear them.
// Message thread.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace phazite::host
{
class Editor final
{
public:
    // The plug-in's editor, made now, with what it tells: its size as the plug-in changes it, and a key Phazite takes,
    // down or let go. Check isOpen(): a plug-in may make none.
    Editor (juce::AudioPluginInstance&, std::function<void (int width, int height)> resized,
            std::function<void (bool isDown, const juce::KeyPress&)> key);
    ~Editor();

    bool isOpen() const noexcept;
    juce::Rectangle<int> getBounds() const;
    bool isResizable() const;

    // Shown in a window of Phazite's (its native handle), at a place in its pixels; hidden with none.
    void place (std::int64_t window, juce::Rectangle<int> area);

    // The keys Phazite takes over it, and whether the plug-in gets every key instead.
    void setKeys (bool getsEveryKey, std::vector<juce::KeyPress>);

    class Holder;

private:
    std::unique_ptr<Holder> holder;

    JUCE_DECLARE_NON_COPYABLE (Editor)
};
} // namespace phazite::host
