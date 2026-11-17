// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SandboxedEditor.h"
#include "devices/core/Clock.h"
#include "model/plugins/internal/SandboxedInstance.h"
#include "ui/input/PluginKeys.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <vector>

// Last, so that its macros can't change the headers above.
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
// clang-format off: commctrl.h needs windows.h first
 #include <windows.h>
 #include <commctrl.h>
// clang-format on
 #ifdef _MSC_VER
  #pragma comment(lib, "comctl32.lib")
 #endif
#endif

namespace phazite
{
namespace
{
    constexpr double openMilliseconds = 30000.0; // how long a plug-in's helper may make its editor before a hang may end it
    constexpr int openLookMilliseconds = 10;     // how often the answer to an editor asked for is looked at
    constexpr int keyChecksPerSecond = 4;        // how often it looks whether the keys Phazite takes changed
    constexpr int largestEditor = 16384;         // the most pixels a side of a plug-in's editor is taken to have
    constexpr int socketLookMilliseconds = 20;   // how often the sockets of closed editors are looked at

#if ! JUCE_WINDOWS
    constexpr int closeMilliseconds = 2000; // without a socket: how long the helper may take to take its window out
#endif

    // A size the helper told, as a side can be.
    int keepSize (int pixels)
    {
        return juce::jlimit (1, largestEditor, pixels);
    }
} // namespace

#if JUCE_LINUX || JUCE_BSD
// The sockets on Linux: X windows of Phazite's own, through a display of its own (at the end of this file, after the X
// headers, whose macros would change the code here). Each takes and gives its X window's ID as a pointer, as JUCE's native
// handles are; none without a display.
namespace x11
{
    void* makeSocket (void* parent);
    void placeSocket (void* socket, void* parent, juce::Rectangle<int> area); // shown there, in that window
    void takeOutSocket (void* socket);                                        // unmapped, into the root window, out of any of Phazite's
    bool holdsWindow (void* socket);                                          // whether a window is still in it (the helper's)
    void destroySocket (void* socket);
} // namespace x11
#endif

namespace
{
#if JUCE_WINDOWS
    // A socket: a plain child window of Phazite's own, the colour it is given under the helper's window, which tells its
    // parent nothing of what happens in it.
    LRESULT CALLBACK socketProcedure (HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_ERASEBKGND)
        {
            RECT area {};
            GetClientRect (window, &area);
            auto* brush = CreateSolidBrush ((COLORREF) GetWindowLongPtrW (window, GWLP_USERDATA));
            FillRect (reinterpret_cast<HDC> (wParam), &area, brush);
            DeleteObject (brush);
            return 1;
        }

        // The keys the plug-in's window doesn't take, which JUCE's window in the helper posts on to its parent, this, as to
        // the window of a host it is in: posted on to Phazite's, which takes them as it did before there was a socket
        // (known bug 165). JUCE workaround W50 (docs/juce-workarounds.md).
        if (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP || message == WM_CHAR
            || message == WM_SYSCHAR)
        {
            if (auto* parent = GetParent (window); parent != nullptr && parent != HWND_MESSAGE)
                PostMessageW (parent, message, wParam, lParam);

            return 0;
        }

        return DefWindowProcW (window, message, wParam, lParam);
    }

    HWND makeSocket (HWND parent, juce::Colour colour)
    {
        auto* module = static_cast<HINSTANCE> (juce::Process::getCurrentModuleInstanceHandle());
        static const auto* className = [module]
        {
            WNDCLASSEXW socketClass {};
            socketClass.cbSize = sizeof (socketClass);
            socketClass.lpfnWndProc = socketProcedure;
            socketClass.hInstance = module;
            socketClass.lpszClassName = L"PhazitePluginSocket";
            RegisterClassExW (&socketClass);
            return socketClass.lpszClassName;
        }();

        auto* socket = CreateWindowExW (WS_EX_NOPARENTNOTIFY, className, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                                        parent, nullptr, module, nullptr);

        if (socket != nullptr)
            SetWindowLongPtrW (socket, GWLP_USERDATA, (LONG_PTR) RGB (colour.getRed(), colour.getGreen(), colour.getBlue()));

        return socket;
    }

