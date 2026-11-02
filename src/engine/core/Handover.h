// SPDX-License-Identifier: AGPL-3.0-or-later
// How the message thread hands the audio thread something to play, whole, without either waiting for the other
// (docs/tracktion-migration.md, T2): the message thread builds it, then gives it; the audio thread takes what was given
// last as each of its blocks starts. What it replaces is freed on the message thread, never on the audio thread, once the
// audio thread has finished a block since it was replaced: the next time something is given, or with the handover.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace phazite::engine
{
template <typename T>
class Handover
{
public:
    Handover() = default;

    // Message thread. The audio thread plays it from its next block on; or nothing, if it is nullptr.
    void give (std::unique_ptr<T> next)
    {
        // Those replaced before the audio thread last started a block that has since ended are no longer played.
        const auto finished = blocksFinished.load();
        std::erase_if (replaced, [finished] (const auto& old) { return finished >= old.second; });

        playing.store (next.get());

        // A block the audio thread is in now may have taken the one replaced as it started, and ends by the next count.
        if (given != nullptr)
            replaced.emplace_back (std::move (given), blocksFinished.load() + 2);

        given = std::move (next);
    }

    // Message thread: what was given last, or nullptr.
    T* get() const noexcept { return given.get(); }

    // Message thread, while the audio thread can't be in a block (it has stopped): frees what was replaced at once.
    void freeReplaced() { replaced.clear(); }

    // The audio thread's hold on what it plays in a block: what was given last as the block starts, until the block ends
    // (as it goes out of scope, however the block ends). Real-time safe.
    class Block
    {
    public:
        explicit Block (Handover& h) noexcept
            : handover (h),
              taken (h.playing.load())
        {
        }
        ~Block() { handover.blocksFinished.fetch_add (1); }

        T* get() const noexcept { return taken; }

    private:
        Handover& handover;
        T* const taken;

        Block (const Block&) = delete;
        Block& operator= (const Block&) = delete;
    };

private:
    // Sequentially consistent, so that the message thread's count after it gave something new can't be taken before it.
    std::atomic<T*> playing { nullptr };
    std::atomic<std::uint64_t> blocksFinished { 0 };
    static_assert (std::atomic<std::uint64_t>::is_always_lock_free);

    std::unique_ptr<T> given;
    std::vector<std::pair<std::unique_ptr<T>, std::uint64_t>> replaced; // each with the count of blocks after which it can go

    Handover (const Handover&) = delete;
    Handover& operator= (const Handover&) = delete;
};
} // namespace phazite::engine
