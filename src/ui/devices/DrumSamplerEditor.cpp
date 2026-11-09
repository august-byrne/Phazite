// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DrumSamplerEditor.h"
#include "model/notes/NoteValues.h"
#include "ui/input/LibraryDrag.h"
#include "ui/theme/Icons.h"
#include "ui/theme/LookAndFeel.h"

#include <array>
#include <cmath>
#include <iterator>
#include <limits>

namespace phazite::ui
{
namespace
{
    using devices::DrumSampler;
    using Field = DrumSampler::Field;

    constexpr int columns = 4, shownPads = 16, rows = DrumSampler::numPads / columns;
    constexpr int padWidth = 52, tallestPad = 60, shortestPad = 36; // each pad's width, and the most and least height it takes
    constexpr int mapWidth = 22;                                    // the map of all the pads, four across
    constexpr int waveformHeight = 100, shortestWaveform = 36;
    constexpr float lightFall = 0.08f; // of a pad's full light, per refresh: about 0.4 s to go out
    constexpr std::array handleFields { Field::start, Field::attack, Field::hold, Field::decay };

    std::string_view getID (int pad, Field field)
    {
        return DrumSampler::info.parameters[(size_t) DrumSampler::getParameterIndex (pad, field)].id;
    }

    // A pad as titles name it: "Pad 36 (C2)", its note and the note's name.
    juce::String getPadTitle (int pad)
    {
        return "Pad " + juce::String (pad) + " (" + juce::MidiMessage::getMidiNoteName (pad, true, true, 4) + ")";
    }

    juce::Colour getPadColour (int pad)
    {
        return juce::Colour (partColours[(size_t) pad % std::size (partColours)]);
    }