    // Whether a helper's window is still in a socket, and the socket destroyed.
    bool holdsWindow (void* socket)
    {
        return GetWindow (static_cast<HWND> (socket), GW_CHILD) != nullptr;
    }

    void destroySocket (void* socket)
    {
        DestroyWindow (static_cast<HWND> (socket));
    }
#elif JUCE_LINUX || JUCE_BSD
    using x11::destroySocket;
    using x11::holdsWindow;
#endif

#if JUCE_WINDOWS || JUCE_LINUX || JUCE_BSD
    // The sockets of the editors that closed, each hidden and in no window of Phazite's, until the helper's window has left it
    // (the helper closed it, or went): destroyed with it still in, Windows would wait for the helper's thread to answer, and
    // on Linux the helper's window would be destroyed under it. On Linux, where the system doesn't take a window away with
    // the process that made it as Windows does, one is destroyed once its helper has gone too (helperAlive), never while a
    // helper that lives still has its window in it.
    class ClosingSockets final : public juce::DeletedAtShutdown,
                                 private juce::Timer
    {
    public:
        static void keep (void* socket, std::function<bool()> helperAlive)
        {
            if (instance == nullptr)
                instance = new ClosingSockets();

            instance->sockets.push_back ({ socket, std::move (helperAlive) });
            instance->startTimer (socketLookMilliseconds);
        }

        ~ClosingSockets() override
        {
            // As Phazite quits: one that still holds a helper's window is left to the system, which ends it with Phazite
            // (destroyed here, on Windows it would wait for that helper's thread).
            stopTimer();

            for (const auto& closing : sockets)
                if (! holdsWindow (closing.socket))
                    destroySocket (closing.socket);

            instance = nullptr;
        }

 #if JUCE_WINDOWS
        // A socket taken out of Phazite's window: hidden, in no window that shows (a message-only window's place), where
        // nothing it holds is destroyed with Phazite's.
        static void takeOut (void* socket)
        {
            ShowWindow (static_cast<HWND> (socket), SW_HIDE);
            SetParent (static_cast<HWND> (socket), HWND_MESSAGE);
        }

        // The window a socket is in, watched as it goes: Windows destroys a window's children after it has told it, so that
        // the socket is taken out first (known bug 165: an editor whose window lost its native one before it went had its
        // socket destroyed with the helper's window in it, which waited for the helper's thread).
        static LRESULT CALLBACK watchParent (HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR socket)
        {
            if (message == WM_DESTROY)
            {
                if (auto* held = reinterpret_cast<HWND> (socket); IsWindow (held) && GetParent (held) == window)
                    takeOut (held);
            }
            else if (message == WM_NCDESTROY)
            {
                RemoveWindowSubclass (window, watchParent, (UINT_PTR) socket);
            }

            return DefSubclassProc (window, message, wParam, lParam);
        }

        static void watch (HWND parent, HWND socket) { SetWindowSubclass (parent, watchParent, (UINT_PTR) socket, (DWORD_PTR) socket); }

        static void unwatch (HWND parent, HWND socket)
        {
            if (parent != nullptr && IsWindow (parent))
                RemoveWindowSubclass (parent, watchParent, (UINT_PTR) socket);
        }
 #else
        static void takeOut (void* socket) { x11::takeOutSocket (socket); }
 #endif

    private:
        struct Closing
        {
            void* socket = nullptr;
            std::function<bool()> helperAlive; // whether the helper whose window it held still runs
        };

        static inline ClosingSockets* instance = nullptr;
        std::vector<Closing> sockets;

        void timerCallback() override
        {
            std::erase_if (sockets,
                           [] (const Closing& closing)
                           {
 #if JUCE_WINDOWS
                               const auto stays = holdsWindow (closing.socket);
 #else
                               const auto stays = holdsWindow (closing.socket) && closing.helperAlive != nullptr && closing.helperAlive();
 #endif
                               if (stays)
                                   return false;

                               destroySocket (closing.socket);
                               return true;
                           });

            if (sockets.empty())
                stopTimer();
        }
    };
#endif
} // namespace

