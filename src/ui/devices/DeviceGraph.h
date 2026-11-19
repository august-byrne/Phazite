// SPDX-License-Identifier: AGPL-3.0-or-later
// What devices' displays share (docs/design.md, section 9): a graph's axes (frequency, level) and grid, curves, a spectrum
// of what a device plays, the last seconds of a level, and the gestures their handles make: a drag or a turn of the wheel,
// each one undo step.
#pragma once

#include "devices/core/SampleTap.h"
#include "devices/core/Spectrum.h"
#include "model/devices/ParameterHandle.h"
#include "ui/components/WheelBurst.h"
#include "ui/theme/DesignTokens.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace phazite::ui
{
class TypedField; // ui/components/TypedField.h: the field a value is typed in
} // namespace phazite::ui

namespace phazite::ui::graph
{
// Frequencies across an area, on a log scale: 20 Hz to 20 kHz unless given others.
struct FrequencyAxis
{
    juce::Rectangle<float> area;
    double low = 20.0, high = 20000.0;

    float xFor (double frequency) const;
    double frequencyAt (float x) const;
};

// Levels up an area, in decibels.
struct LevelAxis
{
    juce::Rectangle<float> area;
    float low = -60.0f, high = 0.0f;

    float yFor (float decibels) const;
    float levelAt (float y) const;
};

// A graph's background, rounded; and its grid: a line at 100 Hz, 1 kHz and 10 kHz (and the ones between, fainter), and one
// every step decibels, with their numbers where asked.
void paintBackground (juce::Graphics&, const Palette&, juce::Rectangle<float> area);
void paintFrequencyGrid (juce::Graphics&, const Palette&, const FrequencyAxis&, bool withNumbers);
void paintLevelGrid (juce::Graphics&, const Palette&, const LevelAxis&, float step, bool withNumbers);

// A curve across an area, y for each x, every step pixels.
juce::Path makeCurve (juce::Rectangle<float> area, const std::function<float (float x)>& yAt, float step = 1.5f);

// A short text in a corner of a graph, such as what the pointer is over ("1.2 kHz  +3.0 dB").
void paintReadout (juce::Graphics&, const Palette&, juce::Rectangle<float> area, const juce::String&, juce::Justification);

// Two readouts along a graph's top, at its left and its right: the right one only where both fit, so that on a graph narrowed to
// its least they never overlap.
void paintReadouts (juce::Graphics&, const Palette&, juce::Rectangle<float> area, const juce::String& left, const juce::String& right);

// "1.2 kHz", "350 Hz"; "+3.0 dB", "-12 dB".
juce::String formatFrequency (double hertz);
juce::String formatDecibels (float decibels);

//==============================================================================
// The spectrum of a device's sound (devices/core/Spectrum.h), as an analyzer shows it: a filled area and a line, each
// level tilted up 4.5 dB an octave around 1 kHz, as analyzers show music, so that a mix looks about flat; each pixel the
// loudest of the frequencies it spans, and each level falling 40 dB a second, as Pro-Q's and SPAN's do by default, so that
// a clap flashes up and falls away within a beat or two.
//
// One trace for both channels, as Pro-Q's and Ableton's Spectrum's (L+R) are, but each channel analyzed apart and each level
// the louder of the two (known bug 120): a sound in the middle reads as a sum's average does, at its level, and a sound panned
// hard reads at its level too, where an average read it 6 dB low, and one out of phase between the channels shows, where an
// average cancelled it to nothing. Or a trace of one way through the sound alone, as an EQ that plays a band on it shows its
// curve's: the middle, (L + R) / 2, the side, (L - R) / 2, the left or the right, each analyzed from the same samples (a
// tap gives both channels; the middle and the side are worked out from them as they are pulled), and only while asked for.
//
// Every analyzer's levels go from -90 dB at the bottom to +6 dB at the top (getLevels()): a sound can be over full scale
// inside a chain.
class SpectrumTrace
{
public:
    SpectrumTrace();

    // What a trace can show: the louder of the two channels, the middle, the side, the left or the right.
    enum class Channels
    {
        louder,
        middle,
        side,
        left,
        right
    };

    void setSampleRate (double);