    // The drum General MIDI puts on a pad's note, which an empty pad shows faintly; none off its map.
    juce::String getGeneralMidiDrum (int pad)
    {
        return pad >= 35 && pad <= 81 ? getDrumName (pad) : juce::String();
    }
} // namespace

//==============================================================================
class DrumSamplerEditor::Pad : public juce::Button,
                               public juce::DragAndDropTarget,
                               public juce::FileDragAndDropTarget
{
public:
    explicit Pad (DrumSamplerEditor& e)
        : juce::Button ("Pad"),
          editor (e)
    {
        setTooltip ("Plays the pad and shows its controls; drop a sound on it to play that instead");
        setWantsKeyboardFocus (true);
        onClick = [this] { editor.play (pad); };
    }

    int getPadNumber() const noexcept { return pad; }

    void setPad (int newPad)
    {
        if (newPad == pad)
            return;

        pad = newPad;
        setName (getPadTitle (pad));
        setTitle (getPadTitle (pad));
        brightness = 0.0f;
        repaint();
    }

    // What it shows: what it plays (nothing for an empty pad), its choke group (0 for none), and whether it is muted or
    // soloed.
    void show (const juce::String& sound, int chokeGroup, bool isMuted, bool isSoloed)
    {
        if (sound == shownName && chokeGroup == choke && isMuted == muted && isSoloed == soloed)
            return;

        shownName = sound;
        choke = chokeGroup;
        muted = isMuted;
        soloed = isSoloed;
        setDescription (sound);
        repaint();
    }

    // Lights up when hit, and goes out slowly.
    void light (float hit)
    {
        const auto shown = std::max (hit, brightness - lightFall);

        if (std::abs (shown - brightness) > 0.001f)
        {
            brightness = std::max (0.0f, shown);
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto& palette = getPalette (*this);
        const auto area = getLocalBounds().toFloat().reduced (1.0f);
        const auto empty = shownName.isEmpty();
        auto fill = palette.surfaceRaised.interpolatedWith (
            getPadColour (pad), empty ? 0.0f : GraphTint::partCurve + (GraphTint::padLit - GraphTint::partCurve) * brightness);

        if (highlighted || down)
            fill = fill.brighter (0.1f);

        g.setColour (muted ? fill.withMultipliedAlpha (SwitchTint::partOff) : fill);
        g.fillRoundedRectangle (area, cornerRadius);

        // Outlined while it is selected, a sound is dropped on it, or it is being swapped.
        const auto lit = pad == editor.selectedPad || dropping || pad == editor.swappedPad;
        g.setColour (lit || hasKeyboardFocus (false) ? palette.accent : palette.border);
        g.drawRoundedRectangle (area, cornerRadius, lit ? 2.0f : 1.0f);

        // Its note and choke group at the top, M and S at the bottom, and what it plays: an empty pad, faintly, the drum
        // General MIDI puts there.
        const auto inside = area.reduced ((float) Space::xs);
        g.setFont (juce::FontOptions (TextSize::small));
        g.setColour (palette.textMuted);
        g.drawText (juce::String (pad), inside, juce::Justification::topLeft);

        if (choke > 0)
            g.drawText ("C" + juce::String (choke), inside, juce::Justification::topRight);

        if (muted)
            g.drawText ("M", inside, juce::Justification::bottomLeft);

        if (soloed)
        {
            g.setColour (palette.soloed);
            g.drawText ("S", inside, juce::Justification::bottomRight);
        }

        g.setColour (empty ? palette.textMuted.withMultipliedAlpha (SwitchTint::partOff) : palette.text);
        g.drawFittedText (empty ? getGeneralMidiDrum (pad) : shownName, inside.toNearestInt(), juce::Justification::centred, 2);
    }

    // From the browser: a sound file.
    bool isInterestedInDragSource (const SourceDetails& details) override { return getDraggedFile (details.description).has_value(); }
    void itemDragEnter (const SourceDetails&) override { setDropping (true); }
    void itemDragExit (const SourceDetails&) override { setDropping (false); }

    void itemDropped (const SourceDetails& details) override
    {
        setDropping (false);

        if (const auto file = getDraggedFile (details.description); file.has_value() && editor.dropFile (pad, *file))
            markPlaced (details.description);
    }

    // From Explorer.
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return files.size() == 1; }
    void fileDragEnter (const juce::StringArray&, int, int) override { setDropping (true); }
    void fileDragExit (const juce::StringArray&) override { setDropping (false); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        setDropping (false);
        editor.dropFile (pad, juce::File (files[0]));
    }

private:
    DrumSamplerEditor& editor;
    int pad = -1;
    juce::String shownName;
    float brightness = 0.0f;
    int choke = 0;
    bool muted = false, soloed = false, dropping = false;

    void setDropping (bool now)
    {
        dropping = now;
        repaint();
    }
};

//==============================================================================
// Every pad, four across, the lowest at the bottom: those that play a sound filled, lit as they play, the selected one
// outlined, the sixteen shown framed. A click or a drag shows the rows around the pointer, the arrow keys a row more.
class DrumSamplerEditor::PadMap : public juce::Component,
                                  public juce::SettableTooltipClient
{
public:
    explicit PadMap (DrumSamplerEditor& e)
        : editor (e)
    {
        setTitle ("Pads Shown");
        setDescription ("Every pad, the lowest at the bottom: those that play a sound filled. Click or drag to show others.");
        setTooltip ("Every pad, the lowest at the bottom, those that play a sound filled: click or drag to show others");
        setWantsKeyboardFocus (true);
    }

    // Which pads play a sound, and how hard each was hit since the last look.
    void show (const std::array<bool, DrumSampler::numPads>& sounding, const std::array<float, DrumSampler::numPads>& hits)
    {
        auto changed = sounding != filled;
        filled = sounding;

        for (size_t pad = 0; pad < lights.size(); ++pad)
        {
            const auto shown = std::max (0.0f, std::max (hits[pad], lights[pad] - lightFall));
            changed = changed || std::abs (shown - lights[pad]) > 0.001f;
            lights[pad] = shown;
        }

        if (changed)
            repaint();
    }

    juce::Rectangle<float> getCell (int pad) const
    {
        const auto area = getLocalBounds().toFloat().reduced (1.0f);
        const auto width = area.getWidth() / columns, height = area.getHeight() / rows;
        return { area.getX() + (float) (pad % columns) * width, area.getBottom() - (float) (pad / columns + 1) * height, width, height };
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        g.setColour (p.background);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius);

        for (int pad = 0; pad < DrumSampler::numPads; ++pad)
        {
            const auto light = lights[(size_t) pad];

            if (filled[(size_t) pad] || light > 0.0f)
            {
                g.setColour (p.textMuted.withMultipliedAlpha (SwitchTint::partOff).interpolatedWith (getPadColour (pad), light));
                g.fillRect (getCell (pad).reduced (0.5f));
            }
        }

        g.setColour (p.accent);
        g.drawRect (getCell (editor.selectedPad), 1.0f);

        const auto shown = getCell (editor.firstShown).getUnion (getCell (editor.firstShown + shownPads - 1));
        g.setColour (hasKeyboardFocus (false) ? p.focus : p.text);
        g.drawRect (shown.expanded (0.5f), 1.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override { showAround (e.position.y); }
    void mouseDrag (const juce::MouseEvent& e) override { showAround (e.position.y); }

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto rowsMoved = key == juce::KeyPress::upKey         ? 1
                               : key == juce::KeyPress::downKey     ? -1
                               : key == juce::KeyPress::pageUpKey   ? shownPads / columns
                               : key == juce::KeyPress::pageDownKey ? -shownPads / columns
                                                                    : 0;

        if (rowsMoved == 0 || key.getModifiers().isAnyModifierKeyDown())
            return false;

        editor.showPadsFrom (editor.firstShown + rowsMoved * columns);
        return true;
    }

    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }

private:
    DrumSamplerEditor& editor;
    std::array<bool, DrumSampler::numPads> filled {};
    std::array<float, DrumSampler::numPads> lights {};

    // The four rows around the one under the pointer: it and the one under it, and two over it.
    void showAround (float y)
    {
        const auto area = getLocalBounds().toFloat().reduced (1.0f);
        const auto row = (int) std::floor ((area.getBottom() - y) / (area.getHeight() / (float) rows));
        editor.showPadsFrom ((row - 1) * columns);
    }
};

//==============================================================================
// The sixteen pads shown and the map beside them; the mouse wheel over them shows others, a row a notch.
class DrumSamplerEditor::PadsPart : public juce::Component
{
public:
    explicit PadsPart (DrumSamplerEditor& e)
        : editor (e),
          map (e)
    {
        for (auto& pad : pads)
        {
            pad = std::make_unique<Pad> (e);
            addAndMakeVisible (*pad);
        }

        addAndMakeVisible (map);
    }

    PadMap& getMap() noexcept { return map; }

    // The sixteen from a pad, and their switches, which they show.
    void showFrom (int first)
    {
        for (int i = 0; i < shownPads; ++i)
        {
            pads[(size_t) i]->setPad (first + i);
            chokes[(size_t) i] = editor.device.getParameter (getID (first + i, Field::choke));
            mutes[(size_t) i] = editor.device.getParameter (getID (first + i, Field::mute));
            solos[(size_t) i] = editor.device.getParameter (getID (first + i, Field::solo));
        }

        map.repaint();
    }