SandboxedEditor::SandboxedEditor (std::shared_ptr<SandboxedInstance> sandboxed, int width, int height, bool canResize)
    : juce::ComponentMovementWatcher (this),
      instance (std::move (sandboxed)),
      resizable (canResize),
      pluginSize (keepSize (width), keepSize (height))
{
    setOpaque (true);
    fitPlugin();

    // As the plug-in resizes its own, and a key Phazite takes, pressed over it.
    instance->onEditorResized = [this] (int newWidth, int newHeight)
    {
        pluginSize = { keepSize (newWidth), keepSize (newHeight) };
        fitPlugin();
    };

    instance->onEditorKey = [this] (bool isDown, int keyCode, int modifiers, int scanCode)
    { runKey (isDown, keyCode, modifiers, scanCode); };
    instance->editorOpened(); // what its window changes, the plug-in needn't tell
    startTimerHz (keyChecksPerSecond);
}

float SandboxedEditor::getScale() const
{
    // Its units in the screen's (as the platform counts them, before its own scale): the Interface Size (JUCE's global scale,
    // which is its window's) and any transform, as JUCE works it out; the helper's window has none of them. Before it is in a
    // window, as the window it goes into will scale it.
    const auto global = juce::Desktop::getInstance().getGlobalScaleFactor();
    const auto scale = juce::Component::getApproximateScaleFactorForComponent (this) * global;
    return getPeer() != nullptr ? scale : scale * global;
}

void SandboxedEditor::fitPlugin()
{
    // As large as the plug-in's editor, which is in the screen's units (known bug 68: its size in pixels taken for this one's
    // cropped it below 100 % and left margins above): unless this is that size already, so that a size told back from a
    // resize made here doesn't move it by a rounding.
    fittedScale = getScale();
    const auto toFit = [this] (int pixels, int now)
    {
        return juce::roundToInt ((float) now * fittedScale) == pixels ? now : std::max (1, juce::roundToInt ((float) pixels / fittedScale));
    };

    setSize (toFit (pluginSize.x, getWidth()), toFit (pluginSize.y, getHeight()));
}

SandboxedEditor::~SandboxedEditor()
{
    instance->onEditorResized = nullptr;
    instance->onEditorKey = nullptr;
    instance->editorClosed();

    // Asked to close without waiting (known bug 166: on Linux the close waited up to 2 seconds), and its window taken out of
    // Phazite's in the socket (which is Phazite's, so that nothing here waits for the helper's thread), kept until the helper's
    // window has left it (known bug 165).
    if (! instance->hasCrashed())
    {
#if JUCE_WINDOWS
        instance->getRemote().closeEditor();
#else
        // Without a socket of its own (no display to make one with; macOS), the helper's window is in Phazite's, which would
        // destroy it under the helper as it goes: the helper is given the time it takes to take it out, as before.
        if (socket == nullptr && placedWindow != 0)
            instance->getRemote().askToCloseEditor()->wait (closeMilliseconds);
        else
            instance->getRemote().closeEditor();
#endif
    }

#if JUCE_WINDOWS
    if (auto* held = static_cast<HWND> (socket); held != nullptr && IsWindow (held))
    {
        ClosingSockets::unwatch (static_cast<HWND> (socketParent), held);
        ClosingSockets::takeOut (held);
        ClosingSockets::keep (held, {});
    }
#elif JUCE_LINUX || JUCE_BSD
    if (socket != nullptr)
    {
        ClosingSockets::takeOut (socket);
        ClosingSockets::keep (socket, instance->watchHelper());
    }
#endif
}

// Under the plug-in's window, before it shows.
void SandboxedEditor::paint (juce::Graphics& g)
{
    g.fillAll (ui::getPalette (*this).surface);
}

void SandboxedEditor::lookAndFeelChanged()
{
#if JUCE_WINDOWS
    if (auto* held = static_cast<HWND> (socket); held != nullptr && IsWindow (held))
    {
        const auto colour = ui::getPalette (*this).surface;
        SetWindowLongPtrW (held, GWLP_USERDATA, (LONG_PTR) RGB (colour.getRed(), colour.getGreen(), colour.getBlue()));
        InvalidateRect (held, nullptr, TRUE);
    }
#endif
}

