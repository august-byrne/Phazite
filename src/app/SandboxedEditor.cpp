// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SandboxedEditor.h"
#include "model/plugins/internal/SandboxedInstance.h"
#include "ui/input/PluginKeys.h"
#include "ui/theme/LookAndFeel.h"

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
    constexpr int openMilliseconds = 10000; // how long a plug-in may take to make its editor
    constexpr int keyChecksPerSecond = 4;   // how often it looks whether the keys Phazite takes changed
    constexpr int largestEditor = 16384;    // the most pixels a side of a plug-in's editor is taken to have

#if JUCE_WINDOWS
    constexpr int socketLookMilliseconds = 20; // how often the sockets of closed editors are looked at
#else
    constexpr int closeMilliseconds = 2000; // how long a plug-in may take to take its editor out of Phazite's window
#endif

    // A size the helper told, as a side can be.
    int keepSize (int pixels)
    {
        return juce::jlimit (1, largestEditor, pixels);
    }

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

    // The sockets of the editors that closed, each hidden and in no window of Phazite's, until the helper's window has left it
    // (the helper closed it, or went): destroyed with it still in, Windows would wait for the helper's thread to answer.
    class ClosingSockets final : public juce::DeletedAtShutdown,
                                 private juce::Timer
    {
    public:
        static void keep (HWND socket)
        {
            if (instance == nullptr)
                instance = new ClosingSockets();

            instance->sockets.push_back (socket);
            instance->startTimer (socketLookMilliseconds);
        }

        ~ClosingSockets() override
        {
            // As Phazite quits: one that still holds a helper's window is left to the system, which ends it with Phazite
            // (destroyed here, it would wait for that helper's thread).
            stopTimer();

            for (auto* socket : sockets)
                if (GetWindow (socket, GW_CHILD) == nullptr)
                    DestroyWindow (socket);

            instance = nullptr;
        }

        // A socket taken out of Phazite's window: hidden, in no window that shows (a message-only window's place), where
        // nothing it holds is destroyed with Phazite's.
        static void takeOut (HWND socket)
        {
            ShowWindow (socket, SW_HIDE);
            SetParent (socket, HWND_MESSAGE);
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

    private:
        static inline ClosingSockets* instance = nullptr;
        std::vector<HWND> sockets;

        void timerCallback() override
        {
            std::erase_if (sockets,
                           [] (HWND socket)
                           {
                               if (GetWindow (socket, GW_CHILD) != nullptr)
                                   return false;

                               DestroyWindow (socket);
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

    instance->onEditorKey = [this] (bool isDown, int keyCode, int modifiers) { runKey (isDown, keyCode, modifiers); };
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

#if JUCE_WINDOWS
    // Asked to close without waiting, and its window taken out of Phazite's in the socket (which is Phazite's, so that nothing
    // here waits for the helper's thread), kept until the helper's window has left it (known bug 165).
    if (! instance->hasCrashed())
        instance->getRemote().closeEditor (0);

    if (auto* held = static_cast<HWND> (socket); held != nullptr && IsWindow (held))
    {
        ClosingSockets::unwatch (static_cast<HWND> (socketParent), held);
        ClosingSockets::takeOut (held);
        ClosingSockets::keep (held);
    }
#else
    if (! instance->hasCrashed())
        instance->getRemote().closeEditor (closeMilliseconds);
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

    if (toldKeys && everyKey == toldEveryKey && wanted == toldWanted)
        return;

    std::vector<std::pair<int, int>> codes;

    for (const auto& key : wanted)
        codes.emplace_back (key.getKeyCode(), key.getModifiers().getRawFlags());

    instance->getRemote().setEditorKeys (everyKey, codes);
    toldKeys = true;
    toldEveryKey = everyKey;
    toldWanted = wanted;
}

// One the helper handed back: run as Phazite's own hook runs a key over a plug-in's window.
void SandboxedEditor::runKey (bool isDown, int keyCode, int modifiers)
{
    auto* keys = ui::getPluginKeys();

    if (keys == nullptr)
        return;

    keys->keyStateChanged (isDown, this);

    if (isDown)
        keys->keyPressed (juce::KeyPress (keyCode, juce::ModifierKeys (modifiers), 0), this);
}

std::unique_ptr<SandboxedEditor> createSandboxedEditor (std::shared_ptr<SandboxedInstance> instance)
{
    if (instance == nullptr || instance->hasCrashed() || ! instance->getRemote().getInfo().hasEditor)
        return nullptr;

    if (const auto size = instance->getRemote().openEditor (openMilliseconds))
        return std::unique_ptr<SandboxedEditor> (new SandboxedEditor (std::move (instance), size->width, size->height, size->isResizable));

    return nullptr;
}
} // namespace phazite
