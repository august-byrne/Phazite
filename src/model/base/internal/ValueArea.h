// SPDX-License-Identifier: AGPL-3.0-or-later
// An area of a song held as an immutable value (the immer library's containers, or a plain value that holds them), which
// changes through the song's own undo history (Song::getUndoManager(), a Song::History): the song is an area for each part
// of it (SongValues.h; docs/design/engine.md).
//
// Each change of the area is a part of the history's step in progress. The step's first change of the area performs one
// action that holds the value before and the value after; the step's later changes only give that action another value
// after. So a step, a gesture and a trial (model/commands/internal/Undo.h) hold each area's changes alike, and Undo and Redo
// put the area's value back in one move, however often the step changed it and wherever the other areas' changes fell
// between its own: a drag keeps two values, not one for each move of the mouse. JUCE's own coalescing
// (UndoableAction::createCoalescedAction()) couldn't do it: it joins an action only with the one performed right before it,
// which in a step that changes several areas is often another area's. "The same step" is the
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
// A value type may be keyed (KeyedAreaValue), and says so itself, by a member static constexpr bool undoesByEntry = true;
// a type without it is plain, whatever functions its namespace has. A keyed type is made of entries by their keys (immer
// maps: MapChanges.h has the helpers) and gives, found by its namespace as isSameValue is and declared before its ValueArea
// is first used (the area asserts it), findChanges (before, after), the entries that differ between two values (each by
// its key, with the entry before and after, either none: added or removed), and withChanges (now, changes, direction), the
// value now with exactly those entries as they were before (ChangeDirection::back) or after (forward). Its Undo and Redo
// then put back only the entries their step changed, an entry at a time: where the area is still at
// the value the step left (or, for a redo, the value it found), the move is to the step's very other value, identity and
// all, as for a plain value; where something was written outside the history meanwhile, it is to the value now with the
// step's entries moved. So a write outside the history to entries a step did not change survives that step's Undo and
// Redo; one to an entry a step changed is lost by that step's Undo, which puts back the entry's value before the step (and
// its Redo puts back the step's value after).
//
// A write outside the history while the area's action is in the history's current step (a drag or a trial in progress, or
// the step that ended last, until another begins or an undo or a redo) is taken into that action's values before and after,
// entry by entry, except where the step changed the entry already, which keeps the step's before and after: so the step's
// later changes, made from the value now, don't take the write along as their own. Such a later change gives the action's
// value after only what that change changed: where the value after is still the area's value now (nothing written outside
// to an entry the step changed: the common case), the next value itself, identity and all; else the value after with the
// entries that differ between the value now and the next moved forward. So a step's Undo and Redo give the same values
// whether its later changes came before or after a write outside it to an entry it changed. A write outside the history
// while the history undoes or redoes (an action of the same step writing the area as it is undone or redone) is only a
// write: no action's values take it in, and the moves of the step's actions meet it as they meet any write.
//
// What it costs: a step's changes (findChanges (before, after)) are worked out once, when first needed (its first Undo or
// Redo that meets a write, or its first write outside it), and kept until a later change in the step changes them; a write
// outside the history that is taken in works out what it changed (findChanges (now, next)) and moves the action's two values
// by the kept changes, so a plug-in that writes continuously pays for what each write changed, not for the step's size
// again: the write leaves the step's entries and their before and after as they were, so the kept changes stay true.
//
// Listeners are told of every change, a step's, an undo's and a redo's alike, with the value left and the value come to:
// what plays the song and its views compare to know what changed. A listener only marks what
// changed, and reads nothing else of the song while it is told: during an undo the rest of the step (the other areas') may
// not be back yet when an area's turn comes. Nor does it change the area.
//
// An undo step's size: the song's history keeps its steps by their count (Song::History, which keeps the units at 1), so an
// action's size in units is JUCE's default. Message thread only, as the song is: nothing here is on the audio thread,
// which gets what plays from what the model compiles.
#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <concepts>
#include <memory>
#include <optional>
#include <utility>