// Where it is in its window, in the window's pixels as the screen counts them; nowhere while it doesn't show.
void SandboxedEditor::place()
{
    // The Interface Size changed, or it went into a window that scales otherwise: as large as the plug-in again, which places
    // it once more.
    if (! juce::approximatelyEqual (getScale(), fittedScale))
        fitPlugin();

    auto* peer = getPeer();
    const auto showing = peer != nullptr && isShowing() && ! instance->hasCrashed();
    auto window = showing ? (std::int64_t) (juce::pointer_sized_int) peer->getNativeHandle() : 0;
    auto area = showing ? (peer->getAreaCoveredBy (*this).toFloat() * peer->getPlatformScaleFactor()).getSmallestIntegerContainer()
                        : juce::Rectangle<int>();

#if JUCE_WINDOWS
    // In its socket, which goes where this is: the helper's window fills it. (One destroyed with the window it was in, as
    // that went before this, is made again.)
    if (socket != nullptr && ! IsWindow (static_cast<HWND> (socket)))
        socket = nullptr;

    if (showing)
    {
        const auto parent = static_cast<HWND> (peer->getNativeHandle());

        if (socket == nullptr)
            socket = makeSocket (parent, ui::getPalette (*this).surface);
        else if (GetParent (static_cast<HWND> (socket)) != parent)
            SetParent (static_cast<HWND> (socket), parent);

        // Its window watched as it goes, which takes it out first (ClosingSockets::watchParent()).
        if (socket != nullptr && socketParent != parent)
        {
            ClosingSockets::unwatch (static_cast<HWND> (socketParent), static_cast<HWND> (socket));
            ClosingSockets::watch (parent, static_cast<HWND> (socket));
            socketParent = parent;
        }
    }

    if (auto* held = static_cast<HWND> (socket); held != nullptr && showing)
    {
        SetWindowPos (held, nullptr, area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                      SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        window = (std::int64_t) (juce::pointer_sized_int) held;
        area = area.withZeroOrigin();
    }
    else if (held != nullptr)
    {
        ShowWindow (held, SW_HIDE);
        window = 0;
        area = {};
    }
#elif JUCE_LINUX || JUCE_BSD
    // The same in an X window of Phazite's own, where there is a display to make one with; else in Phazite's window itself.
    // X destroys every window inside one that is destroyed, and nothing tells this before JUCE's window goes: the socket is
    // taken out into the root window, hidden, as soon as this stops showing (its window hidden, or this taken out of it), and
    // goes into the window it shows in again, whichever that is now. JUCE workaround W52 (docs/juce-workarounds.md).
    if (showing)
    {
        auto* parent = peer->getNativeHandle();

        if (socket == nullptr)
            socket = x11::makeSocket (parent);

        socketParent = parent;
    }

    if (socket != nullptr)
    {
        if (showing)
            x11::placeSocket (socket, socketParent, area);
        else
            x11::takeOutSocket (socket);

        window = showing ? (std::int64_t) (juce::pointer_sized_int) socket : 0;
        area = showing ? area.withZeroOrigin() : juce::Rectangle<int>();
    }
#endif

    if (window == placedWindow && area == placedArea)
        return;

    placedWindow = window;
    placedArea = area;
    instance->getRemote().placeEditor (window, area.getX(), area.getY(), area.getWidth(), area.getHeight());
    timerCallback(); // the keys, now that its window is known
}

// The keys Phazite takes over its window, told again as they change.
void SandboxedEditor::timerCallback()
{
    if (! juce::approximatelyEqual (getScale(), fittedScale))
        place();

    auto* keys = ui::getPluginKeys();
    const auto everyKey = keys != nullptr && keys->sendsAllKeys (*getTopLevelComponent());
    const auto wanted = keys != nullptr ? keys->getWantedKeys() : std::vector<juce::KeyPress>();
    const auto wantedPlaces = keys != nullptr ? keys->getWantedPlaces() : std::vector<std::pair<unsigned int, int>>();

    if (toldKeys && everyKey == toldEveryKey && wanted == toldWanted && wantedPlaces == toldPlaces)
        return;

    std::vector<std::pair<int, int>> codes, places;

    for (const auto& key : wanted)
        codes.emplace_back (key.getKeyCode(), key.getModifiers().getRawFlags());

    for (const auto& [scanCode, modifiers] : wantedPlaces)
        places.emplace_back ((int) scanCode, modifiers);

    instance->getRemote().setEditorKeys (everyKey, codes, places);
    toldKeys = true;
    toldEveryKey = everyKey;
    toldWanted = wanted;
    toldPlaces = wantedPlaces;
}

void SandboxedEditor::runKey (bool isDown, int keyCode, int modifiers, int scanCode)
{
    auto* keys = ui::getPluginKeys();

    if (keys == nullptr)
        return;

    keys->keyStateChanged (isDown, this);

    if (isDown)
        keys->keyPressed (keys->makeHandedBackKey (keyCode, modifiers, (unsigned int) juce::jmax (0, scanCode)), this);
}

//==============================================================================
struct SandboxedEditorRequest::Asked
{
    std::shared_ptr<sandbox::Helper::Reply> reply;
    double since = 0.0;
};

SandboxedEditorRequest::SandboxedEditorRequest (std::shared_ptr<SandboxedInstance> sandboxed, Made then)
    : instance (std::move (sandboxed)),
      asked (std::make_unique<Asked> (Asked { instance->getRemote().askToOpenEditor(), devices::Clock::getMilliseconds() })),
      made (std::move (then))
{
    startTimer (openLookMilliseconds);
}

SandboxedEditorRequest::~SandboxedEditorRequest()
{
    // Let go of before its editor came: the helper closes what it makes (it takes what it is asked in turn).
    stopTimer();

    if (asked != nullptr && ! instance->hasCrashed())
        instance->getRemote().closeEditor();
}

void SandboxedEditorRequest::timerCallback()
{
    // Waited for as long as the helper lives (the review of known bug 166: a big editor that took longer than ten seconds
    // never opened): for a while the helper isn't taken for hung meanwhile, as it makes the editor on its message thread;
    // after that it is, if its message thread doesn't answer the heartbeat, which ends the wait with its helper.
    if (! asked->reply->isDone() && ! instance->hasCrashed())
    {
        if (devices::Clock::getMilliseconds() - asked->since >= openMilliseconds)
            asked->reply->stopHoldingOffHang();

        return;
    }

    // Done: the editor made of the helper's answer (none for no answer), handed over last, as what takes it may let go of
    // this.
    stopTimer();
    const auto size = instance->hasCrashed() ? std::nullopt : sandbox::RemoteInstance::readEditor (*asked->reply);
    std::unique_ptr<SandboxedEditor> editor;

    if (size.has_value())
        editor.reset (new SandboxedEditor (instance, size->width, size->height, size->isResizable)); // which closes it as it goes
    else if (! instance->hasCrashed())
        instance->getRemote().closeEditor();

    asked = nullptr; // nothing left to close as this goes
    const auto then = std::move (made);

    if (then != nullptr)
        then (std::move (editor));
}

std::unique_ptr<SandboxedEditorRequest> requestSandboxedEditor (std::shared_ptr<SandboxedInstance> instance,
                                                                SandboxedEditorRequest::Made then)
{
    if (instance == nullptr || instance->hasCrashed() || ! instance->getRemote().getInfo().hasEditor)
        return nullptr;

    return std::unique_ptr<SandboxedEditorRequest> (new SandboxedEditorRequest (std::move (instance), std::move (then)));
}
} // namespace phazite

