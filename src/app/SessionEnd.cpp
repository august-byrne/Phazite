// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SessionEnd.h"

#include <juce_core/juce_core.h>

#include <utility>

// Last, so that its macros can't change the headers above.
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace phazite
{
#if JUCE_WINDOWS
namespace
{
    LRESULT CALLBACK watchProcedure (HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (auto* watch = reinterpret_cast<SessionEndWatch*> (GetWindowLongPtrW (window, GWLP_USERDATA)))
            if (message == WM_QUERYENDSESSION || message == WM_ENDSESSION)
                return (LRESULT) watch->handle (message, (std::uintptr_t) wParam, (std::intptr_t) lParam);

        return DefWindowProcW (window, message, wParam, lParam);
    }
} // namespace
#endif

SessionEndWatch::SessionEndWatch (std::function<void (bool)> told)
    : onChange (std::move (told))
{
#if JUCE_WINDOWS
    // A top-level window, never shown: Windows asks every one whether the session may end (a message-only window hears none
    // of it).
    auto* module = static_cast<HINSTANCE> (juce::Process::getCurrentModuleInstanceHandle());
    static const auto* className = [module]
    {
        WNDCLASSEXW watchClass {};
        watchClass.cbSize = sizeof (watchClass);
        watchClass.lpfnWndProc = watchProcedure;
        watchClass.hInstance = module;
        watchClass.lpszClassName = L"PhaziteSessionEnd";
        RegisterClassExW (&watchClass);
        return watchClass.lpszClassName;
    }();

    auto* made = CreateWindowExW (WS_EX_TOOLWINDOW, className, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, module, nullptr);

    if (made != nullptr)
        SetWindowLongPtrW (made, GWLP_USERDATA, (LONG_PTR) this);

    window = made;
#endif
}

SessionEndWatch::~SessionEndWatch()
{
#if JUCE_WINDOWS
    if (auto* made = static_cast<HWND> (window))
    {
        SetWindowLongPtrW (made, GWLP_USERDATA, 0);
        DestroyWindow (made);
    }
#endif
}

std::intptr_t SessionEndWatch::handle ([[maybe_unused]] unsigned int message, [[maybe_unused]] std::uintptr_t wParam,
                                       [[maybe_unused]] std::intptr_t lParam)
{
#if JUCE_WINDOWS
    // Asked whether the session may end (a shutdown, a restart, or a sign-out, which lParam says): it may, as far as this
    // window goes; the app's own windows answer for the project.
    if (message == WM_QUERYENDSESSION)
    {
        // Not the session's end but a program that asks Phazite to close (an installer, through Windows' Restart Manager):
        // closed as an ordinary quit is, which JUCE's window begins (its plug-ins asked, the save question asked).
        if ((lParam & ENDSESSION_CLOSEAPP) != 0)
            return TRUE;

        ending = true;

        if (onChange != nullptr)
            onChange (true);

        return TRUE;
    }

    // Told whether it ends: not after all (another program, or the user, cancelled it).
    if (message == WM_ENDSESSION && wParam == FALSE && ending)
    {
        ending = false;

        if (onChange != nullptr)
            onChange (false);
    }
#endif

    return 0;
}
} // namespace phazite