    Pad* find (int pad) const
    {
        for (const auto& each : pads)
            if (each->getPadNumber() == pad)
                return each.get();

        return nullptr;
    }

    void repaintPads()
    {
        for (auto& each : pads)
            each->repaint();

        map.repaint();
    }

    // Each pad shows its first layer's sound (or else its first that plays one), and how many more it has: "Snare +1".
    void refresh (const std::array<int, DrumSampler::numPads>& layers, const std::array<float, DrumSampler::numPads>& hits)
    {
        for (size_t i = 0; i < pads.size(); ++i)
        {
            auto& pad = *pads[i];
            const auto number = pad.getPadNumber();
            const auto count = layers[(size_t) number];
            auto name = editor.device.getPadName (number);

            for (int layer = 1; layer < DrumSampler::numLayers && name.isEmpty() && count > 0; ++layer)
                name = editor.device.getPadName (number, layer);

            pad.show (count > 1 ? name + " +" + juce::String (count - 1) : name, juce::roundToInt (chokes[i].getValue()),
                      mutes[i].getValue() > 0.5f, solos[i].getValue() > 0.5f);
            pad.light (hits[(size_t) number]);
        }

        std::array<bool, DrumSampler::numPads> sounding {};

        for (size_t each = 0; each < sounding.size(); ++each)
            sounding[each] = layers[each] > 0;

        map.show (sounding, hits);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        map.setBounds (area.removeFromRight (mapWidth));
        area.removeFromRight (Space::xs);
        const auto width = (area.getWidth() - (columns - 1) * Space::xs) / columns;
        const auto height = (area.getHeight() - (columns - 1) * Space::xs) / columns;

        // The lowest at the bottom left, the highest at the top right.
        for (int i = 0; i < shownPads; ++i)
        {
            const auto row = columns - 1 - i / columns, column = i % columns;
            pads[(size_t) i]->setBounds (area.getX() + column * (width + Space::xs), area.getY() + row * (height + Space::xs), width,
                                         height);
        }
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        notches += (wheel.isReversed ? -wheel.deltaY : wheel.deltaY) / wheelNotch;
        const auto steps = (int) notches;
        notches -= steps;

        if (steps != 0)
            editor.showPadsFrom (editor.firstShown + steps * columns);
    }

private:
    DrumSamplerEditor& editor;
    PadMap map;
    std::array<std::unique_ptr<Pad>, shownPads> pads;
    std::array<ParameterHandle, shownPads> chokes, mutes, solos;
    double notches = 0.0; // a trackpad's movements, added up until they make a row
};

//==============================================================================
// The selected pad's sound as it plays (reversed, from its end), with its envelope over it: the part it plays, as loud as
// the envelope lets it, solid, the rest faint; the start a line, the attack's end and the hold's end handles at the top
// and the decay's at the bottom, each dragged sideways as one undo step; and where it is as it plays.
class DrumSamplerEditor::Waveform : public juce::Component
{
public:
    explicit Waveform (DrumSamplerEditor& e)
        : editor (e)
    {
        setTitle ("Sound");
        setDescription ("The pad's sound as it plays, with its envelope over it. Drag the start, the attack's end, the hold's end "
                        "or the decay's end sideways.");
    }

    // The pad shown, its sound and its values: drawn again when one of them changes.
    void show (int pad)
    {
        if (pad != shownPad)
        {
            shownPad = pad;

            for (size_t i = 0; i < handleFields.size(); ++i)
                handles[i] = editor.device.getParameter (getID (pad, handleFields[i]));

            for (const auto field : { Field::reverse, Field::tune, Field::fine })
                others[(size_t) field] = editor.device.getParameter (getID (pad, field));

            drawn = {};
            repaint();
        }

        auto* drums = editor.device.getProcessor<DrumSampler>();
        const auto* sound = drums != nullptr ? drums->getPadSound (pad) : nullptr;
        const auto reversed = others[(size_t) Field::reverse].getValue() > 0.5f;

        if (sound != shownSound || reversed != shownReversed)
        {
            shownSound = sound;
            shownReversed = reversed;
            measure();
            repaint();
        }

        // Its values, which place the handles and how long it plays (its tuning).
        std::array<float, handleFields.size() + 2> now {};

        for (size_t i = 0; i < handleFields.size(); ++i)
            now[i] = handles[i].getValue();

        now[handleFields.size()] = others[(size_t) Field::tune].getValue();
        now[handleFields.size() + 1] = others[(size_t) Field::fine].getValue();

        if (now != drawn)
        {
            drawn = now;
            repaint();
        }

        if (const auto played = drums != nullptr ? drums->getPlayhead (pad).get() : -1.0f; ! juce::approximatelyEqual (played, playhead))
        {
            playhead = played;
            repaint();
        }
    }

    // Where a handle is drawn: the start's on its line halfway down, the attack's and the hold's at the top, the decay's at the
    // bottom.
    juce::Point<float> getHandlePoint (Field field) const
    {
        const auto area = getArea();
        const auto y = field == Field::start   ? area.getCentreY()
                       : field == Field::decay ? area.getBottom() - GraphTint::handleRadius
                                               : area.getY() + GraphTint::handleRadius;
        return { getHandleX (field), y };
    }

