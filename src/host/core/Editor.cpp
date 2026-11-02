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
            std::function<void (bool, const juce::KeyPress&)> onKey)
        : resizedByPlugin (std::move (onResized)),
          keyCaught (std::move (onKey)),
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
        }

        // Its place in its own units, which its window counts in the screen's pixels.
        const auto scale = getPeer() != nullptr ? getPeer()->getPlatformScaleFactor() : 1.0;
        setBounds ((area.toDouble() / scale).toNearestInt());
        setVisible (true);
    }

    void setKeys (bool all, std::vector<juce::KeyPress> wanted)
    {
        getsEveryKey = all;
        keys = std::move (wanted);
    }

    bool takes (const juce::KeyPress& key) const { return ! getsEveryKey && std::find (keys.begin(), keys.end(), key) != keys.end(); }

    void tellKey (bool isDown, const juce::KeyPress& key) { keyCaught (isDown, key); }

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
    std::function<void (bool, const juce::KeyPress&)> keyCaught;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::int64_t parent = 0;
    bool getsEveryKey = false, keyHeld = false;
    std::vector<juce::KeyPress> keys;

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
        keyCaught (true, key);
        return true;
    }

    bool keyStateChanged (bool isDown, juce::Component*) override
    {
        if (! isDown && keyHeld && ! getsEveryKey)
        {
            keyHeld = false;
            keyCaught (false, {});
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

            if (holder == nullptr || ! holder->takes (key))
                return;

            // Phazite gets the key, and the plug-in doesn't.
            holder->tellKey (isDown, key);
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
                std::function<void (bool, const juce::KeyPress&)> key)
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

void Editor::setKeys (bool getsEveryKey, std::vector<juce::KeyPress> keys)
{
    holder->setKeys (getsEveryKey, std::move (keys));
}
} // namespace phazite::host
