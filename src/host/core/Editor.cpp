// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Editor.h"

#include <algorithm>

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

namespace phazite::host
{
namespace
{
    // A component and everything in it.
    void forEachComponent (juce::Component& component, const std::function<void (juce::Component&)>& action)
    {
        action (component);

        for (auto* child : component.getChildren())
            forEachComponent (*child, action);
    }
} // namespace

// The window the editor is in: as large as the editor, but as Phazite places it while the editor can be resized.
class Editor::Holder final : public juce::Component,
                             private juce::KeyListener
{
public:
    Holder (juce::AudioPluginInstance& plugin, std::function<void (int, int)> onResized,
            std::function<void (bool, const juce::KeyPress&, int)> onKey)
        : resizedByPlugin (std::move (onResized)),
          keyCaught (std::move (onKey)),
          hosted (plugin),
          editor (plugin.createEditorAndMakeActive())
    {
        setOpaque (true);

        if (editor == nullptr)
            return;

        addAndMakeVisible (*editor);
        setSize (editor->getWidth(), editor->getHeight());
        getOpen().push_back (this);
#if JUCE_WINDOWS
        installHook();
#else
        forEachComponent (*this, [this] (juce::Component& component) { component.addKeyListener (this); });
#endif
    }

    ~Holder() override
    {
        std::erase (getOpen(), this);
        forEachComponent (*this, [this] (juce::Component& component) { component.removeKeyListener (this); });

        // The plug-in told first, as whoever deletes an editor tells it (AudioProcessorEditor's destructor asks for it): else
        // a processor that makes its own editor (the tests' stand-ins) kept it as its active one and made none again, so
        // that its window, closed, never opened again. JUCE's hosted VST3 window tells it itself, which this repeats
        // harmlessly.
        if (editor != nullptr)
            hosted.editorBeingDeleted (editor.get());

        editor = nullptr; // before the plug-in can go
    }

    bool isOpen() const noexcept { return editor != nullptr; }
    bool isResizable() const { return editor != nullptr && editor->isResizable(); }

    void place (std::int64_t window, juce::Rectangle<int> area)
    {
        if (editor == nullptr)
            return;

        if (window == 0 || area.isEmpty())
        {
            setVisible (false);
            return;
        }

        // A child of Phazite's window, made again for another.
        if (window != parent)
        {
            removeFromDesktop();
            parent = window;
            addToDesktop (0, reinterpret_cast<void*> ((juce::pointer_sized_int) window));

#if JUCE_WINDOWS
            // What happens in it isn't told to Phazite's window it is in (a click, its window destroyed), which Windows would
            // send to Phazite's thread and wait for: the helper waits for nothing of Phazite's (known bug 165). Set again for
            // each native window JUCE makes, which it does here alone. JUCE workaround W51 (docs/juce-workarounds.md).
            if (auto* peer = getPeer())
            {
                auto* handle = static_cast<HWND> (peer->getNativeHandle());
                SetWindowLongPtrW (handle, GWL_EXSTYLE, GetWindowLongPtrW (handle, GWL_EXSTYLE) | WS_EX_NOPARENTNOTIFY);
            }
#endif
        }

        // Its place in its own units, which its window counts in the screen's pixels; as large as Phazite's area where the
        // editor can be resized, else as large as the editor, which Phazite's area is within a pixel's rounding of.
        const auto scale = getPeer() != nullptr ? getPeer()->getPlatformScaleFactor() : 1.0;
        const auto placed = (area.toDouble() / scale).toNearestInt();
        setBounds (isResizable() ? placed : placed.withSize (editor->getWidth(), editor->getHeight()));
        setVisible (true);
    }

    void setKeys (bool all, std::vector<juce::KeyPress> wanted, std::vector<std::pair<int, int>> wantedPlaces)
    {
        getsEveryKey = all;
        keys = std::move (wanted);
        places = std::move (wantedPlaces);
    }

    // By its place where it has one (its scan code), as Phazite matched it in its own layout, and then by that alone: the
    // code, as this helper's layout names the key, may mean another key to Phazite's, whose layout may be another (one
    // switched to while the plug-in's window had the keys), and taken so the key would reach neither (the second review of
    // known bug 177). By its code where it has no place (Space, the arrows, a key Phazite's layout has no character at).
    bool takes (const juce::KeyPress& key, int scanCode = 0) const
    {
        if (getsEveryKey)
            return false;

        const auto isPlace = [scanCode] (const auto& place) { return scanCode != 0 && place.first == scanCode; };

        if (std::any_of (places.begin(), places.end(), isPlace))
            return std::any_of (places.begin(), places.end(), [&key, &isPlace] (const auto& place)
                                { return isPlace (place) && place.second == key.getModifiers().getRawFlags(); });

        return std::find (keys.begin(), keys.end(), key) != keys.end();
    }

    void tellKey (bool isDown, const juce::KeyPress& key, int scanCode) { keyCaught (isDown, key, scanCode); }

    // The open editor a native window is in, if any.
    static Holder* findHolder ([[maybe_unused]] void* nativeWindow)
    {
#if JUCE_WINDOWS
        for (auto* holder : getOpen())
            if (auto* peer = holder->getPeer())
                for (auto window = static_cast<HWND> (nativeWindow); window != nullptr; window = GetParent (window))
                    if (window == static_cast<HWND> (peer->getNativeHandle()))
                        return holder;
#endif

        return nullptr;
    }

private:
    std::function<void (int, int)> resizedByPlugin;
    std::function<void (bool, const juce::KeyPress&, int)> keyCaught;
    juce::AudioPluginInstance& hosted;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::int64_t parent = 0;
    bool getsEveryKey = false, keyHeld = false;
    std::vector<juce::KeyPress> keys;
    std::vector<std::pair<int, int>> places;

    static std::vector<Holder*>& getOpen()
    {
        static std::vector<Holder*> open;
        return open;
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }

    void resized() override
    {
        if (isResizable())
            editor->setBounds (getLocalBounds());
    }

    // The plug-in resized its editor: the window follows, and Phazite is told.
    void childBoundsChanged (juce::Component* child) override
    {
        if (child != editor.get() || (editor->getWidth() == getWidth() && editor->getHeight() == getHeight()))
            return;

        setSize (editor->getWidth(), editor->getHeight());
        resizedByPlugin (editor->getWidth(), editor->getHeight());
    }

    // Away from Windows: the keys as its components hear them.
    using juce::Component::keyPressed;
    using juce::Component::keyStateChanged;

    bool keyPressed (const juce::KeyPress& key, juce::Component*) override
    {
        if (! takes (key))
            return false;

        keyHeld = true;
        keyCaught (true, key, 0);
        return true;
    }

    bool keyStateChanged (bool isDown, juce::Component*) override
    {
        if (! isDown && keyHeld && ! getsEveryKey)
        {
            keyHeld = false;
            keyCaught (false, {}, 0);
        }

        return false;
    }

#if JUCE_WINDOWS
    // A hook on the messages the message thread takes from its queue, before they go to their windows: a plug-in's own
    // native windows included, which JUCE doesn't see. Made once, with the first editor.
    struct Hook
    {
        Hook()
            : hook (SetWindowsHookExW (WH_GETMESSAGE, &Hook::getMessage, nullptr, GetCurrentThreadId()))
        {
        }

        ~Hook()
        {
            if (hook != nullptr)
                UnhookWindowsHookEx (hook);
        }

        static LRESULT CALLBACK getMessage (int code, WPARAM removed, LPARAM message)
        {
            if (code == HC_ACTION && removed == PM_REMOVE)
                filter (*reinterpret_cast<MSG*> (message));

            return CallNextHookEx (nullptr, code, removed, message);
        }

        static void filter (MSG& message)
        {
            const auto isDown = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
            const auto isUp = message.message == WM_KEYUP || message.message == WM_SYSKEYUP;

            if (! (isDown || isUp) || isTextField (GetFocus()))
                return;

            auto* holder = findHolder (message.hwnd);
            const auto key = toKeyPress ((UINT) message.wParam);

            // Its place: the scan code of a key that isn't an extended one (a place of the keyboard's main block).
            const auto scanCode = (message.lParam & (1 << 24)) != 0 ? 0 : (int) ((message.lParam >> 16) & 0xff);

            if (holder == nullptr || ! holder->takes (key, scanCode))
                return;

            // Phazite gets the key, and the plug-in doesn't.
            holder->tellKey (isDown, key, scanCode);
            message.message = WM_NULL;
        }

        // A native text field, where Space and letters are typed.
        static bool isTextField (HWND window)
        {
            wchar_t className[64] = {};

            if (window == nullptr || GetClassNameW (window, className, 64) == 0)
                return false;

            return juce::String (className).containsIgnoreCase ("edit");
        }

        // The key as JUCE names it, with the modifier keys held now: Space by its code, and a key that types a character
        // by its unshifted character (as Phazite's own hook names it: ui/input/PluginKeys.cpp).
        static juce::KeyPress toKeyPress (UINT virtualKey)
        {
            const auto modifiers = juce::ModifierKeys::getCurrentModifiersRealtime().withoutMouseButtons();

            if (virtualKey == VK_SPACE)
                return { juce::KeyPress::spaceKey, modifiers, 0 };

            const auto character = (int) (MapVirtualKeyW (virtualKey, MAPVK_VK_TO_CHAR) & 0x7fff); // the top bit marks dead keys
            return { character != 0 ? character : (int) virtualKey, modifiers, 0 };
        }

        HHOOK hook = nullptr;
    };

    static void installHook() { static Hook hook; }
#endif
};

Editor::Editor (juce::AudioPluginInstance& plugin, std::function<void (int, int)> resized,
                std::function<void (bool, const juce::KeyPress&, int)> key)
    : holder (std::make_unique<Holder> (plugin, std::move (resized), std::move (key)))
{
}

Editor::~Editor() = default;

bool Editor::isOpen() const noexcept
{
    return holder->isOpen();
}

juce::Rectangle<int> Editor::getBounds() const
{
    return holder->getLocalBounds();
}

bool Editor::isResizable() const
{
    return holder->isResizable();
}

void Editor::place (std::int64_t window, juce::Rectangle<int> area)
{
    holder->place (window, area);
}

void Editor::setKeys (bool getsEveryKey, std::vector<juce::KeyPress> keys, std::vector<std::pair<int, int>> places)
{
    holder->setKeys (getsEveryKey, std::move (keys), std::move (places));
}
} // namespace phazite::host