    // Where a handle is: the start, or the end of the attack, the hold or the decay.
    float getHandleX (Field field) const
    {
        const auto area = getArea();

        // Not a switch: a pad has many fields besides these, which GCC's -Wswitch-enum would want named.
        if (field == Field::start)
            return xFor (getStartSeconds(), area);

        if (field == Field::attack)
            return xFor (getAttackEnd(), area);

        if (field == Field::hold)
            return xFor (getHoldEnd(), area);

        if (field == Field::decay)
            return xFor (getHoldEnd() + getValue (Field::decay) * 0.001, area);

        return area.getX();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto colour = getPadColour (std::max (0, shownPad));
        g.setColour (p.background);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius);
        const auto area = getArea();

        if (shownSound == nullptr)
        {
            g.setColour (p.textMuted);
            g.setFont (juce::FontOptions (TextSize::small));
            g.drawText ("No sound: drop one on the pad", area, juce::Justification::centred, true);
            return;
        }

        // The sound mirrored about the middle, faint; the part it plays, as loud as its envelope lets it, solid.
        juce::Path whole, played;
        const auto stretch = area.getWidth() / (float) std::max<size_t> (1, peaks.size());

        for (size_t i = 0; i < peaks.size(); ++i)
        {
            const auto x = area.getX() + (float) i * stretch;
            const auto height = std::min (1.0f, peaks[i]) * area.getHeight() * 0.5f;
            const auto heard = height * getEnvelope (secondsAt (x + 0.5f * stretch, area));
            whole.addRectangle (x, area.getCentreY() - height, std::max (1.0f, stretch), 2.0f * height);

            if (heard > 0.0f)
                played.addRectangle (x, area.getCentreY() - heard, std::max (1.0f, stretch), 2.0f * heard);
        }

        g.setColour (colour.withAlpha (GraphTint::partCurve));
        g.fillPath (whole);
        g.setColour (colour);
        g.fillPath (played);

        // The envelope: from the start up the attack, along the hold, then down the decay.
        juce::Path envelope;
        const auto top = area.getY(), bottom = area.getBottom();
        const auto startX = getHandleX (Field::start), attackX = getHandleX (Field::attack), holdX = getHandleX (Field::hold);
        envelope.startNewSubPath (startX, bottom);
        envelope.lineTo (attackX, top);
        envelope.lineTo (holdX, top);

        if (! isWhole())
            for (auto x = holdX; x < area.getRight(); x += 2.0f)
                envelope.lineTo (x, bottom - (bottom - top) * getEnvelope (secondsAt (x, area)));

        g.setColour (p.text);
        g.strokePath (envelope, juce::PathStrokeType (1.5f));
        g.drawLine (startX, top, startX, bottom, 1.5f);

        for (const auto field : handleFields)
        {
            if (field == Field::decay && isWhole())
                continue;

            g.fillEllipse (juce::Rectangle<float> (2.0f * GraphTint::handleRadius, 2.0f * GraphTint::handleRadius)
                               .withCentre (getHandlePoint (field)));
        }

        // Where it is, as it plays.
        if (playhead >= 0.0f)
        {
            const auto x = area.getX() + playhead * area.getWidth();
            g.drawLine (x, top, x, bottom, 1.5f);
        }

        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (juce::String (getLength(), 2) + " s", area.reduced ((float) Space::xs), juce::Justification::bottomRight, false);
    }

    // The handle nearest the press, which the drag moves; of two in one place, the later (the hold's over the attack's).
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragged = -1;

        if (shownSound == nullptr)
            return;

        auto nearest = std::numeric_limits<float>::max();

        for (size_t i = 0; i < handleFields.size(); ++i)
        {
            if (handleFields[i] == Field::decay && isWhole())
                continue;

            if (const auto distance = e.position.getDistanceFrom (getHandlePoint (handleFields[i])); distance <= nearest)
            {
                nearest = distance;
                dragged = (int) i;
            }
        }

        if (dragged >= 0)
        {
            handles[(size_t) dragged].beginGesture();
            mouseDrag (e);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragged < 0)
            return;

        const auto area = getArea();
        const auto at = secondsAt (e.position.x, area);
        auto& handle = handles[(size_t) dragged];
        const auto range = handle.getRange();
        auto value = 0.0f;

        const auto field = handleFields[(size_t) dragged];

        if (field == Field::start)
        {
            value = (float) (at / std::max (0.001, getLength()));
        }
        else if (field == Field::attack)
        {
            value = (float) ((at - getStartSeconds()) * 1000.0);
        }
        else if (field == Field::hold)
        {
            // Dragged to the end, it plays the whole sound.
            value = e.position.x >= area.getRight() - 0.02f * area.getWidth() ? DrumSampler::wholeSound
                                                                              : (float) ((at - getAttackEnd()) * 1000.0);
            value = std::min (value, DrumSampler::wholeSound);
        }
        else if (field == Field::decay)
        {
            value = (float) ((at - getHoldEnd()) * 1000.0);
        }

        handle.setValue (juce::jlimit (range.start, range.end, value));
        show (shownPad); // its handles where it is now, for the next press too
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragged >= 0)
            handles[(size_t) dragged].endGesture();

        dragged = -1;
    }

