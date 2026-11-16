// SPDX-License-Identifier: AGPL-3.0-or-later
// The computer's session ending (Windows shutting down, restarting or signing the user out), and not ending after all (the
// user cancelled the shutdown), as a hidden top-level window of Phazite's own hears it: JUCE's windows take
// WM_QUERYENDSESSION for systemRequestedQuit(), as they take an ordinary quit, and hear nothing of WM_ENDSESSION, which
// says that the end was cancelled. JUCE workaround W49 (docs/juce-workarounds.md). Told on the message thread, whichever
// of Phazite's windows Windows asks first (known bug 168). A program that asks Phazite to close (an installer, through
// Restart Manager: ENDSESSION_CLOSEAPP) is no session's end: Phazite closes as an ordinary quit does. Nothing on other
// systems yet.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace phazite
{
class SessionEndWatch final
{
public:
    // What it tells: true as the session is to end, false as its end is cancelled.
    explicit SessionEndWatch (std::function<void (bool ending)> onChange);
    ~SessionEndWatch();

    bool isEnding() const noexcept { return ending; }

    // What its window does with a message of Windows' (its procedure calls it; the tests call it as Windows would), and
    // what the window answers.
    std::intptr_t handle (unsigned int message, std::uintptr_t wParam, std::intptr_t lParam);

    SessionEndWatch (const SessionEndWatch&) = delete;
    SessionEndWatch& operator= (const SessionEndWatch&) = delete;

private:
    std::function<void (bool)> onChange;
    bool ending = false;
    void* window = nullptr; // its native handle, on Windows
};
} // namespace phazite