//==============================================================================
// Linux's sockets. Last, so that the X headers and their macros can't change the code above.
#if JUCE_LINUX || JUCE_BSD
// As juce_gui_basics.h includes them (JUCE_GUI_BASICS_INCLUDE_XHEADERS).
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wvariadic-macros")
 #include <X11/Xlib.h>
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

#include <dlfcn.h>

namespace phazite::x11
{
namespace
{
    // A display of Phazite's own (libX11 loaded at run time, as JUCE loads it), whose windows are the sockets: X windows of one
    // display can be another client's parent (the helper's) and child (of JUCE's window). Its requests go out at once, and
    // those that make or move a socket are waited for by the X server alone (XSync()), so that the helper finds the socket
    // and the socket is out before the window it was in can go. JUCE workaround W53 (docs/juce-workarounds.md).
    struct Display11
    {
        Display11()
        {
            if (auto* openDisplay = reinterpret_cast<decltype (&::XOpenDisplay)> (getFunction ("XOpenDisplay")))
                display = openDisplay (nullptr);

            createWindow = reinterpret_cast<decltype (&::XCreateSimpleWindow)> (getFunction ("XCreateSimpleWindow"));
            destroyWindow = reinterpret_cast<decltype (&::XDestroyWindow)> (getFunction ("XDestroyWindow"));
            mapWindow = reinterpret_cast<decltype (&::XMapWindow)> (getFunction ("XMapWindow"));
            unmapWindow = reinterpret_cast<decltype (&::XUnmapWindow)> (getFunction ("XUnmapWindow"));
            moveResize = reinterpret_cast<decltype (&::XMoveResizeWindow)> (getFunction ("XMoveResizeWindow"));
            reparent = reinterpret_cast<decltype (&::XReparentWindow)> (getFunction ("XReparentWindow"));
            queryTree = reinterpret_cast<decltype (&::XQueryTree)> (getFunction ("XQueryTree"));
            freeData = reinterpret_cast<decltype (&::XFree)> (getFunction ("XFree"));
            flush = reinterpret_cast<decltype (&::XFlush)> (getFunction ("XFlush"));
            syncDisplay = reinterpret_cast<decltype (&::XSync)> (getFunction ("XSync"));
            rootWindow = reinterpret_cast<decltype (&::XDefaultRootWindow)> (getFunction ("XDefaultRootWindow"));
            setErrorHandler = reinterpret_cast<decltype (&::XSetErrorHandler)> (getFunction ("XSetErrorHandler"));

            if (createWindow == nullptr || destroyWindow == nullptr || mapWindow == nullptr || unmapWindow == nullptr
                || moveResize == nullptr || reparent == nullptr || queryTree == nullptr || freeData == nullptr || flush == nullptr
                || syncDisplay == nullptr || rootWindow == nullptr || setErrorHandler == nullptr)
                display = nullptr; // no sockets (a display opened is left open, as below)
        }