    // The ways it analyzes, besides the louder of the two channels (which the left's and the right's give): asked for, the
    // middle's and the side's start from silence; no longer asked for, they stop (made the first time, on the message thread).
    void analyze (bool middleAndSide);

    // Takes the newest samples (each channel apart), analyzing them as they come; call at each refresh, then update(), which
    // shows them and returns whether the trace moved.
    void pull (devices::StereoTap&);
    void pull (devices::SampleTap&);
    bool update();

    // Afresh: what it has analyzed goes, every level at the floor (a display shown again: DisplayTicker::Display::shownAgain()).
    void clear();

    void paint (juce::Graphics&, const FrequencyAxis&, const LevelAxis&, juce::Colour fill, juce::Colour line,
                Channels = Channels::louder) const;
    float getLevelAt (double frequency, Channels = Channels::louder) const;         // tilted, in decibels
    float getLevelOver (double from, double to, Channels = Channels::louder) const; // the loudest between two, tilted at their middle

    static constexpr float tiltPerOctave = 4.5f;
    static constexpr float fallPerSecond = 40.0f; // decibels

    // Every analyzer's levels up an area.
    static constexpr float lowest = -90.0f, highest = 6.0f;
    static LevelAxis getLevels (juce::Rectangle<float> area) { return { area, lowest, highest }; }

private:
    devices::Spectrum left, right;
    std::unique_ptr<devices::Spectrum> middle, side; // while asked for
    bool middleAndSideShown = false;
    double sampleRate = 48000.0;
    std::vector<float> lefts, rights, ways;

    void add (int numPulled);
    float getRawLevelOver (double from, double to, Channels) const;
};

//==============================================================================
// The last readings of a value, for a display that scrolls: one a reading that comes at an even rate (Age's pitch), or one a
// refresh with the moment it was taken, each the loudest of what came since the one before (a delay's peaks), so that what it
// shows is where it was in time, however late a refresh came.
class LevelHistory
{
public:
    explicit LevelHistory (int length, float initialValue);

    void push (float);
    void push (float, double milliseconds); // by juce::Time::getMillisecondCounterHiRes()
    int size() const noexcept { return (int) values.size(); }
    float get (int index) const noexcept;      // from the oldest (0) to the newest (size() - 1)
    double getTime (int index) const noexcept; // when it was pushed, the same way; 0 for none

    // The reading that covers a moment: the oldest taken at it or after (each covers the time since the one before); -1 for
    // a moment before them all or after the newest.
    int findAt (double milliseconds) const noexcept;

    // Afresh: every reading this value, none with a time (a display shown again).
    void clear (float value);

    // Its values, oldest first, after the numbers there: what a display draws from (DrawnNumbers, DisplayTicker.h).
    void appendTo (std::vector<float>& numbers) const;

    // Across an area, the oldest at the left: a line through the values, or the area under it down to the bottom.
    juce::Path makeLine (juce::Rectangle<float> area, const std::function<float (float value)>& yFor) const;
    juce::Path makeArea (juce::Rectangle<float> area, const std::function<float (float value)>& yFor, float baseY) const;

private:
    std::vector<float> values;
    std::vector<double> times;
    int next = 0;
};

//==============================================================================
// A handle of a graph, as the graph finds it at a place (HandleDrag::findHandle): the parameters a drag of it moves (the first
// names the undo step), the one the wheel turns over it (none for nothing), where it is (the field a value is typed in opens
// over it), and the graph's own items for its menu, after the parameters' sections (an EQ band's types). A handle that is a
// thing rather than its parameters' values (a sampler's slice) takes a double-click as its graph says. More parameters its
// menu and readout show, after those (an EQ band's threshold beside its Dynamic); and more a double-click puts back with
// those it moves (an EQ band's Q with its frequency and gain).
struct Handle
{
    Handle (std::vector<ParameterHandle*> moving, juce::Point<float> place, ParameterHandle* turning = nullptr)
        : parameters (std::move (moving)),
          wheel (turning),
          at (place)
    {
    }