private:
    DrumSamplerEditor& editor;
    int shownPad = -1;
    const DrumSampler::Sound* shownSound = nullptr;
    bool shownReversed = false;
    double soundSeconds = 0.0; // at its own rate
    std::vector<float> peaks;
    float playhead = -1.0f;
    std::array<ParameterHandle, handleFields.size()> handles;
    std::array<ParameterHandle, DrumSampler::fieldsPerPad> others; // by field: reverse, tune and fine
    std::array<float, handleFields.size() + 2> drawn {};
    int dragged = -1; // the handle dragged, by its place in handleFields

    juce::Rectangle<float> getArea() const { return getLocalBounds().toFloat().reduced (2.0f); }

    float getValue (Field field) const
    {
        for (size_t i = 0; i < handleFields.size(); ++i)
            if (handleFields[i] == field)
                return drawn[i];

        return 0.0f;
    }

    // How long it plays, at its tuning, and where it starts, its attack ends and its hold ends, in seconds.
    double getLength() const
    {
        const auto semitones = (double) drawn[handleFields.size()] + drawn[handleFields.size() + 1] / 100.0;
        return soundSeconds / std::exp2 (semitones / 12.0);
    }

    bool isWhole() const { return getValue (Field::hold) >= DrumSampler::wholeSound; }
    double getStartSeconds() const { return getValue (Field::start) * getLength(); }
    double getAttackEnd() const { return getStartSeconds() + getValue (Field::attack) * 0.001; }
    double getHoldEnd() const { return isWhole() ? getLength() : getAttackEnd() + getValue (Field::hold) * 0.001; }

    float xFor (double seconds, juce::Rectangle<float> area) const
    {
        return area.getX() + (float) (juce::jlimit (0.0, 1.0, seconds / std::max (0.001, getLength()))) * area.getWidth();
    }

    double secondsAt (float x, juce::Rectangle<float> area) const
    {
        return (double) ((x - area.getX()) / std::max (1.0f, area.getWidth())) * getLength();
    }

    // The envelope's level at a time: nothing before the start, up the attack, along the hold, then down the decay to -60 dB.
    float getEnvelope (double seconds) const
    {
        if (seconds < getStartSeconds())
            return 0.0f;

        if (seconds < getAttackEnd())
            return (float) ((seconds - getStartSeconds()) / std::max (1.0e-6, getAttackEnd() - getStartSeconds()));

        if (seconds < getHoldEnd() || isWhole())
            return 1.0f;

        const auto decay = std::max (0.001, getValue (Field::decay) * 0.001);
        const auto level = std::pow (0.001, (seconds - getHoldEnd()) / decay);
        return level < 0.001 ? 0.0f : (float) level;
    }

    // The loudest of each stretch of the sound as it plays, for a few hundred of them.
    void measure()
    {
        peaks.clear();
        soundSeconds = 0.0;

        if (shownSound == nullptr)
            return;

        constexpr int stretches = 400;
        const auto& audio = shownSound->audio;
        const auto frames = audio.getNumSamples();
        soundSeconds = frames / std::max (1.0, shownSound->sampleRate);
        peaks.assign ((size_t) stretches, 0.0f);

        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        {
            const auto* samples = audio.getReadPointer (channel);

            for (int i = 0; i < frames; ++i)
            {
                const auto played = shownReversed ? frames - 1 - i : i;
                auto& peak = peaks[(size_t) std::min (stretches - 1, (int) ((juce::int64) played * stretches / std::max (1, frames)))];
                peak = std::max (peak, std::abs (samples[i]));
            }
        }
    }
};

//==============================================================================
// The selected pad's name with Reverse, Swap and Reset, its sound drawn, and the controls of its start and envelope.
class DrumSamplerEditor::SoundPart : public juce::Component
{
public:
    SoundPart (DrumSamplerEditor& e, ParameterControls& c)
        : editor (e),
          waveform (e),
          controls (c)
    {
        name.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
        setThemeColour (name, juce::Label::textColourId, &Palette::text);
        addAndMakeVisible (name);

        reverseButton.setClickingTogglesState (true);
        reverseButton.setTooltip ("Plays the pad's sound backwards, from its end");
        addAndMakeVisible (reverseButton);

        resetButton.setTooltip ("Puts the kit's own sound back on the pad");
        resetButton.onClick = [this]
        {
            editor.device.clearPadSound (editor.selectedPad);
            editor.refresh();
        };
        addAndMakeVisible (resetButton);

        swapButton.setClickingTogglesState (false);
        swapButton.onClick = [this]
        {
            if (editor.onSwap != nullptr)
                editor.onSwap (editor.selectedPad);
        };
        addChildComponent (swapButton); // while onSwap is set

        addAndMakeVisible (waveform);
        addAndMakeVisible (controls);
    }

    Waveform& getWaveform() noexcept { return waveform; }
    juce::TextButton& getSwapButton() noexcept { return swapButton; }

    void showPad (int pad)
    {
        reverseAttachment.setParameter (editor.device.getParameter (getID (pad, Field::reverse)));
        reverseButton.setTitle ("Pad " + juce::String (pad) + " Reverse");
    }