namespace phazite
{
// Which way a keyed value's changes are moved (above): each entry to its state before the step, or after it.
enum class ChangeDirection
{
    back,
    forward
};

namespace detail
{
    // So that only a value type's own functions, found by its namespace, are called.
    void isSameValue() = delete;
    void findChanges() = delete;
    void withChanges() = delete;

    // Whether a value type says it is keyed (above).
    template <typename Value>
    concept IsMarkedKeyed = requires { requires bool (Value::undoesByEntry); };

    // Whether a value type gives the keyed functions, found by its namespace.
    template <typename Value>
    concept GivesKeyedChanges = requires (const Value& value) {
        findChanges (value, value);
        { withChanges (value, findChanges (value, value), ChangeDirection::back) } -> std::same_as<Value>;
    };

    // What differs between two keyed values, as the value type says: its own type of changes.
    template <typename Value>
    auto findAreaChanges (const Value& before, const Value& after)
    {
        return findChanges (before, after);
    }

    template <typename Value, typename Changes>
    Value withAreaChanges (const Value& now, const Changes& changes, ChangeDirection direction)
    {
        return withChanges (now, changes, direction);
    }

    template <typename Value>
    using AreaChanges = decltype (findAreaChanges (std::declval<const Value&>(), std::declval<const Value&>()));
} // namespace detail

// A value made of entries by their keys, marked so (undoesByEntry), whose Undo and Redo move only the entries their step
// changed (above).
template <typename Value>
concept KeyedAreaValue = detail::IsMarkedKeyed<Value>;

namespace detail
{

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

    // What an action keeps beside its two values: nothing for a plain value.
    template <typename Value>
    struct KeyedState
    {
    };

    // For a keyed value: the changes between its two values, worked out when first needed, and whether it was undone last (so
    // that its next perform() is a redo).
    template <typename Value>
        requires KeyedAreaValue<Value>
    struct KeyedState<Value>
    {
        std::optional<AreaChanges<Value>> changes {};
        bool undone = false;
    };
} // namespace detail

template <typename Value>
class ValueArea
{
    static_assert (! KeyedAreaValue<Value> || detail::GivesKeyedChanges<Value>,
                   "A value type marked undoesByEntry gives findChanges (before, after) and withChanges (now, changes, "
                   "direction) in its own namespace, declared before its ValueArea is first used (ValueArea.h)");

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
            open->changeAfter (shared->now, next);
            shared->moveTo (std::move (next));
            return;
        }

