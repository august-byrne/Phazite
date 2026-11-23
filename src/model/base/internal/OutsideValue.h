// SPDX-License-Identifier: AGPL-3.0-or-later
// A value of the song's that is never in its undo history (roadmap step S5.20): view state written outside it alone, as a
// device's window place (DevicesOutside.h), which no step holds, no Undo puts back and no step may write. Unlike an area
// (ValueArea.h), it knows no undo manager at all, so that nothing can put it in a step: set() moves it to its next value and
// tells its listeners, whatever step is open. A value type as an area's: copyable, its copies cheap, and the same as another
// without a look inside where it says so (a member isSameAs()), else by ==; setting the same value is nothing.
//
// Not held by the song yet: the store's switch gives one to SongValues (model/base/internal/SongValues.h). Message thread
// only, as the song's tree is.
#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <concepts>
#include <utility>

namespace phazite
{
template <typename Value>
class OutsideValue
{
public:
    explicit OutsideValue (Value start = {})
        : now (std::move (start))
    {
    }

    ~OutsideValue()
    {
        JUCE_ASSERT_MESSAGE_THREAD
        listeners.clear();
    }

    const Value& get() const noexcept { return now; }

    // Its next value, a move outside any step, its listeners told with the value left and the value come to; nothing for
    // the same value.
    void set (Value next)
    {
        JUCE_ASSERT_MESSAGE_THREAD

        if (isSame (next, now))
            return;

        const auto before = std::exchange (now, std::move (next));
        listeners.call ([&] (Listener& listener) { listener.outsideValueChanged (before, now); });
    }

    struct Listener
    {
        virtual ~Listener() = default;

        // The value went from one to another. Mark what changed, and read nothing else of the song (as an area's listener).
        virtual void outsideValueChanged (const Value& before, const Value& after) = 0;
    };

    // A listener may remove itself, or another, while it is told.
    void addListener (Listener* listener)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        listeners.add (listener);
    }

    void removeListener (Listener* listener)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        listeners.remove (listener);
    }

private:
    static bool isSame (const Value& a, const Value& b)
    {
        if constexpr (requires {
                          { a.isSameAs (b) } -> std::convertible_to<bool>;
                      })
            return a.isSameAs (b);
        else
            return a == b;
    }

    Value now;
    juce::ListenerList<Listener> listeners;

    JUCE_DECLARE_NON_COPYABLE (OutsideValue)
    JUCE_DECLARE_NON_MOVEABLE (OutsideValue)
};
} // namespace phazite