    void refresh()
    {
        const auto pad = editor.selectedPad;
        const auto sound = editor.device.getPadName (pad);
        const auto text = getPadTitle (pad) + ": " + (sound.isNotEmpty() ? sound : juce::String ("Empty"));
        name.setText (text, juce::dontSendNotification);
        name.setTitle (text);
        resetButton.setEnabled (editor.device.hasPadSound (pad));
        reverseAttachment.update();

        // Swap, where something can swap; lit on the pad being swapped.
        if (const auto canSwap = editor.onSwap != nullptr; canSwap != swapButton.isVisible())
        {
            swapButton.setVisible (canSwap);
            resized();
        }

        swapButton.setToggleState (editor.swappedPad >= 0 && editor.swappedPad == pad, juce::dontSendNotification);
        swapButton.setTooltip (swapButton.getToggleState()
                                   ? "Swapping this pad's sound: click to keep the one on it, or press Esc in the browser to put the "
                                     "old one back"
                                   : "Tries the browser's sounds on the pad, in the beat: move to each with the arrow keys; Enter keeps "
                                     "one, Esc puts this one back");
        waveform.show (pad);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto top = area.removeFromTop (controlHeight);
        const auto place = [&top] (juce::TextButton& button)
        {
            button.setBounds (top.removeFromRight (button.getBestWidthForHeight (controlHeight)));
            top.removeFromRight (Space::xs);
        };

        place (resetButton);

        if (swapButton.isVisible())
            place (swapButton);

        place (reverseButton);
        top.removeFromRight (Space::xs);
        name.setBounds (top);
        area.removeFromTop (Space::xs);
        controls.setBounds (area.removeFromBottom (controls.getPreferredHeight()));
        area.removeFromBottom (Space::xs);
        waveform.setBounds (area);
    }

private:
    DrumSamplerEditor& editor;
    juce::Label name;
    juce::TextButton reverseButton { "Reverse" }, swapButton { "Swap" }, resetButton { "Reset" };
    ToggleAttachment reverseAttachment { reverseButton };
    Waveform waveform;
    ParameterControls& controls;
};

//==============================================================================
// One of the selected pad's layers: its number and what it plays, or where a sound can go; a sound dropped on it, from the
// browser or Explorer, plays there, and Clear takes it off (the first layer's is the pad's own, which Reset puts back).
class DrumSamplerEditor::LayerSlot : public juce::Component,
                                     public juce::DragAndDropTarget,
                                     public juce::FileDragAndDropTarget
{
public:
    LayerSlot (DrumSamplerEditor& e, int index)
        : editor (e),
          layer (index)
    {
        setTitle ("Layer " + juce::String (layer + 1));
        clearButton.setQuiet (true);
        clearButton.setTooltip ("Takes this sound off the pad");
        clearButton.onClick = [this]
        {
            editor.device.clearPadSound (editor.selectedPad, layer);
            editor.refresh();
        };
        addChildComponent (clearButton);
    }

    void show (const juce::String& sound)
    {
        if (sound == shownName)
            return;

        shownName = sound;
        setDescription (sound);
        clearButton.setVisible (layer > 0 && sound.isNotEmpty());
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto area = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (p.background);
        g.fillRoundedRectangle (area, cornerRadius);
        g.setColour (dropping ? p.accent : p.border);
        g.drawRoundedRectangle (area, cornerRadius, dropping ? 2.0f : 1.0f);

        const auto inside = area.reduced ((float) Space::xs);
        g.setFont (juce::FontOptions (TextSize::small));
        g.setColour (p.textMuted);
        g.drawText (juce::String (layer + 1), inside, juce::Justification::topLeft);
        g.setColour (shownName.isEmpty() ? p.textMuted.withMultipliedAlpha (SwitchTint::partOff) : p.text);
        g.drawFittedText (shownName.isEmpty() ? juce::String ("Drop a sound") : shownName,
                          inside.withTrimmedTop (TextSize::small).toNearestInt(), juce::Justification::centred, 2);
    }

    void resized() override
    {
        const auto size = juce::roundToInt (TextSize::small) + Space::xs;
        clearButton.setBounds (getLocalBounds().removeFromTop (size).removeFromRight (size));
    }

    // From the browser: a sound file.
    bool isInterestedInDragSource (const SourceDetails& details) override { return getDraggedFile (details.description).has_value(); }
    void itemDragEnter (const SourceDetails&) override { setDropping (true); }
    void itemDragExit (const SourceDetails&) override { setDropping (false); }

    void itemDropped (const SourceDetails& details) override
    {
        setDropping (false);

        if (const auto file = getDraggedFile (details.description); file.has_value() && editor.dropFile (editor.selectedPad, *file, layer))
            markPlaced (details.description);
    }

    // From Explorer.
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return files.size() == 1; }
    void fileDragEnter (const juce::StringArray&, int, int) override { setDropping (true); }
    void fileDragExit (const juce::StringArray&) override { setDropping (false); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        setDropping (false);
        editor.dropFile (editor.selectedPad, juce::File (files[0]), layer);
    }

private:
    DrumSamplerEditor& editor;
    const int layer;
    juce::String shownName;
    bool dropping = false;
    IconButton clearButton { "Clear Layer " + juce::String (layer + 1), Icon::close };

    void setDropping (bool now)
    {
        dropping = now;
        repaint();
    }
};

//==============================================================================
// The selected pad's layers: how they share its hits beside them, each one's sound over its level.
class DrumSamplerEditor::LayersPart : public juce::Component
{
public:
    LayersPart (DrumSamplerEditor& e, ParameterControls& modeControl, ParameterControls& levelControls)
        : editor (e),
          mode (modeControl),
          levels (levelControls)
    {
        for (int layer = 0; layer < DrumSampler::numLayers; ++layer)
        {
            slots[(size_t) layer] = std::make_unique<LayerSlot> (e, layer);
            addAndMakeVisible (*slots[(size_t) layer]);
        }

        addAndMakeVisible (mode);
        addAndMakeVisible (levels);
    }

    LayerSlot& getSlot (int layer) const { return *slots[(size_t) juce::jlimit (0, DrumSampler::numLayers - 1, layer)]; }