        auto* action = new Replacement (shared, shared->now, std::move (next));
        shared->open = action;
        undoManager.perform (action); // which deletes it if it can't perform it, and ~Replacement() forgets it
    }

    // The area's value outside the history: for a song being read from its file or made. For a plain value, a value set outside
    // the history while the history holds steps of the area is taken back whole by an Undo of any of them (each puts back the
    // whole value it held), so what writes outside the history does so only where the history starts again or is held. So
    // what is on purpose no undo step (view state, mute and solo) lives in a value of its own, never in an area's undoable value.
    // For a keyed value (above), a write outside the history to entries a step did not change survives that step's Undo and
    // Redo, and one to an entry a step changed is lost by that step's Undo; written while the area's
    // action is in the history's current step, it is taken into that action's values, but for the entries the step changed;
    // written while the history undoes or redoes (an action of the step writing the area as it is undone or redone), it is
    // only written, taken into no action's values.
    void setOutsideHistory (Value next)
    {
        JUCE_ASSERT_MESSAGE_THREAD

        if constexpr (KeyedAreaValue<Value>)
            if (! undoManager.isPerformingUndoRedo() && ! detail::isSameAreaValue (next, shared->now))
                if (auto* open = findOpenAction())
                    open->takeInOutsideWrite (shared->now, next);

        shared->moveTo (std::move (next));
    }

    // The area's next value where it is alike to the value now but not the same one (the same values held in another value,
    // whose identity alone matters: model/base/internal/SongValues.h, an alias group's one value), which is no change of the
    // song's and so makes no step of its own: a part of the area's action in the step in progress where it has one, so that a
    // redo of that step holds it; else outside the history. There, for a plain value, an Undo of an earlier step puts back
    // the whole value it held, so the alike value's identity is lost, nothing of the song's. For a keyed value (above), the
    // area is then no longer at the value an earlier step left, so that step's Undo (and Redo) writes the step's entries onto
    // the alike value: every other entry stays as the alike value holds it, sharing what it shares, and the value come to is
    // alike to the step's value before (or after), not that very value; its identity alone differs, nothing of the song's.
    void setWithoutStep (Value next)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        jassert (! undoManager.isPerformingUndoRedo()); // nothing changes the area while an undo or a redo is told

        if (detail::isSameAreaValue (next, shared->now))
            return;

        if (auto* open = findOpenAction())
            open->changeAfter (shared->now, next);

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

    // An action that puts the area's value after in the place of its value before, and back. For a keyed value, an undo and a
    // redo move only the step's entries where something was written outside the history since (above).
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
            if constexpr (KeyedAreaValue<Value>)
            {
                if (std::exchange (keyed.undone, false))
                {
                    moveByChanges (before, after, ChangeDirection::forward);
                    return true;
                }
            }

            area->moveTo (after);
            return true;
        }

        bool undo() override
        {
            if constexpr (KeyedAreaValue<Value>)
            {
                keyed.undone = true;
                moveByChanges (after, before, ChangeDirection::back);
            }
            else
            {
                area->moveTo (before);
            }

            return true;
        }

        // The step's value after, by a later change in the step from the area's value now to the next. For a keyed value, where a
        // write outside the history to an entry the step changed left the value after other than the value now, it takes only
        // what this change changed (above); and the changes kept are no longer the step's.
        void changeAfter (const Value& now, const Value& next)
        {
            if constexpr (KeyedAreaValue<Value>)
            {
                keyed.changes.reset();

                if (! detail::isSameAreaValue (after, now))
                {
                    after = detail::withAreaChanges (after, detail::findAreaChanges (now, next), ChangeDirection::forward);
                    return;
                }
            }

            after = next;
        }

        // A write outside the history while the step is open, from the value now to the next: taken into both values but for the
        // entries the step changed already, which keep the step's before and after (so its Undo and Redo put those back). Where
        // the write changed none of the step's entries, the value after is the next itself, as the area's
        // value is, so that an undo with nothing written since is the plain move. The step's entries, and each one's before and
        // after, stay as they were, so the changes kept stay the step's (above).
        void takeInOutsideWrite (const Value& now, const Value& next)
            requires KeyedAreaValue<Value>
        {
            const auto written = detail::findAreaChanges (now, next);
            const auto& changedByStep = getChanges();
            const auto withWrite = detail::withAreaChanges (before, written, ChangeDirection::forward);
            before = detail::withAreaChanges (withWrite, changedByStep, ChangeDirection::back);
            after = detail::withAreaChanges (next, changedByStep, ChangeDirection::forward);
        }

        // The step's changes, worked out the first time they are needed and kept (its type deduced: a plain value has none).
        const auto& getChanges()
            requires KeyedAreaValue<Value>
        {
            if (! keyed.changes.has_value())
                keyed.changes = detail::findAreaChanges (before, after);

            return *keyed.changes;
        }

        // From the value the move leaves (from) to the one it comes to (to): that very value where the area is still at from;
        // else the value now with the step's entries moved, nothing where that is the value now.
        void moveByChanges (const Value& from, const Value& to, ChangeDirection direction)
            requires KeyedAreaValue<Value>
        {
            if (detail::isSameAreaValue (area->now, from))
            {
                area->moveTo (to);
                return;
            }

            auto next = detail::withAreaChanges (area->now, getChanges(), direction);

            if (! detail::isSameAreaValue (next, area->now))
                area->moveTo (std::move (next));
        }

        std::shared_ptr<Shared> area;
        Value before, after;
        detail::KeyedState<Value> keyed;
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