        // The library, opened as juce::DynamicLibrary opens one and never closed, as the display below isn't: a
        // juce::DynamicLibrary still alive as the program ends is a leak to JUCE's leak detector (Debug builds), which the
        // tests' assertion net failed the plug-ins' tests for (CI's linux-debug, 2026-11-17).
        void* const library = dlopen ("libX11.so.6", RTLD_LOCAL | RTLD_NOW);

        void* getFunction (const char* name) const { return library != nullptr ? dlsym (library, name) : nullptr; }

        ::Display* display = nullptr;
        decltype (&::XCreateSimpleWindow) createWindow = nullptr;
        decltype (&::XDestroyWindow) destroyWindow = nullptr;
        decltype (&::XMapWindow) mapWindow = nullptr;
        decltype (&::XUnmapWindow) unmapWindow = nullptr;
        decltype (&::XMoveResizeWindow) moveResize = nullptr;
        decltype (&::XReparentWindow) reparent = nullptr;
        decltype (&::XQueryTree) queryTree = nullptr;
        decltype (&::XFree) freeData = nullptr;
        decltype (&::XFlush) flush = nullptr;
        decltype (&::XSync) syncDisplay = nullptr;
        decltype (&::XDefaultRootWindow) rootWindow = nullptr;
        decltype (&::XSetErrorHandler) setErrorHandler = nullptr;
    };