    std::vector<ParameterHandle*> parameters;
    ParameterHandle* wheel = nullptr;
    juce::Point<float> at;
    std::function<void (juce::PopupMenu&)> addItems;
    bool revertsOnDoubleClick = true;
    std::vector<ParameterHandle*> alsoShown, alsoReverted;
};

// What a handle moves, by name and value as its knob writes it, from the values set by hand: "Cutoff 1.2 kHz  Resonance 40%";
// the words its names begin with alike said once ("Band 4  Frequency 800 Hz  Gain +3.0 dB").
juce::String describe (const std::vector<ParameterHandle*>&);

// How far a drag moves a handle for each pixel the pointer goes, and a notch of the wheel turns it: a fifth as far while Shift
// is held, as on a knob (fineDragFactor).
float getFineFactor (const juce::ModifierKeys&);

//==============================================================================
// A drag of a graph's handles, which every graph's drag goes through, so that each takes a handle the same way (known bugs
// 116, 117 and 140), and the grammar every graph's handles keep, as a knob does (docs/design/devices.md, "A graph's gestures"):
//   - A drag moves a handle as far as the pointer goes, a fifth as far while Shift is held (getMoved()); a double-click puts
//     back the defaults of what it moves (revert()); a right-click shows the menu of each parameter it moves (getMenu()),
//     whose Type a Value... opens the field a knob has, over the handle (typeValue()); the wheel over it turns what it says,
//     and elsewhere goes on to what is around the graph (passWheelOn()); and while it is under the pointer or dragged, its
//     values show in the graph's corner (getReadout()) and the pointer's tooltip says what it does (getTooltip()). A graph
//     that keeps the grammar says where its handles are (findHandle); the drag hears the graph's mouse after the graph for
//     the double-click, the right-click and the pointer's place.
//   - A press takes the handles it moves (take()), each from its value set by hand (ParameterHandle::getBaseValue()), not from
//     the value it plays with its modulation, which the graph draws. It begins their gesture, as a knob's press does, so that
//     a handle held still while the song plays grabs its parameter in Touch and Latch; but it changes no value, and a gesture
//     that changed nothing leaves no undo step and no unsaved change.
//   - The drag moves each from there by as far as the pointer has gone since the press (moveAlong()), so that a handle taken
//     at its edge doesn't jump to the pointer, as Ableton Live's and FabFilter's handles don't; an axis the pointer hasn't
//     moved along keeps its value exactly, and a value is written only when it changes (a tremor under a pixel writes
//     nothing). The drag is one undo step, named after the first handle taken, which the others join; the mouse going up
//     keeps it (end()).
//   - Escape in the middle takes back all it did, leaving nothing to redo, as in Ableton Live and most DAWs (and as
//     ParameterHandle::cancelGesture() does for a knob), and the drag does nothing more until the mouse goes up. It hears
//     Escape through a key listener on the graph's window, from the press until the mouse goes up, so that a graph is no
//     stop for the keyboard's focus; and a click on one leaves the keys where they were (setMouseClickGrabsKeyboardFocus()).
//   - The graph's wheels (WheelGesture) are its too: a press ends a turn of the wheel that is still one undo step, so that
//     the drag is a step of its own (it ends every one: WheelBurst::endAll()), and the wheel turns nothing while a drag is
//     taken.
// The handles are the graph's, and must outlive the drag (make it a member declared after them).
class WheelGesture;

class HandleDrag : private juce::KeyListener,
                   private juce::MouseListener
{
public:
    explicit HandleDrag (juce::Component& graph);
    ~HandleDrag() override;

    // A press takes these handles (none, for a drag that changes the song only as it ends, which Escape still takes back).
    void take (std::vector<ParameterHandle*>);
    bool isTaken() const noexcept { return taken; } // from take() until end() or Escape
    float getStart (size_t handle) const;           // its value set by hand as the press took it

    // How far the pointer has gone since the press, for the handles to move by: a fifth as far while Shift is held, which may be
    // pressed or let go in the middle without a jump. In the graph's coordinates, from the graph's own drags.
    juce::Point<float> getMoved (const juce::MouseEvent&);

    // A value for a handle taken, kept to its range; nothing is written when it is the value the handle has.
    void set (size_t handle, float value);

    // A handle moved from its value set by hand at the press, along one of the graph's axes: from where that value is
    // (placeOf, in the graph's pixels), as far as the pointer has moved along it, and back to a value (valueAt); moved by
    // nothing, it keeps its value as the press took it, exactly.
    void moveAlong (size_t handle, float moved, const std::function<float (float value)>& placeOf,
                    const std::function<float (float place)>& valueAt);

    void end();    // the mouse went up: keeps what the drag did
    bool cancel(); // Escape: takes it back; whether there was a drag to take back

    // After Escape took the drag back, for the graph to show the values from before (and forget what it was dragging); and after
    // a double-click or a value typed set a handle's values, for the graph to show them.
    std::function<void()> onCancelled;

    // Where the graph's handles are: the one at a place in the graph's coordinates, or none there (empty space). Unset, the
    // graph takes its double-clicks, right-clicks and wheel itself.
    std::function<std::optional<Handle> (juce::Point<float>)> findHandle;
    std::optional<Handle> getHandleAt (juce::Point<float>) const;

    // A double-click on a handle: the defaults of the parameters it moves, exactly, as one undo step named as its drag's is.
    void revert (const std::vector<ParameterHandle*>&);

    // A right-click on a handle: a section for each parameter it moves, as a knob's menu has (makeParameterMenu(), Type a
    // Value... opening the field over the handle), then the graph's own items. Empty where there is no handle.
    juce::PopupMenu getMenu (const Handle&);
    juce::PopupMenu getMenuAt (juce::Point<float>);

    // The field a knob opens to type a value in, over a place in the graph: what is typed, read in the parameter's units, is set
    // as one undo step. The field while it is open, else nullptr.
    void typeValue (ParameterHandle&, juce::Point<float> at);
    juce::TextEditor* getValueField() const noexcept;

    // The wheel where the graph turns nothing: to what is around it, as a component without a wheel of its own passes it on (a
    // view that scrolls).
    void passWheelOn (const juce::MouseEvent&, const juce::MouseWheelDetails&);

    // What the handle dragged, or else the one under the pointer, moves, by name and value (describe()), for the graph's corner;
    // empty over empty space. And the pointer's tooltip over a handle: what it moves and how, and what the wheel turns there
    // ("Wheel: Drive"); empty elsewhere, where the graph's description says what it is.
    juce::String getReadout() const;
    juce::String getTooltip() const;

    // The drag of a graph, for the tests, which right-click, read and turn its handles through it.
    static HandleDrag* findOf (const juce::Component& graph);

private:
    friend class WheelGesture;

    juce::Component& graph;
    juce::Component::SafePointer<juce::Component> window; // whose keys it hears while a drag is taken
    std::vector<ParameterHandle*> handles;
    std::vector<float> starts;
    bool taken = false;
    bool movedFromPress = false;                 // getMoved() has heard the drag since the press
    juce::Point<float> lastPosition, movedSoFar; // where the pointer was last, and how far the handles have gone
    std::optional<juce::Point<float>> pointer;   // over the graph, where; none while it is elsewhere
    juce::String shownReadout;                   // as the graph last showed it, to repaint as it changes
    std::unique_ptr<TypedField> valueField;      // Type a Value...'s, while it is open and until a message after it closed

    void listen (bool shouldListen);
    void letGo(); // forgets the handles, and stops listening
    void changed();
    void pointAt (std::optional<juce::Point<float>>);
    bool keyPressed (const juce::KeyPress&, juce::Component*) override;

    void mouseEnter (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    JUCE_DECLARE_NON_COPYABLE (HandleDrag)
};

//==============================================================================
// The wheel turning a parameter, such as an EQ band's Q, as one undo step for a burst of it (WheelBurst: until it rests for a
// moment, a press, or the pointer leaving the graph). The handle turned is the caller's, and must outlive this (make it a
// member declared after the handles). A turn starts from the value set by hand (ParameterHandle::getBaseValue()), as a drag
// does. It is its graph's, whose drag (HandleDrag, declared before it) ends it as a press takes a handle, and while a drag
// is taken it turns nothing.
class WheelGesture
{
public:
    explicit WheelGesture (HandleDrag& graphsDrag);

    void turn (ParameterHandle&, float newValue);
    void end() { burst.end(); }

private:
    HandleDrag& drag;
    ParameterHandle* turning = nullptr;
    WheelBurst burst; // after what it ends

    JUCE_DECLARE_NON_COPYABLE (WheelGesture)
};
} // namespace phazite::ui::graph