    static int getSlotHeight() { return 2 * juce::roundToInt (TextSize::small) + 2 * Space::xs; }

    int getPreferredWidth() const { return mode.getPreferredWidth() + Space::xs + levels.getPreferredWidth(); }
    int getPreferredHeight() const
    {
        return std::max (mode.getPreferredHeight(), getSlotHeight() + Space::xs + levels.getPreferredHeight());
    }

    void refresh()
    {
        for (int layer = 0; layer < DrumSampler::numLayers; ++layer)
            slots[(size_t) layer]->show (editor.device.getPadName (editor.selectedPad, layer));
    }

    void resized() override
    {
        auto area = getLocalBounds();
        mode.setBounds (area.removeFromLeft (mode.getPreferredWidth()).withHeight (mode.getPreferredHeight()));
        area.removeFromLeft (Space::xs);
        levels.setBounds (area.removeFromBottom (levels.getPreferredHeight()));
        area.removeFromBottom (Space::xs);
        auto row = area.removeFromBottom (getSlotHeight());
        const auto width = row.getWidth() / DrumSampler::numLayers;

        for (auto& slot : slots)
            slot->setBounds (row.removeFromLeft (width).reduced (Space::xs / 2, 0));
    }

private:
    DrumSamplerEditor& editor;
    ParameterControls &mode, &levels;
    std::array<std::unique_ptr<LayerSlot>, DrumSampler::numLayers> slots;
};

//==============================================================================
// The sampler's output, which every pad goes through but those on tracks of their own; and Own Track, lit while the
// selected pad has one, which a click gives it or takes away.
class DrumSamplerEditor::OutputPart : public juce::Component
{
public:
    OutputPart (DrumSamplerEditor& e, ParameterControls& levelControl)
        : editor (e),
          level (levelControl)
    {
        addAndMakeVisible (level);
        ownTrack.setClickingTogglesState (false);
        ownTrack.onClick = [this]
        {
            editor.device.setPadOnOwnTrack (editor.selectedPad, ! ownTrack.getToggleState());
            editor.refresh();
        };
        addAndMakeVisible (ownTrack);
    }

    juce::Button& getOwnTrackButton() noexcept { return ownTrack; }

    int getPreferredWidth() { return std::max (level.getPreferredWidth(), ownTrack.getBestWidthForHeight (controlHeight)); }
    int getPreferredHeight() const { return level.getPreferredHeight() + Space::xs + controlHeight; }

    void refresh()
    {
        const auto onOwnTrack = editor.device.getPadTrack (editor.selectedPad).isValid();
        ownTrack.setTitle ("Pad " + juce::String (editor.selectedPad) + " on Its Own Track");

        if (onOwnTrack == ownTrack.getToggleState() && ownTrack.getTooltip().isNotEmpty())
            return;

        ownTrack.setToggleState (onOwnTrack, juce::dontSendNotification);
        ownTrack.setTooltip (onOwnTrack ? "The pad plays on a track of its own, after this one, with its own effects, sends and fader: "
                                          "click to bring it back here, deleting that track"
                                        : "Gives the pad a track of its own, after this one: a mixer channel with its own effects, sends "
                                          "and fader, which other tracks' sidechains can listen to");
    }

    void resized() override
    {
        auto area = getLocalBounds();
        level.setBounds (area.removeFromTop (level.getPreferredHeight()));
        area.removeFromTop (Space::xs);
        ownTrack.setBounds (area.removeFromTop (controlHeight));
    }

private:
    DrumSamplerEditor& editor;
    ParameterControls& level;
    juce::TextButton ownTrack { "Own Track" };
};

//==============================================================================
DrumSamplerEditor::DrumSamplerEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle),
      DisplayTicker::Display (static_cast<juce::Component&> (*this))
{
    padsPart = &keep (std::make_unique<PadsPart> (*this));
    padsPart->showFrom (firstShown);

    auto& soundControls = makePadControls ({ Field::start, Field::attack, Field::hold, Field::decay, Field::mode }, 5,
                                           { "Start", "Attack", "Hold", "Decay", "Mode" });
    soundPart = &keep (std::make_unique<SoundPart> (*this, soundControls));

    const auto padRows = columns * padWidth + (columns - 1) * Space::xs + Space::xs + mapWidth;
    const DeviceLayout::Part pads {
        "Pads", padsPart, padRows, columns * tallestPad + (columns - 1) * Space::xs, columns * shortestPad + (columns - 1) * Space::xs,
        false
    };
    const auto below = 2 * Space::xs + controlHeight + soundControls.getPreferredHeight();
    const DeviceLayout::Part sound {
        "Sound", soundPart, soundControls.getPreferredWidth(), waveformHeight + below, shortestWaveform + below, true
    };

    const auto pad = makePadPart ("Pad", { Field::level, Field::pan, Field::velocity, Field::choke, Field::mute, Field::solo }, 6,
                                  { "Level", "Pan", "Velocity", "Choke", "Mute", "Solo" });
    const auto pitch =
        makePadPart ("Pitch", { Field::tune, Field::fine, Field::sweep, Field::sweepTime }, 4, { "Tune", "Fine", "Sweep", "Sweep Time" });
    const auto filter = makePadPart ("Filter", { Field::filter, Field::cutoff, Field::resonance, Field::velocityToCutoff }, 4,
                                     { "Type", "Cutoff", "Resonance", "Velocity" });
    auto& outputLevel = makeControls ({ DrumSampler::info.parameters[(size_t) DrumSampler::outputParameter].id }, 1,
                                      ParameterControls::Size::compact, {}, { "Level" });
    outputPart = &keep (std::make_unique<OutputPart> (*this, outputLevel));
    const DeviceLayout::Part output { "Output", outputPart, outputPart->getPreferredWidth(), outputPart->getPreferredHeight(),
                                      outputPart->getPreferredHeight() };

    auto& layerMode = makePadControls ({ Field::layerMode }, 1, { "Mode" });
    auto& layerLevels = makePadControls ({ Field::layer1Level, Field::layer2Level, Field::layer3Level, Field::layer4Level }, 4,
                                         { "Layer 1", "Layer 2", "Layer 3", "Layer 4" });
    layersPart = &keep (std::make_unique<LayersPart> (*this, layerMode, layerLevels));
    const DeviceLayout::Part layers { "Layers", layersPart, layersPart->getPreferredWidth(), layersPart->getPreferredHeight(),
                                      layersPart->getPreferredHeight() };

    setParts ({ { pads }, { sound }, { pad, pitch }, { layers, filter }, { output } });
    getLayout().setPinned (0);

    // The pads' first readings would light them for what played before the editor opened.
    if (auto* drums = device.getProcessor<DrumSampler>())
        for (int each = 0; each < DrumSampler::numPads; ++each)
            drums->getPadMeter (each).take();

    selectPad (selectedPad);
}