    // Opened once and never closed, on purpose: closed as static objects go, after JUCE has taken its X error handler away,
    // an error still to come would reach Xlib's own, which ends the process inside its exit; and its windows, the sockets,
    // go with the process anyway. A pointer that stays reachable, which a leak checker doesn't count.
    Display11& getDisplay()
    {
        static auto* const opened = new Display11();
        return *opened;
    }

    // An error a socket's request meets (a window that died meanwhile: the helper's, or the one it was in) taken for nothing,
    // with a handler of its own while the requests are made and until the X server has answered them (XSync()), as JUCE does
    // around its own; the handler before put back then.
    int ignoreError (::Display*, ::XErrorEvent*)
    {
        return 0;
    }

    struct ErrorsIgnored
    {
        explicit ErrorsIgnored (Display11& d)
            : x (d),
              before (x.setErrorHandler (ignoreError))
        {
        }

        ~ErrorsIgnored()
        {
            x.syncDisplay (x.display, False);
            x.setErrorHandler (before);
        }

        Display11& x;
        const ::XErrorHandler before;

        JUCE_DECLARE_NON_COPYABLE (ErrorsIgnored)
    };

    ::Window toWindow (void* handle)
    {
        return (::Window) (juce::pointer_sized_uint) handle;
    }

    void* toHandle (::Window window)
    {
        return (void*) (juce::pointer_sized_uint) window;
    }

    // The window a window is in now, and whether any is in it; 0 and false where the X server can't tell.
    ::Window findParent (Display11& x, ::Window window, bool* holdsAny = nullptr)
    {
        ::Window root = 0, inside = 0;
        ::Window* children = nullptr;
        unsigned int numChildren = 0;
        const ErrorsIgnored ignored (x);

        if (x.queryTree (x.display, window, &root, &inside, &children, &numChildren) == 0)
            return 0;

        if (holdsAny != nullptr)
            *holdsAny = numChildren > 0;

        if (children != nullptr)
            x.freeData (children);

        return inside;
    }
} // namespace

void* makeSocket (void* parent)
{
    auto& x = getDisplay();

    if (x.display == nullptr || parent == nullptr)
        return nullptr;

    const ErrorsIgnored ignored (x); // there by the time the helper is told of it (its XSync())
    const auto made = x.createWindow (x.display, toWindow (parent), 0, 0, 1, 1, 0, 0, 0);
    return made != 0 ? toHandle (made) : nullptr;
}

void placeSocket (void* socket, void* parent, juce::Rectangle<int> area)
{
    auto& x = getDisplay();

    if (x.display == nullptr || socket == nullptr || parent == nullptr)
        return;

    const auto window = toWindow (socket);

    if (area.isEmpty())
    {
        takeOutSocket (socket);
        return;
    }

    // In the window it is to be in (another, if JUCE made its window again, or it was taken out), where this is.
    const auto inside = findParent (x, window);
    const ErrorsIgnored ignored (x);

    if (inside != toWindow (parent))
        x.reparent (x.display, window, toWindow (parent), area.getX(), area.getY());

    x.moveResize (x.display, window, area.getX(), area.getY(), (unsigned int) std::max (1, area.getWidth()),
                  (unsigned int) std::max (1, area.getHeight()));
    x.mapWindow (x.display, window);
}

void takeOutSocket (void* socket)
{
    auto& x = getDisplay();

    if (x.display == nullptr || socket == nullptr)
        return;

    const auto window = toWindow (socket);
    const ErrorsIgnored ignored (x); // out before the window it was in can go (its XSync())
    x.unmapWindow (x.display, window);
    x.reparent (x.display, window, x.rootWindow (x.display), 0, 0);
}

bool holdsWindow (void* socket)
{
    auto& x = getDisplay();

    if (x.display == nullptr || socket == nullptr)
        return false;

    auto holds = false;
    findParent (x, toWindow (socket), &holds);
    return holds;
}

void destroySocket (void* socket)
{
    auto& x = getDisplay();

    if (x.display == nullptr || socket == nullptr)
        return;

    const ErrorsIgnored ignored (x);
    x.destroyWindow (x.display, toWindow (socket));
}
} // namespace phazite::x11
#endif
