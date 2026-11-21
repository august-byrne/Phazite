// SPDX-License-Identifier: AGPL-3.0-or-later
// An area of a song held as an immutable value (the immer library's containers, or a plain value that holds them), which
// changes through the song's own undo history (Song::getUndoManager(), a Song::History) while the rest of the song is still
// its juce::ValueTree: how the song moves to values one area at a time (roadmap step S5.20; docs/design/engine.md).
//
// Each change of the area is a part of the history's step in progress, as a change of the tree's with the undo manager is.
// The step's first change of the area performs one action that holds the value before and the value after; the step's later
// changes only give that action another value after. So a step, a gesture and a trial (model/commands/internal/Undo.h) hold
// the tree's changes and the area's alike, unchanged, and Undo and Redo put the area's value back in one move, however often
// the step changed it and wherever the tree's changes fell between its own: a drag keeps two values, not one for each move
// of the mouse. JUCE's own coalescing (UndoableAction::createCoalescedAction()) couldn't do it: it joins an action only with
// the one performed right before it, which in a step that changes the tree too is often the tree's. "The same step" is the
// history's current transaction while the area's last action is still among its actions
// (UndoManager::getActionsInCurrentTransaction(): none once another step began, nor after an undo or a redo); an action tells
// the area as it is destroyed (a step the history lets go, the history cleared), so that the pointer compared is never
// another action's. The actions share the area's state through a std::shared_ptr, so a history that outlives the area
// (it should not, but a song is taken apart in some order) undoes and redoes into nothing, telling no one.
//
// A value type is copyable, its copies cheap and sharing what they hold (immer's containers are), and it says whether two
// values are the same without a look inside: a free function isSameValue (a, b) in the type's own namespace (found by
// argument-dependent lookup), else a member a.isSameAs (b), else a.identity() == b.identity() (immer's containers: whether
// they share their root), else a == b, for a small plain value. Two values that are the same mean no change: set() with
// one performs nothing, and a step with nothing else is no step.
//
// Listeners are told of every change, a step's, an undo's and a redo's alike, with the value left and the value come to:
// what the song's index, what plays the song and its views compare to know what changed. A listener only marks what
// changed, and reads nothing else of the song while it is told: the spikes found that during an undo the rest of the step
// (the tree's changes, the other areas') may not be back yet when an area's turn comes. Nor does it change the area.
//
// An undo step's size: the song's history keeps its steps by their count (Song::History, which keeps the units at 1), so an
// action's size in units is JUCE's default. Message thread only, as the song's tree is: nothing here is on the audio thread,
// which gets what plays from what the model compiles.
#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <concepts>
#include <memory>
#include <utility>

namespace phazite
{
namespace detail
{
    void isSameValue() = delete; // so that only a value type's own isSameValue, found by its namespace, is called

    // Whether two values of an area are the same, as the value type says (above).
    template <typename Value>
    bool isSameAreaValue (const Value& a, const Value& b)
    {
        if constexpr (requires {
                          { isSameValue (a, b) } -> std::convertible_to<bool>;
                      })
            return isSameValue (a, b);
        else if constexpr (requires {
                               { a.isSameAs (b) } -> std::convertible_to<bool>;
                           })
            return a.isSameAs (b);
        else if constexpr (requires {
                               { a.identity() == b.identity() } -> std::convertible_to<bool>;
                           })
            return a.identity() == b.identity();
        else
            return a == b;
    }
} // namespace detail

template <typename Value>
class ValueArea
{
public:
    // The song's undo history, which must outlive the area, and the area's value as it starts.
    ValueArea (juce::UndoManager& history, Value start)
        : undoManager (history),
          shared (std::make_shared<Shared> (std::move (start)))
    {
    }

    ~ValueArea()
    {
        JUCE_ASSERT_MESSAGE_THREAD
        shared->listeners.clear(); // the history's actions may outlive the area: they tell no one
    }

    const Value& get() const noexcept { return shared->now; }

    // The area's next value, as a part of the history's step in progress: nothing if it is the same value as now.
    void set (Value next)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        jassert (! undoManager.isPerformingUndoRedo()); // nothing changes the area while an undo or a redo is told

        if (detail::isSameAreaValue (next, shared->now))
            return;

        if (auto* open = findOpenAction())
        {
            open->after = next;
            shared->moveTo (std::move (next));
            return;
        }

        auto* action = new Replacement (shared, shared->now, std::move (next));
        shared->open = action;
        undoManager.perform (action); // which deletes it if it can't perform it, and ~Replacement() forgets it
    }

    // The area's value outside the history: for a song being read from its file or made.
    void setOutsideHistory (Value next)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        shared->moveTo (std::move (next));
    }

    struct Listener
    {
        virtual ~Listener() = default;

        // The area went from one value to another: by a step, an undo, a redo or setOutsideHistory(). Mark what changed and
        // read nothing else of the song (above).
        virtual void valueAreaChanged (const Value& before, const Value& after) = 0;
    };

    // A listener may remove itself, or another, while it is told.
    void addListener (Listener* listener)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        shared->listeners.add (listener);
    }

    void removeListener (Listener* listener)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        shared->listeners.remove (listener);
    }

    // The actions the history holds for the area: one for each step that changed it and is still kept (for the tests).
    int countActions() const noexcept { return shared->actions; }

private:
    struct Replacement;

    struct Shared
    {
        explicit Shared (Value start)
            : now (std::move (start))
        {
        }

        Value now;
        Replacement* open = nullptr; // the action performed last, while its step may still be in progress
        juce::ListenerList<Listener> listeners;
        int actions = 0;

        void moveTo (Value next)
        {
            JUCE_ASSERT_MESSAGE_THREAD
            const auto before = std::exchange (now, std::move (next));
            listeners.call ([&] (Listener& listener) { listener.valueAreaChanged (before, now); });
        }
    };

    // An action that puts the area's value after in the place of its value before, and back.
    struct Replacement final : juce::UndoableAction
    {
        Replacement (std::shared_ptr<Shared> state, Value valueBefore, Value valueAfter)
            : area (std::move (state)),
              before (std::move (valueBefore)),
              after (std::move (valueAfter))
        {
            ++area->actions;
        }

        ~Replacement() override
        {
            if (area->open == this)
                area->open = nullptr;

            --area->actions;
        }

        bool perform() override
        {
            area->moveTo (after);
            return true;
        }

        bool undo() override
        {
            area->moveTo (before);
            return true;
        }

        std::shared_ptr<Shared> area;
        Value before, after;
    };

    // The area's action in the step in progress, if it has one: the action performed last, while the history still lists it
    // among the actions of its current transaction.
    Replacement* findOpenAction() const
    {
        if (shared->open == nullptr)
            return nullptr;

        juce::Array<const juce::UndoableAction*> current;
        undoManager.getActionsInCurrentTransaction (current);

        if (! current.contains (shared->open))
            shared->open = nullptr;

        return shared->open;
    }

    juce::UndoManager& undoManager;
    std::shared_ptr<Shared> shared;

    JUCE_DECLARE_NON_COPYABLE (ValueArea)
    JUCE_DECLARE_NON_MOVEABLE (ValueArea)
};
} // namespace phazite