DrumSamplerEditor::~DrumSamplerEditor() = default;

ParameterControls& DrumSamplerEditor::makePadControls (std::vector<Field> fields, int numColumns, const std::vector<juce::String>& names)
{
    std::vector<std::string_view> ids;

    for (const auto field : fields)
        ids.push_back (getID (selectedPad, field));

    auto& made = makeControls (std::move (ids), numColumns, ParameterControls::Size::compact, {}, names);
    padControls.emplace_back (&made, std::move (fields));
    return made;
}

DeviceLayout::Part DrumSamplerEditor::makePadPart (const juce::String& title, std::vector<Field> fields, int numColumns,
                                                   const std::vector<juce::String>& names)
{
    auto& made = makePadControls (std::move (fields), numColumns, names);
    return { title, &made, made.getPreferredWidth(), made.getPreferredHeight(), made.getPreferredHeight() };
}

juce::Button* DrumSamplerEditor::getPad (int pad)
{
    return padsPart->find (pad);
}

juce::Component& DrumSamplerEditor::getPadMap() const
{
    return padsPart->getMap();
}

juce::Button& DrumSamplerEditor::getSwapButton() noexcept
{
    return soundPart->getSwapButton();
}

juce::Button& DrumSamplerEditor::getOwnTrackButton() noexcept
{
    return outputPart->getOwnTrackButton();
}

juce::Component& DrumSamplerEditor::getWaveform() const
{
    return soundPart->getWaveform();
}

float DrumSamplerEditor::getHandleX (Field field) const
{
    return soundPart->getWaveform().getHandleX (field);
}

void DrumSamplerEditor::selectPad (int pad)
{
    selectedPad = juce::jlimit (0, DrumSampler::numPads - 1, pad);

    for (const auto& [shown, fields] : padControls)
    {
        std::vector<std::string_view> ids;

        for (const auto field : fields)
            ids.push_back (getID (selectedPad, field));

        shown->showParameters (device, ids);
    }

    soundPart->showPad (selectedPad);

    // Shown among the sixteen.
    if (selectedPad < firstShown)
        showPadsFrom (selectedPad);
    else if (selectedPad >= firstShown + shownPads)
        showPadsFrom (selectedPad - shownPads + columns);

    padsPart->repaintPads();
    refresh();
}

void DrumSamplerEditor::showPadsFrom (int pad)
{
    const auto first = juce::jlimit (0, DrumSampler::numPads - shownPads, pad - ((pad % columns) + columns) % columns);

    if (first == firstShown && padsPart->find (first) != nullptr)
        return;

    firstShown = first;
    padsPart->showFrom (first);
    refresh();
}

void DrumSamplerEditor::play (int pad)
{
    selectPad (pad);

    if (auto* drums = device.getProcessor<DrumSampler>())
        drums->audition (pad, 1.0f);
}

bool DrumSamplerEditor::dropFile (int pad, const juce::File& file, int layer)
{
    selectPad (pad);
    const auto put = device.setPadSound (pad, file, layer);
    refresh();
    return put;
}

juce::Component& DrumSamplerEditor::getLayerSlot (int layer) const
{
    return layersPart->getSlot (layer);
}

void DrumSamplerEditor::refresh()
{
    // How hard each pad was hit since the last look, and how many of its layers play a sound.
    std::array<float, DrumSampler::numPads> hits {};
    std::array<int, DrumSampler::numPads> layers {};

    if (auto* drums = device.getProcessor<DrumSampler>())
    {
        for (int pad = 0; pad < DrumSampler::numPads; ++pad)
        {
            hits[(size_t) pad] = drums->getPadMeter (pad).take();

            for (int layer = 0; layer < DrumSampler::numLayers; ++layer)
                layers[(size_t) pad] += drums->getPadSound (pad, layer) != nullptr ? 1 : 0;
        }
    }

    if (const auto swapped = device.getSwap().value_or (-1); swapped != swappedPad)
    {
        swappedPad = swapped;
        padsPart->repaintPads();
    }

    padsPart->refresh (layers, hits);
    soundPart->refresh();
    layersPart->refresh();
    outputPart->refresh();
}
} // namespace phazite::ui
