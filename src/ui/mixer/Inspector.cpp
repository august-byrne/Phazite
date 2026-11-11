// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Inspector.h"
#include "StripParts.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/SongSelection.h"
#include "model/arrangement/TimelineModel.h"
#include "model/automation/AutomationModel.h"
#include "model/chords/ChordModel.h"
#include "model/commands/SongGesture.h"
#include "model/mixer/MixerModel.h"
#include "model/notes/NotesModel.h"
#include "model/transport/TempoModel.h"
#include "model/transport/TransportModel.h"
#include "ui/components/Controls.h"
#include "ui/components/TrackSwitch.h"
#include "ui/components/ValueText.h"
#include "ui/devices/DeviceMenus.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/CommandUI.h"
#include "ui/input/TextFieldRelease.h"
#include "ui/playlist/ChordLane.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace phazite::ui
{
using namespace strip;

namespace
{
    constexpr int nameWidth = 84;         // a row's name, on its left
    constexpr int stackWidth = 140;       // a control needs this much beside its name, or the name goes above it
    constexpr int stackedNameHeight = 16; // a name above its control
    constexpr int titleHeight = 32;
    constexpr int tipLines = 3; // a section's tip, wrapped

    // "Bar 5", or "Bar 5, beat 3" for a place after a bar's first beat, its beats its time signature's (eighths in 6/8); bars
    // count from 1, as the time line's do.
    juce::String describePlace (const ProjectContext& context, BeatPosition place)
    {
        const auto bar = getBarAt (context, place); // from 0
        const auto barStart = getBarStart (context, bar);
        const auto intoBar = (place - barStart) / getBeatLength (context, barStart); // in its beats

        if (intoBar < 0.001)
            return "Bar " + juce::String (bar + 1);

        return "Bar " + juce::String (bar + 1) + ", beat "
               + juce::String (1.0 + intoBar, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    }

    // "2 bars", "1 bar, 2 beats" or "3 beats", in the time signature where it starts: its bars and its beats.
    juce::String describeLength (const ProjectContext& context, BeatRange range)
    {
        const auto length = range.getLength();
        const auto bar = getBarLength (context, range.getStart());
        const auto bars = (int) std::floor (length / bar + 0.0001);
        const auto rest = std::max (0.0, (length - bar * (double) bars) / getBeatLength (context, range.getStart()));
        const auto restText = rest < 0.001 ? juce::String() : juce::String (rest, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
        const auto barsText = bars == 1 ? juce::String ("1 bar") : juce::String (bars) + " bars";

        const auto beatsWord = std::abs (rest - 1.0) < 0.001 ? " beat" : " beats";

        if (bars == 0)
            return restText + beatsWord;

        return restText.isEmpty() ? barsText : barsText + ", " + restText + beatsWord;
    }

    juce::String getModeName (AutomationMode mode)
    {
        switch (mode)
        {
            case AutomationMode::touch:
                return "Touch";
            case AutomationMode::latch:
                return "Latch";
            case AutomationMode::write:
                return "Write";
            case AutomationMode::read:
            default:
                return "Read";
        }
    }

    // A color, which a click on offers the palette's others.
    struct ColourButton : public juce::Button
    {
        explicit ColourButton (const juce::String& title)
            : juce::Button (title)
        {
            setTitle (title);
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool) override
        {
            const auto area = getLocalBounds().toFloat().reduced (1.0f).withWidth ((float) controlHeight * 2.0f);
            g.setColour (colour);
            g.fillRoundedRectangle (area, cornerRadius);
            g.setColour (highlighted ? getPalette (*this).text : getPalette (*this).border);
            g.drawRoundedRectangle (area, cornerRadius, 1.0f);
        }

        juce::Colour colour;
    };

    // A row's text, in the text color, cut short with dots when it doesn't fit.
    void setUpValueLabel (juce::Label& label)
    {
        label.setFont (juce::FontOptions (TextSize::body));
        label.setMinimumHorizontalScale (1.0f);
    }

    // A track's switch for mute and solo, with what it shows in words beside it ("Playing", "Soloed").
    struct SwitchRow : public juce::Component
    {
        explicit SwitchRow (ProjectContext& context)
            : trackSwitch (context)
        {
            setUpValueLabel (state);
            state.setInterceptsMouseClicks (false, false);
            state.setAccessible (false); // the switch says it
            addAndMakeVisible (trackSwitch);
            addAndMakeVisible (state);
        }

        void setTrack (TrackID track)
        {
            trackSwitch.setTrack (track);
            update();
        }

        void update()
        {
            trackSwitch.update();
            state.setText (trackSwitch.getStateText(), juce::dontSendNotification);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            trackSwitch.setBounds (area.removeFromLeft (SwitchTint::size).withSizeKeepingCentre (SwitchTint::size, SwitchTint::size));
            state.setBounds (area);
        }

        TrackSwitch trackSwitch;
        juce::Label state;
    };

    // A single-line field that changes something when Enter is pressed or the focus leaves it. Enter and Escape let the keys
    // go back to the window's shortcuts, as a press elsewhere does (TextFieldRelease.h); each apply changes nothing when the
    // text is what is shown already.
    void setUpNameEditor (juce::TextEditor& editor, std::function<void (const juce::String&)> apply)
    {
        const auto release = [&editor]
        {
            clearSelection (editor);
            editor.giveAwayKeyboardFocus();
        };

        editor.setFont (juce::FontOptions (TextSize::body));
        editor.setSelectAllWhenFocused (true);
        editor.onReturnKey = [&editor, apply, release]
        {
            apply (editor.getText());
            release();
        };
        editor.onFocusLost = [&editor, apply] { apply (editor.getText()); };
        editor.onEscapeKey = release;
    }

    // The numbers in a text, as words and commas part them ("Bar 5, beat 3.5": 5 and 3.5).
    juce::StringArray getNumbers (const juce::String& text)
    {
        juce::StringArray numbers;
        numbers.addTokens (text.retainCharacters ("0123456789.-, ").replaceCharacter (',', ' '), " ", {});
        numbers.removeEmptyStrings();
        return numbers;
    }

    // A place typed as describePlace() writes it ("Bar 5, beat 3"), or as a bar and a beat ("5.3", "5 3.5", "5"), each
    // counted from 1. Nothing without a bar.
    std::optional<BeatPosition> parsePlace (const ProjectContext& context, const juce::String& text)
    {
        auto numbers = getNumbers (text);

        if (numbers.size() == 1 && numbers[0].containsChar ('.'))
            numbers = juce::StringArray::fromTokens (numbers[0], ".", {});

        if (numbers.isEmpty() || numbers[0].isEmpty())
            return {};

        const auto bar = std::max (1, numbers[0].getIntValue());
        const auto beat = numbers.size() > 1 ? std::max (1.0, numbers[1].getDoubleValue()) : 1.0;
        const auto barStart = getBarStart (context, bar - 1);
        return barStart + getBeatLength (context, barStart) * (beat - 1.0);
    }

    // A length typed as describeLength() writes it ("1 bar, 2 beats", "3 beats"), as a note value ("1/16", of a whole note
    // of four quarter notes, whatever the time signature), or in beats alone ("0.5"), in the time signature at a place.
    std::optional<BeatDuration> parseLength (const ProjectContext& context, const juce::String& text, BeatPosition at)
    {
        const auto lower = text.toLowerCase();

        if (lower.containsChar ('/'))
        {
            const auto over = lower.upToFirstOccurrenceOf ("/", false, false).getDoubleValue();
            const auto under = lower.fromFirstOccurrenceOf ("/", false, false).getDoubleValue();
            return over > 0.0 && under > 0.0 ? std::optional (BeatDuration::fromBeats (4.0 * over / under)) : std::nullopt;
        }

        const auto numbers = getNumbers (lower);

        if (numbers.isEmpty())
            return {};

        const auto beat = getBeatLength (context, at);

        if (! lower.contains ("bar"))
            return beat * numbers[0].getDoubleValue();

        const auto beats = numbers.size() > 1 ? numbers[1].getDoubleValue() : 0.0;
        return getBarLength (context, at) * numbers[0].getDoubleValue() + beat * beats;
    }

    // A note's name, with middle C as C4 (the keys' and the top bar's), and one typed so ("F#3", "Bb2") or as its number.
    juce::String getPitchName (int pitch)
    {
        return juce::MidiMessage::getMidiNoteName (pitch, true, true, 4);
    }

    std::optional<double> parsePitch (const juce::String& text)
    {
        const auto name = text.trim().toUpperCase();

        if (name.isEmpty())
            return {};

        if (name.containsOnly ("0123456789"))
            return (double) juce::jlimit (0, 127, name.getIntValue());

        constexpr int semitones[] = { 9, 11, 0, 2, 4, 5, 7 }; // A to G
        const auto letter = (int) name[0] - 'A';

        if (! juce::isPositiveAndBelow (letter, 7) || ! name.containsAnyOf ("0123456789"))
            return {};

        auto semitone = semitones[letter];
        auto rest = name.substring (1);

        if (rest.startsWithChar ('#'))
            ++semitone;
        else if (rest.startsWithChar ('B')) // a flat, as "Bb" reads in capitals
            --semitone;

        rest = rest.trimCharactersAtStart ("#B");
        return (double) juce::jlimit (0, 127, (rest.getIntValue() + 1) * 12 + semitone);
    }

    //==============================================================================
    // A value of one item or of several, as Logic Pro's inspector shows one for several regions: the lowest of theirs, or
    // their range while they differ ("64 to 100"). Dragged, scrolled or stepped with the arrow keys, each item changes by as
    // much as the field, by whole steps from where it was (change; a drag is one undo step); a value typed goes to set.
    struct MultiField : public NumberField
    {
        MultiField (ProjectContext& c, const juce::String& title, double minimum, double maximum, double stepSize, juce::String undoName)
            : NumberField (title, minimum, maximum, stepSize, minimum),
              context (c),
              dragName (std::move (undoName))
        {
            textFromValueFunction = [this] (double value)
            {
                const auto text = format (value);
                return highest - lowest > getStep() / 1000.0 ? text + " to " + format (value + highest - lowest) : text;
            };

            valueFromTextFunction = [this] (const juce::String& text)
            {
                typed = parse (text);
                return typed.value_or (getValue());
            };

            // A drag is one step; a value typed (which JUCE hands over as a drag's start and end) is the model's own step.
            onDragStart = [this]
            {
                dragFrom = getValue();

                if (! typed.has_value())
                    gesture = std::make_unique<SongGesture> (context, dragName);
            };

            onDragEnd = [this] { gesture = nullptr; };

            onValueChange = [this]
            {
                const auto now = getValue();

                if (const auto value = std::exchange (typed, std::nullopt))
                    set (*value);
                else
                    change (now - shown);

                shown = now;
            };
        }

        ~MultiField() override { gesture = nullptr; } // ends a drag in progress, keeping its change

        // What the items have now: their lowest value and their highest.
        void show (double low, double high)
        {
            lowest = low;
            highest = std::max (low, high);

            if (! isMouseButtonDown (true))
            {
                shown = low;
                setValue (low, juce::dontSendNotification);
            }

            updateText();
        }

        // A drag changes the items by whole steps from where they were.
        double snapValue (double attemptedValue, DragMode dragMode) override
        {
            if (dragMode == notDragging)
                return attemptedValue;

            const auto steps = std::round ((attemptedValue - dragFrom) / getStep());
            return juce::jlimit (getMinimum(), getMaximum(), dragFrom + steps * getStep());
        }

        // Only a value typed is one to set.
        void mouseDown (const juce::MouseEvent& e) override
        {
            typed.reset();
            NumberField::mouseDown (e);
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            typed.reset();
            NumberField::mouseWheelMove (e, wheel);
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            typed.reset();
            return NumberField::keyPressed (key);
        }

        std::function<juce::String (double)> format = [] (double value) { return juce::String (value); };
        std::function<std::optional<double> (const juce::String&)> parse = [] (const juce::String& text) -> std::optional<double>
        { return text.containsAnyOf ("0123456789") ? std::optional (text.getDoubleValue()) : std::nullopt; };
        std::function<void (double amount)> change = [] (double) {};
        std::function<void (double value)> set = [] (double) {};

    private:
        ProjectContext& context;
        const juce::String dragName;
        std::unique_ptr<SongGesture> gesture;
        std::optional<double> typed; // the value typed, until it is set
        double lowest = 0.0, highest = 0.0;
        double shown = 0.0;    // the lowest, as the items have it, which a change is from
        double dragFrom = 0.0; // the field's value as the drag started
    };
} // namespace

//==============================================================================
struct Inspector::Page : public juce::Component
{
    struct Row
    {
        std::unique_ptr<juce::Label> name;
        juce::Component* control = nullptr;
        int height = controlHeight;
        bool visible = true;
    };

    Page (const juce::String& kindName, juce::Colour stripe)
        : kind (kindName),
          colour (stripe)
    {
        setTitle (kindName);
        title.setFont (juce::FontOptions (TextSize::title, juce::Font::bold));
        title.setMinimumHorizontalScale (1.0f); // the inspector shows it, over the rows, which scroll
    }

    virtual void refresh() {}      // the song changed: what it shows, as it is now
    virtual void updateValues() {} // a timer: the faders' and switches' values

    // A row: its name on the left, a control filling the rest. A row without a control is a heading.
    void addRow (const juce::String& name, juce::Component* control, int height = controlHeight)
    {
        auto label = std::make_unique<juce::Label> (juce::String(), name);
        label->setFont (juce::FontOptions (control != nullptr ? TextSize::small : TextSize::body,
                                           control != nullptr ? juce::Font::plain : juce::Font::bold));
        setThemeColour (*label, juce::Label::textColourId, &Palette::textMuted);
        label->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (*label);

        if (control != nullptr)
            addAndMakeVisible (control);

        rows.push_back ({ std::move (label), control, height, true });
    }

    // Shows or hides a row, by its control: rows that don't apply (an audio clip's fades for a MIDI clip) take no room.
    void setRowVisible (juce::Component& control, bool visible)
    {
        for (auto& row : rows)
        {
            if (row.control == &control && row.visible != visible)
            {
                row.visible = visible;
                row.name->setVisible (visible);
                control.setVisible (visible);
                resized();
            }
        }
    }

    // In a narrow panel each name goes above its control, so that the control has the whole width.
    static bool isStacked (int width) { return width < 2 * nameWidth + stackWidth; }
    static bool hasNameAbove (const Row& row) { return row.control != nullptr && row.name->getText().isNotEmpty(); }

    int getPreferredHeight (int width) const
    {
        auto height = 0;

        for (const auto& row : rows)
            if (row.visible)
                height += row.height + Space::xs + (isStacked (width) && hasNameAbove (row) ? stackedNameHeight : 0);

        return height + Space::m;
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (Space::m, 0).withTrimmedBottom (Space::m);
        const auto stacked = isStacked (getWidth());

        for (auto& row : rows)
        {
            if (! row.visible)
                continue;

            if (row.control == nullptr)
            {
                row.name->setBounds (area.removeFromTop (row.height));
                area.removeFromTop (Space::xs);
                continue;
            }

            if (stacked && hasNameAbove (row))
                row.name->setBounds (area.removeFromTop (stackedNameHeight));

            auto line = area.removeFromTop (row.height);
            area.removeFromTop (Space::xs);

            if (! stacked)
                row.name->setBounds (line.removeFromLeft (nameWidth).removeFromTop (controlHeight));

            row.control->setBounds (line);
        }
    }

    const juce::String kind;
    juce::Colour colour;
    juce::Label title;
    std::vector<Row> rows;
};

namespace
{
    //==============================================================================
    // A track's send to a return: its level, off until first turned, which makes the send (question 13), and whether it
    // takes the sound before or after the fader.
    struct SendRow : public juce::Component
    {
        SendRow (ProjectContext& c, TrackID from, const SendTarget& target)
            : context (c)
        {
            showAsDecibels (level);
            level.setTooltip ("How much of this track the send feeds the return: off until you turn it. Click it and type a level, or "
                              "double-click for 0 dB");
            attachment.setParameter (getSendLevelTo (context, from, target.returnTrack));
            prePost.setTooltip ("Pre: the send takes the sound before the fader, so the fader doesn't change it. Post: after the fader");
            prePost.onClick = [this]
            {
                if (shown.send.has_value())
                    setPreFader (context, shown.send->id, ! shown.send->preFader);
            };

            remove.setTooltip ("Removes the send");
            remove.onClick = [this]
            {
                if (shown.send.has_value())
                    removeSend (context, shown.send->id);
            };

            addAndMakeVisible (level);
            addAndMakeVisible (prePost);
            addAndMakeVisible (remove);
            show (target);
        }

        void show (const SendTarget& target)
        {
            shown = target;
            level.setTitle ("Send to " + target.name);
            prePost.setButtonText (target.send.has_value() && target.send->preFader ? "Pre" : "Post");

            // A send that isn't there has nothing to switch or remove.
            prePost.setVisible (target.send.has_value());
            remove.setVisible (target.send.has_value());
        }

        void resized() override
        {
            auto area = getLocalBounds();
            remove.setBounds (area.removeFromRight (controlHeight));
            area.removeFromRight (Space::xs);
            prePost.setBounds (area.removeFromRight (44));
            area.removeFromRight (Space::xs);
            level.setBounds (area);
        }

        ProjectContext& context;
        SendTarget shown;
        Fader level { Fader::Direction::horizontal, "Send level", decibelsToFaderPosition (0.0f) };
        ParameterAttachment attachment { level };
        juce::TextButton prePost, remove { juce::String (juce::CharPointer_UTF8 ("\xc3\x97")) };
    };

    //==============================================================================
    // A track's name, color, level, routing, devices and automation.
    struct TrackPage : public Inspector::Page
    {
        TrackPage (CommandBridge& bridge, ProjectContext& c, TrackID id)
            : Page ("Track", juce::Colours::transparentBlack),
              commands (bridge),
              context (c),
              track (id),
              colourButton ("Color"),
              muteSolo (c)
        {
            setUpNameEditor (nameEditor,
                             [this] (const juce::String& text)
                             {
                                 if (const auto row = getTrackRow (context, track); row.has_value() && text != row->name)
                                     renameTrack (context, track, text);
                             });
            nameEditor.setTitle ("Name");
            nameEditor.setTooltip ("The track's name: type another and press Enter");
            colourButton.setTooltip ("The track's color, which its clips and its channel show. Click for another");
            colourButton.onClick = [this]
            {
                showColourMenu (colourButton, colourButton.colour, [&projectContext = context, id = track] (juce::Colour chosen)
                                { setTrackColours (projectContext, { id }, chosen); });
            };

            showAsDecibels (volume);
            volume.setTooltip ("The track's volume. Click it and type a level, or double-click for 0 dB; drag with Shift held for fine "
                               "steps");
            volumeAttachment.setParameter (getVolume (context, track));
            showAsPan (pan);
            pan.setTooltip ("Where the track sits between the left and right speakers. Click it and type a position (30L), or "
                            "double-click to center it");
            panAttachment.setParameter (getPan (context, track));
            muteSolo.setTrack (track);

            instrumentButton.onClick = [this] { instrumentClicked(); };
            effectsButton.setTooltip ("Shows the track's effects in the Effects tab, under the playlist, the first one selected");
            effectsButton.onClick = [this]
            {
                if (const auto effects = getEffects (context, track); ! effects.empty())
                {
                    context.getSongSelection().selectOnly (effects.front().id);
                    commands.invoke ("device.show");
                }
            };

            addSendButton.setTooltip ("Sends some of this track to a return track, which holds an effect that several tracks "
                                      "share, such as a reverb");
            addSendButton.onClick = [this] { showSendMenu(); };

            for (const auto mode : { AutomationMode::read, AutomationMode::touch, AutomationMode::latch, AutomationMode::write })
                modeBox.addItem (getModeName (mode), (int) mode + 1);

            modeBox.setTitle ("Automation mode");
            modeBox.setTooltip ("How moving the track's controls while the song plays writes their automation: Read plays it, "
                                "Touch writes while a control is held, Latch from the first move until playback stops, and "
                                "Write overwrites from when playback starts");
            modeBox.onChange = [this] { setAutomationMode (context, track, (AutomationMode) (modeBox.getSelectedId() - 1)); };
            lanesButton.setTooltip ("Shows the track's automation lanes under it in the playlist, or collapses them");
            lanesButton.onClick = [this]
            {
                context.getSongSelection().selectOnly (track);
                commands.invoke ("automation.showLanes");
            };

            outputButton.setTitle ("Output");
            outputButton.onClick = [this] { strip::showOutputMenu (context, commands, track, outputButton); };

            for (auto* label : { &kindLabel })
                setUpValueLabel (*label);

            addRow ("Name", &nameEditor);
            addRow ("Color", &colourButton);
            addRow ("Kind", &kindLabel);
            addRow ("Volume", &volume);
            addRow ("Pan", &pan);
            addRow ("Mute, solo", &muteSolo);
            addRow ("Output", &outputButton);
            addRow ("Instrument", &instrumentButton);
            addRow ("Effects", &effectsButton);
            addRow ("Automation", &modeBox);
            addRow ("Lanes", &lanesButton);
            sendsHeadingIndex = (int) rows.size();
            refresh();
        }

        void refresh() override
        {
            channel = getChannel (context, track);

            if (! channel.has_value())
                return;

            title.setText (channel->name, juce::dontSendNotification);
            colour = channel->colour;
            colourButton.colour = channel->colour;
            colourButton.repaint();

            if (! nameEditor.hasKeyboardFocus (true))
                nameEditor.setText (channel->name, juce::dontSendNotification);

            kindLabel.setText (channel->kind == ChannelKind::bus           ? "Folder: mixes its tracks"
                               : channel->kind == ChannelKind::plainFolder ? "Folder: scales its tracks' volumes"
                               : channel->kind == ChannelKind::returnTrack ? "Return: holds effects tracks share"
                                                                           : "Track",
                               juce::dontSendNotification);
            outputButton.setButtonText (channel->kind == ChannelKind::plainFolder ? juce::String ("No sound of its own")
                                        : channel->outputBus.isNotEmpty()         ? channel->outputBus
                                                                                  : juce::String ("Master"));

            // A track or a bus chooses where its sound goes; a return goes to the master, and a plain folder has no sound.
            const auto chooses = channel->kind != ChannelKind::plainFolder && canChooseOutput (context, track);
            outputButton.setEnabled (chooses);
            outputButton.setTooltip (chooses ? "Where the sound goes: to the master, which you hear, or into a bus, which the track "
                                               "goes into as a folder. Click to choose"
                                             : "Where the sound goes");

            // A track's instrument; or a button that asks for one while the track's notes make no sound.
            const auto instrument = getInstrument (context, track);
            instrumentButton.setEnabled (canHoldInstrument (context, track));
            instrumentButton.setButtonText (instrument.has_value()     ? instrument->name
                                            : channel->needsInstrument ? juce::String ("Choose instrument")
                                                                       : juce::String ("+ Instrument"));
            instrumentButton.setTooltip (instrument.has_value() ? "Shows the instrument in the Instrument tab"
                                                                : "Adds an instrument, which plays the track's notes");

            const auto effects = getEffects (context, track);
            juce::StringArray names;

            for (const auto& effect : effects)
                names.add (effect.name);

            effectsButton.setButtonText (names.isEmpty() ? juce::String ("No effects") : names.joinIntoString (", "));
            effectsButton.setEnabled (! effects.empty());

            modeBox.setSelectedId ((int) getAutomationMode (context, track) + 1, juce::dontSendNotification);
            const auto lanes = (int) getAutomationLanes (context, track).size();
            lanesButton.setButtonText (lanes == 0   ? juce::String ("Show volume lane")
                                       : lanes == 1 ? juce::String ("1 lane: show or collapse")
                                                    : juce::String (lanes) + " lanes: show or collapse");

            refreshSends();
            repaint();
        }

        void refreshSends()
        {
            // A row for every return the track can send to (tracks and returns send; folders don't).
            const auto canSend = channel->kind == ChannelKind::track || channel->kind == ChannelKind::returnTrack;
            const auto targets = canSend ? getSendTargets (context, track) : std::vector<SendTarget>();
            std::vector<TrackID> returns;

            for (const auto& target : targets)
                returns.push_back (target.returnTrack);

            if (returns == shownReturns && rows.size() > (size_t) sendsHeadingIndex)
            {
                for (size_t i = 0; i < targets.size(); ++i)
                    sendRows[(int) i]->show (targets[i]);

                return;
            }

            // The rows after the heading: a row for each return, then the button that makes a new one.
            rows.resize ((size_t) sendsHeadingIndex);
            sendRows.clear();
            shownReturns = returns;

            if (canSend)
            {
                addRow ("Sends", nullptr);

                for (const auto& target : targets)
                    addRow ("To " + target.name, sendRows.add (new SendRow (context, track, target)));

                addRow ({}, &addSendButton);
            }
            else
            {
                addSendButton.setVisible (false);
            }

            if (auto* inspector = findParentComponentOfClass<Inspector>())
                inspector->resized();
        }

        void updateValues() override
        {
            volumeAttachment.update();
            panAttachment.update();
            muteSolo.update();

            for (auto* send : sendRows)
                send->attachment.update();
        }

        void instrumentClicked()
        {
            if (const auto instrument = getInstrument (context, track))
                showDevice (context, commands, *instrument);
            else
                showInstrumentMenu (context, track, instrumentButton);
        }

        void showSendMenu()
        {
            // The returns there are have their rows above: a new one, with an effect, as Studio One makes it in one step,
            // or empty.
            juce::PopupMenu menu;
            auto& projectContext = context;
            const auto from = track;
            menu.addSectionHeader ("A new return track, for an effect that tracks share");
            menu.addItem ("With an Effect...",
                          [&projectContext, from, safeButton = juce::Component::SafePointer<juce::Component> (&addSendButton)]
                          {
                              if (safeButton != nullptr)
                                  showNewReturnMenu (projectContext, from, *safeButton);
                          });

            menu.addItem ("Empty", [&projectContext, from] { addSendToNewReturn (projectContext, from); });
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addSendButton));
        }

        CommandBridge& commands;
        ProjectContext& context;
        const TrackID track;
        std::optional<Channel> channel;
        juce::TextEditor nameEditor;
        ColourButton colourButton;
        juce::Label kindLabel;
        juce::TextButton outputButton;
        Fader volume { Fader::Direction::horizontal, "Volume", decibelsToFaderPosition (0.0f) };
        Fader pan { Fader::Direction::horizontal, "Pan", 0.0 };
        ParameterAttachment volumeAttachment { volume }, panAttachment { pan };
        SwitchRow muteSolo;
        juce::TextButton instrumentButton, effectsButton, lanesButton, addSendButton { "+ Send" };
        juce::ComboBox modeBox;
        juce::OwnedArray<SendRow> sendRows;
        std::vector<TrackID> shownReturns; // the returns the send rows are for
        int sendsHeadingIndex = 0;
    };

    //==============================================================================
    // Clips: one's name, kind, place and length, whether it plays, and whether it is an alias; an audio clip's gain, pitch
    // and fades. Several share the fields and switches, which act on all of them.
    struct ClipsPage : public Inspector::Page
    {
        ClipsPage (CommandBridge& bridge, ProjectContext& c, std::vector<ClipID> shown)
            : Page (shown.size() == 1 ? "Clip" : "Clips", juce::Colours::transparentBlack),
              commands (bridge),
              context (c),
              clips (std::move (shown))
        {
            for (auto* label : { &kindLabel, &trackLabel, &aliasLabel, &fadeInLabel, &fadeOutLabel })
                setUpValueLabel (*label);

            // Where they start and how long they are, their gain and pitch, each changed or set for all of them.
            const auto setUp = [this] (MultiField& field, ClipProperty property, const juce::String& tooltip)
            {
                field.setTooltip (tooltip);
                field.change = [this, property] (double amount) { changeClips (context, clips, property, amount); };
                field.set = [this, property] (double value) { setClipsTo (context, clips, property, value); };
            };

            setUp (startField, ClipProperty::start, "Where the clips start: drag, scroll or type a bar and a beat (5.3)");
            startField.format = [this] (double beat) { return describePlace (context, BeatPosition::fromBeats (beat)); };
            startField.parse = [this] (const juce::String& text) -> std::optional<double>
            {
                const auto place = parsePlace (context, text);
                return place.has_value() ? std::optional (place->inBeats()) : std::nullopt;
            };

            setUp (lengthField, ClipProperty::length, "How long the clips are: drag, scroll or type a length (2 bars, 1/4)");
            lengthField.format = [this] (double beats) { return describeLength (context, { start, BeatDuration::fromBeats (beats) }); };
            lengthField.parse = [this] (const juce::String& text) -> std::optional<double>
            {
                const auto length = parseLength (context, text, start);
                return length.has_value() ? std::optional (length->inBeats()) : std::nullopt;
            };

            setUp (gainField, ClipProperty::gain, "How loud the clips play: drag, scroll or type decibels");
            gainField.format = [] (double dB) { return formatDecibels (dB); };
            gainField.parse = [] (const juce::String& text) { return parseDecibels (text); };

            setUp (pitchField, ClipProperty::pitch, "How far the clips' audio is pitched up or down, in semitones, keeping its length");
            pitchField.format = [] (double semitones)
            {
                return (semitones > 0.0 ? "+" : "") + juce::String (semitones, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".")
                       + " st";
            };

            mutedToggle.setTitle ("Muted");
            mutedToggle.setTooltip ("A muted clip doesn't play");
            mutedToggle.onClick = [this] { toggleClipsMuted (context, clips); };
            makeUniqueButton.setTooltip ("Makes the clips copies of their own, so that changing them no longer changes the clips "
                                         "they were aliases of");
            makeUniqueButton.onClick = [this] { invokeOnClips ("clip.makeUnique"); };

            // A name of their own, which the playlist shows over them; none, and each is called after its track.
            setUpNameEditor (nameEditor,
                             [this] (const juce::String& text)
                             {
                                 if (text.trim() != shownName)
                                     renameClips (context, clips, text);
                             });
            nameEditor.setTitle ("Name");
            nameEditor.setTooltip ("A name the playlist shows over the clips, with their aliases. Leave it empty and each is called "
                                   "after its track, which shows no name over it");

            // The fade shapes are commands, ticked for the shape the clips have.
            for (int i = 0; i < (int) std::size (fadeShapes); ++i)
                shapeBox.addItem (fadeShapes[i].name, i + 1);

            shapeBox.setTitle ("Fade shape");
            shapeBox.setTooltip ("How the clips' fades in and out curve");
            shapeBox.onChange = [this]
            {
                if (const auto index = shapeBox.getSelectedId() - 1; juce::isPositiveAndBelow (index, (int) std::size (fadeShapes)))
                    invokeOnClips (fadeShapes[index].command);
            };

            addRow ("Name", &nameEditor);
            addRow ("Kind", &kindLabel);
            addRow ("Track", &trackLabel);
            addRow ("Starts", &startField);
            addRow ("Length", &lengthField);
            addRow ("Muted", &mutedToggle);
            addRow ("Alias", &aliasLabel);
            addRow ({}, &makeUniqueButton);
            addRow ("Gain", &gainField);
            addRow ("Pitch", &pitchField);
            addRow ("Fade in", &fadeInLabel);
            addRow ("Fade out", &fadeOutLabel);
            addRow ("Fade shape", &shapeBox);
            refresh();
        }

        // A command that acts on the selected clips, with these selected.
        void invokeOnClips (const juce::String& id)
        {
            auto& selection = context.getSongSelection();

            if (selection.getClips() != clips)
            {
                selection.clear();

                for (const auto clip : clips)
                    selection.add (clip);
            }

            commands.invoke (id);
        }

        void refresh() override
        {
            std::vector<ClipInfo> infos;

            for (const auto id : clips)
                if (const auto info = getClip (context, id))
                    infos.push_back (*info);

            if (infos.empty())
                return;

            const auto& first = infos.front();
            const auto row = getTrackRow (context, first.track);
            colour = row.has_value() ? row->colour : juce::Colours::transparentBlack;
            title.setText (infos.size() == 1 ? (first.name.isNotEmpty() ? first.name : juce::String ("Clip"))
                                             : juce::String ((int) infos.size()) + " clips",
                           juce::dontSendNotification);

            const auto allAudio =
                std::all_of (infos.begin(), infos.end(), [] (const ClipInfo& info) { return info.kind == ClipType::audio; });
            const auto allMidi =
                std::all_of (infos.begin(), infos.end(), [] (const ClipInfo& info) { return info.kind == ClipType::midi; });
            kindLabel.setText (allMidi ? "MIDI" : allAudio ? "Audio" : "MIDI and audio", juce::dontSendNotification);
            trackLabel.setText (row.has_value() ? row->name : juce::String(), juce::dontSendNotification);

            // Each field the range of the clips' values.
            const auto showRange = [] (MultiField& field, const std::vector<double>& values)
            {
                if (! values.empty())
                    field.show (*std::min_element (values.begin(), values.end()), *std::max_element (values.begin(), values.end()));
            };

            std::vector<double> starts, lengths, gains, pitches;

            for (const auto& info : infos)
            {
                starts.push_back (info.range.getStart().inBeats());
                lengths.push_back (info.range.getLength().inBeats());

                if (const auto audio = getAudioClip (context, info.id))
                {
                    gains.push_back (audio->gain);
                    pitches.push_back (audio->pitch);
                }
            }

            // Stepped by the time signature's beat where the earliest starts, as their text counts (an eighth in 6/8).
            start = BeatPosition::fromBeats (*std::min_element (starts.begin(), starts.end()));
            startField.setStep (getBeatLength (context, start).inBeats());
            lengthField.setStep (getBeatLength (context, start).inBeats());
            showRange (startField, starts);
            showRange (lengthField, lengths);
            showRange (gainField, gains);
            showRange (pitchField, pitches);
            setRowVisible (gainField, allAudio);
            setRowVisible (pitchField, allAudio);
            mutedToggle.setToggleState (std::all_of (infos.begin(), infos.end(), [] (const ClipInfo& info) { return info.muted; }),
                                        juce::dontSendNotification);

            const auto linked = std::any_of (infos.begin(), infos.end(), [] (const ClipInfo& info) { return info.linked; });
            aliasLabel.setText (linked ? "Yes: plays what its copies play" : "No", juce::dontSendNotification);
            setRowVisible (makeUniqueButton, linked);

            // Their own name, if they share one; else nothing, over the name each is called after, or "Several names".
            const auto sameName = std::all_of (infos.begin(), infos.end(), [&first] (const ClipInfo& info)
                                               { return info.name == first.name && info.namedAfterTrack == first.namedAfterTrack; });
            shownName = sameName && ! first.namedAfterTrack ? first.name : juce::String();

            if (! nameEditor.hasKeyboardFocus (true))
            {
                nameEditor.setText (shownName, juce::dontSendNotification);
                nameEditor.setTextToShowWhenEmpty (! sameName              ? juce::String ("Several names")
                                                   : first.namedAfterTrack ? first.name + " (the track's)"
                                                                           : juce::String(),
                                                   getPalette (*this).textMuted);
            }

            // An audio clip's fades, as parts of its length; a MIDI clip has none.
            fadeInLabel.setText (juce::String (juce::roundToInt (first.fadeIn * 100.0)) + "% of the clip", juce::dontSendNotification);
            fadeOutLabel.setText (juce::String (juce::roundToInt (first.fadeOut * 100.0)) + "% of the clip", juce::dontSendNotification);

            for (auto* fadeRow : std::initializer_list<juce::Component*> { &fadeInLabel, &fadeOutLabel, &shapeBox })
                setRowVisible (*fadeRow, allAudio);

            refreshShape();
            repaint();
        }

        void refreshShape()
        {
            auto ticked = 0;

            for (int i = 0; i < (int) std::size (fadeShapes) && context.getSongSelection().getClips() == clips; ++i)
                if (const auto* command = commands.getRegistry().find (fadeShapes[i].command))
                    if (command->isTicked != nullptr && command->isTicked())
                        ticked = i + 1;

            shapeBox.setSelectedId (ticked, juce::dontSendNotification);
        }

        void updateValues() override
        {
            if (! shapeBox.isPopupActive())
                refreshShape();
        }

        struct FadeShape
        {
            const char* name;
            const char* command;
        };

        static constexpr FadeShape fadeShapes[] = { { "Linear", "clip.fadeLinear" },
                                                    { "Convex", "clip.fadeConvex" },
                                                    { "Concave", "clip.fadeConcave" },
                                                    { "S-curve", "clip.fadeSCurve" } };

        CommandBridge& commands;
        ProjectContext& context;
        const std::vector<ClipID> clips;
        BeatPosition start; // the earliest clip's, where lengths are counted in bars
        juce::Label kindLabel, trackLabel, aliasLabel, fadeInLabel, fadeOutLabel;
        MultiField startField { context, "Starts", 0.0, 100000.0, 1.0, "Move Clips" };
        MultiField lengthField { context, "Length", 0.0, 100000.0, 1.0, "Change Clip Length" }; // the model keeps a 64th at least
        MultiField gainField { context, "Gain", minClipGain, maxClipGain, 0.5, "Change Clip Gain" };
        MultiField pitchField { context, "Pitch", -maxClipPitch, maxClipPitch, 1.0, "Change Clip Pitch" };
        juce::ToggleButton mutedToggle;
        juce::TextButton makeUniqueButton { "Make Unique" };
        juce::ComboBox shapeBox;
        juce::TextEditor nameEditor;
        juce::String shownName; // what the name field shows: their own name, if they share one, else nothing
    };

    //==============================================================================
    // Notes selected in the piano roll: where they start, how long they are, their pitch and velocity, whether they play,
    // and the clip they are in. Several share the fields, which act on all of them, as Bitwig's inspector edits notes.
    struct NotesPage : public Inspector::Page
    {
        NotesPage (CommandBridge& bridge, ProjectContext& c, std::vector<NoteRef> shown)
            : Page (shown.size() == 1 ? "Note" : "Notes", juce::Colours::transparentBlack),
              commands (bridge),
              context (c),
              notes (std::move (shown))
        {
            setUpValueLabel (clipLabel);

            const auto setUp = [this] (MultiField& field, NoteProperty property, const juce::String& tooltip)
            {
                field.setTooltip (tooltip);
                field.change = [this, property] (double amount) { changeNotes (context, notes, property, amount); };
                field.set = [this, property] (double value) { setNotesTo (context, notes, property, value); };
            };

            setUp (startField, NoteProperty::start, "Where the notes start: drag, scroll or type a bar and a beat (5.3)");
            startField.format = [this] (double beat) { return describePlace (context, BeatPosition::fromBeats (beat)); };
            startField.parse = [this] (const juce::String& text) -> std::optional<double>
            {
                const auto place = parsePlace (context, text);
                return place.has_value() ? std::optional (place->inBeats()) : std::nullopt;
            };

            setUp (lengthField, NoteProperty::length, "How long the notes are: drag, scroll or type a length (1/16, 2 beats)");
            lengthField.format = [this] (double beats) { return describeLength (context, { start, BeatDuration::fromBeats (beats) }); };
            lengthField.parse = [this] (const juce::String& text) -> std::optional<double>
            {
                const auto length = parseLength (context, text, start);
                return length.has_value() ? std::optional (length->inBeats()) : std::nullopt;
            };

            setUp (pitchField, NoteProperty::pitch, "The notes' pitch: drag or scroll to transpose them, or type a note (C4)");
            pitchField.format = [] (double pitch) { return getPitchName ((int) std::lround (pitch)); };
            pitchField.parse = [] (const juce::String& text) { return parsePitch (text); };

            setUp (velocityField, NoteProperty::velocity, "How hard the notes are played, from 1 to 127");
            velocityField.format = [] (double velocity) { return juce::String ((int) std::lround (velocity)); };

            mutedToggle.setTitle ("Muted");
            mutedToggle.setTooltip ("A muted note doesn't play");
            mutedToggle.onClick = [this] { commands.invoke ("edit.mute"); }; // the selected notes, which these are

            addRow ("Starts", &startField);
            addRow ("Length", &lengthField);
            addRow ("Pitch", &pitchField);
            addRow ("Velocity", &velocityField);
            addRow ("Muted", &mutedToggle);
            addRow ("Clip", &clipLabel);
            refresh();
        }

        void refresh() override
        {
            std::vector<NoteData> shown;

            for (const auto ref : notes)
                if (const auto note = getNote (context, ref))
                    shown.push_back (*note);

            if (shown.empty())
                return;

            const auto range = [&shown] (auto&& valueOf)
            {
                auto lowest = valueOf (shown.front()), highest = lowest;

                for (const auto& note : shown)
                {
                    lowest = std::min (lowest, valueOf (note));
                    highest = std::max (highest, valueOf (note));
                }

                return std::pair (lowest, highest);
            };

            const auto [firstStart, lastStart] = range ([] (const NoteData& note) { return note.start.inBeats(); });
            start = BeatPosition::fromBeats (firstStart);
            startField.show (firstStart, lastStart);
            const auto [shortest, longest] = range ([] (const NoteData& note) { return note.length.inBeats(); });
            lengthField.show (shortest, longest);
            const auto [lowest, highest] = range ([] (const NoteData& note) { return (double) note.pitch; });
            pitchField.show (lowest, highest);
            const auto [softest, hardest] = range ([] (const NoteData& note) { return (double) note.velocity; });
            velocityField.show (softest, hardest);

            mutedToggle.setToggleState (std::all_of (shown.begin(), shown.end(), [] (const NoteData& note) { return note.muted; }),
                                        juce::dontSendNotification);

            title.setText (shown.size() == 1 ? getPitchName (shown.front().pitch) : juce::String ((int) shown.size()) + " notes",
                           juce::dontSendNotification);

            // Their clip's (the first's, of several), in its track's color.
            const auto clip = getNoteClip (context, shown.front().clip);
            colour = clip.has_value() ? clip->colour : juce::Colours::transparentBlack;
            clipLabel.setText (clip.has_value() ? clip->name : juce::String(), juce::dontSendNotification);
            repaint();
        }

        CommandBridge& commands;
        ProjectContext& context;
        const std::vector<NoteRef> notes;
        BeatPosition start; // the earliest note's, where lengths are counted in bars
        MultiField startField { context, "Starts", 0.0, 100000.0, 0.25, "Move Notes" };
        MultiField lengthField { context, "Length", 0.0, 1024.0, 0.25, "Change Note Length" }; // the model keeps a 64th at least
        MultiField pitchField { context, "Pitch", 0.0, 127.0, 1.0, "Transpose Notes" };
        MultiField velocityField { context, "Velocity", 1.0, 127.0, 1.0, "Change Velocity" };
        juce::ToggleButton mutedToggle;
        juce::Label clipLabel;
    };

    //==============================================================================
    // Automation points selected in a lane: the parameter they move, where they are, their value, and how the line after
    // each bends. Several share the fields, which act on all of them.
    struct AutomationPage : public Inspector::Page
    {
        AutomationPage (ProjectContext& c, const ParameterRef& ref, std::vector<int> shown)
            : Page ("Automation", juce::Colours::transparentBlack),
              context (c),
              parameter (ref),
              indices (std::move (shown)),
              handle (getParameter (c, ref)),
              target (getAutomationTarget (c, ref))
        {
            setUpValueLabel (parameterLabel);

            positionField.setTooltip ("Where the points are: drag, scroll or type a bar and a beat (5.3)");
            positionField.format = [this] (double beat) { return describePlace (context, BeatPosition::fromBeats (beat)); };
            positionField.parse = [this] (const juce::String& text) -> std::optional<double>
            {
                const auto place = parsePlace (context, text);
                return place.has_value() ? std::optional (place->inBeats()) : std::nullopt;
            };
            positionField.change = [this] (double amount)
            { moveAutomationPoints (context, parameter, indices, BeatDuration::fromBeats (amount), 0.0f); };
            positionField.set = [this] (double value)
            { moveAutomationPoints (context, parameter, indices, BeatDuration::fromBeats (value - earliest), 0.0f); };

            valueField.setTooltip ("The points' value: drag or scroll to move them all, or type one for all of them");
            valueField.format = [this] (double value) { return getValueText ((float) value); };
            valueField.parse = [this] (const juce::String& text) { return parseValue (text); };
            valueField.change = [this] (double amount) { moveAutomationPoints (context, parameter, indices, {}, (float) amount); };
            valueField.set = [this] (double value) { setAutomationPointValues (context, parameter, indices, (float) value); };

            curveField.setTooltip ("How the line after each point bends: 0% is straight, up to 100% either way");
            curveField.format = [] (double bend) { return juce::String (juce::roundToInt (bend * 100.0)) + "%"; };
            curveField.parse = [] (const juce::String& text) -> std::optional<double>
            { return text.containsAnyOf ("0123456789") ? std::optional (text.getDoubleValue() / 100.0) : std::nullopt; };
            curveField.change = [this] (double) { setAutomationCurves (context, parameter, indices, (float) curveField.getValue()); };
            curveField.set = [this] (double value) { setAutomationCurves (context, parameter, indices, (float) value); };

            addRow ("Parameter", &parameterLabel);
            addRow ("Position", &positionField);
            addRow ("Value", &valueField);
            addRow ("Curve", &curveField);
            refresh();
        }

        // A value as the parameter's lane and control show it, and one typed so.
        ParameterKind getParameterKind() const { return target.has_value() ? target->kind : ParameterKind::other; }

        juce::String getValueText (float value) const
        {
            if (getParameterKind() == ParameterKind::volume)
                return formatDecibels (faderPositionToDecibels (value));

            if (getParameterKind() == ParameterKind::pan)
                return formatPan (value);

            if (const auto info = handle.getInfo())
                return formatParameterValue (*info, value);

            return handle.getText (value);
        }

        std::optional<double> parseValue (const juce::String& text) const
        {
            if (getParameterKind() == ParameterKind::volume)
            {
                const auto dB = parseDecibels (text);
                return dB.has_value() ? std::optional ((double) decibelsToFaderPosition ((float) *dB)) : std::nullopt;
            }

            if (getParameterKind() == ParameterKind::pan)
                return parsePan (text);

            if (const auto info = handle.getInfo())
            {
                const auto value = parseParameterValue (*info, text);
                return value.has_value() ? std::optional ((double) *value) : std::nullopt;
            }

            // As the device writes it: a plug-in's as the plug-in reads it.
            const auto value = handle.readText (text);
            return value.has_value() ? std::optional ((double) *value) : std::nullopt;
        }

        void refresh() override
        {
            const auto points = getAutomationPoints (context, parameter);
            std::vector<AutomationPoint> shown;

            for (const auto index : indices)
                if (juce::isPositiveAndBelow (index, (int) points.size()))
                    shown.push_back (points[(size_t) index]);

            if (shown.empty())
                return;

            const auto range = [&shown] (auto&& valueOf)
            {
                auto lowest = valueOf (shown.front()), highest = lowest;

                for (const auto& point : shown)
                {
                    lowest = std::min (lowest, valueOf (point));
                    highest = std::max (highest, valueOf (point));
                }

                return std::pair (lowest, highest);
            };

            const auto [first, last] = range ([] (const AutomationPoint& point) { return point.beat.inBeats(); });
            earliest = first;
            positionField.show (first, last);
            const auto [lowest, highest] = range ([] (const AutomationPoint& point) { return (double) point.value; });
            valueField.show (lowest, highest);
            const auto [least, most] = range ([] (const AutomationPoint& point) { return (double) point.curve; });
            curveField.show (least, most);

            const auto name = target.has_value() ? target->deviceName + ": " + target->name : handle.getName();
            parameterLabel.setText (name, juce::dontSendNotification);
            title.setText (shown.size() == 1 ? juce::String ("Automation point") : juce::String ((int) shown.size()) + " points",
                           juce::dontSendNotification);

            if (const auto row = target.has_value() ? getTrackRow (context, target->track) : std::nullopt)
                colour = row->colour;

            repaint();
        }

        ProjectContext& context;
        const ParameterRef parameter;
        const std::vector<int> indices;
        const ParameterHandle handle;
        const std::optional<AutomationTarget> target;
        double earliest = 0.0; // the earliest point's beat
        juce::Label parameterLabel;
        MultiField positionField { context, "Position", 0.0, 100000.0, 0.25, "Move Automation Points" };
        MultiField valueField { context,
                                "Value",
                                getLowest (handle),
                                getHighest (handle),
                                (getHighest (handle) - getLowest (handle)) / 100.0,
                                "Move Automation Points" };

        // The parameter's range, or 0 to 1 for one that is gone.
        static double getLowest (const ParameterHandle& h)
        {
            const auto range = h.getRange();
            return range.end > range.start ? range.start : 0.0;
        }

        static double getHighest (const ParameterHandle& h)
        {
            const auto range = h.getRange();
            return range.end > range.start ? range.end : 1.0;
        }
        MultiField curveField { context, "Curve", -1.0, 1.0, 0.01, "Bend Automation" };
    };

    //==============================================================================
    // A section: its name, color, place, length, energy and tip.
    struct SectionPage : public Inspector::Page
    {
        SectionPage (ProjectContext& c, SectionID id)
            : Page ("Section", juce::Colours::transparentBlack),
              context (c),
              section (id),
              colourButton ("Color")
        {
            setUpNameEditor (nameEditor,
                             [this] (const juce::String& text)
                             {
                                 if (const auto shown = getSongSection (context, section); shown.has_value() && text != shown->name)
                                     renameSection (context, section, text);
                             });
            nameEditor.setTitle ("Name");
            nameEditor.setTooltip ("The section's name: type another and press Enter");
            colourButton.setTooltip ("The section's color, behind the tracks. Click for another");
            colourButton.onClick = [this]
            {
                showColourMenu (colourButton, colourButton.colour, [&projectContext = context, id = section] (juce::Colour chosen)
                                { setSectionColour (projectContext, id, chosen); });
            };

            for (auto* label : { &startLabel, &lengthLabel, &energyLabel, &tipLabel })
                setUpValueLabel (*label);

            tipLabel.setJustificationType (juce::Justification::topLeft);
            addRow ("Name", &nameEditor);
            addRow ("Color", &colourButton);
            addRow ("Starts", &startLabel);
            addRow ("Length", &lengthLabel);
            addRow ("Energy", &energyLabel);
            addRow ("Tip", &tipLabel, tipLines * controlHeight);
            refresh();
        }

        void refresh() override
        {
            const auto shown = getSongSection (context, section);

            if (! shown.has_value())
                return;

            title.setText (shown->name, juce::dontSendNotification);
            colour = shown->colour;
            colourButton.colour = shown->colour;
            colourButton.repaint();

            if (! nameEditor.hasKeyboardFocus (true))
                nameEditor.setText (shown->name, juce::dontSendNotification);

            startLabel.setText (describePlace (context, shown->range.getStart()), juce::dontSendNotification);
            lengthLabel.setText (describeLength (context, shown->range), juce::dontSendNotification);
            energyLabel.setText (juce::String (juce::roundToInt (shown->energy * 100.0f)) + "%: how intense it is",
                                 juce::dontSendNotification);
            tipLabel.setText (shown->tip.isNotEmpty() ? shown->tip : juce::String ("No tip"), juce::dontSendNotification);
            repaint();
        }

        ProjectContext& context;
        const SectionID section;
        juce::TextEditor nameEditor;
        ColourButton colourButton;
        juce::Label startLabel, lengthLabel, energyLabel, tipLabel;
    };

    //==============================================================================
    // With nothing selected, the song: its tempo, time signature, key, length, tracks and sections.
    struct SongPage : public Inspector::Page
    {
        explicit SongPage (ProjectContext& c)
            : Page ("Song", juce::Colours::transparentBlack),
              context (c)
        {
            title.setText ("Song", juce::dontSendNotification);

            for (auto* label : { &tempoLabel, &signatureLabel, &lengthLabel, &tracksLabel, &sectionsLabel, &hintLabel })
                setUpValueLabel (*label);

            // The key the chord lane's chords are in, from the song's start (a change of key further on is the lane's).
            keyButton.setTitle ("Key");
            keyButton.setTooltip ("The song's key: the chord lane's chords are in it and move with it, and the chord progressions "
                                  "play in it. Click for another");
            keyButton.onClick = [this]
            {
                auto& projectContext = context;
                getKeyChoiceMenu (getLaneKeyAt (context, {}), [&projectContext] (Key key) { setLaneKey (projectContext, {}, key); })
                    .showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&keyButton));
            };

            hintLabel.setText ("Select a track, clips, notes, automation points or a section to see and change them here.",
                               juce::dontSendNotification);
            hintLabel.setJustificationType (juce::Justification::topLeft);
            addRow ("Tempo", &tempoLabel);
            addRow ("Time", &signatureLabel);
            addRow ("Key", &keyButton);
            addRow ("Length", &lengthLabel);
            addRow ("Tracks", &tracksLabel);
            addRow ("Sections", &sectionsLabel);
            addRow ({}, &hintLabel, 2 * controlHeight);
            refresh();
        }

        void refresh() override
        {
            tempoLabel.setText (describeSongTempo(), juce::dontSendNotification);
            const auto signature = getTimeSignature (context).value_or (TimeSignature {});
            signatureLabel.setText (juce::String (signature.numerator) + "/" + juce::String (signature.denominator),
                                    juce::dontSendNotification);
            keyButton.setButtonText (describeKey (getLaneKeyAt (context, {})));
            lengthLabel.setText (describeLength (context, { BeatPosition(), getTimelineEnd (context) }), juce::dontSendNotification);
            tracksLabel.setText (juce::String ((int) getAllPlaylistTracks (context).size()), juce::dontSendNotification);
            sectionsLabel.setText (juce::String ((int) getSongSections (context).size()), juce::dontSendNotification);
        }

        // The song's tempo, "120 BPM"; or, while it changes along the song (the tempo lane), how far: "50 to 260 BPM".
        juce::String describeSongTempo() const
        {
            const auto tempos = getTempoChanges (context);

            if (tempos.size() <= 1)
                return formatTempo (getTempoHandle (context).getValue());

            const auto [lowest, highest] = std::minmax_element (tempos.begin(), tempos.end(),
                                                                [] (const TempoChange& a, const TempoChange& b) { return a.bpm < b.bpm; });
            return formatTempo (lowest->bpm).upToFirstOccurrenceOf (" ", false, false) + " to " + formatTempo (highest->bpm);
        }

        ProjectContext& context;
        juce::Label tempoLabel, signatureLabel, lengthLabel, tracksLabel, sectionsLabel, hintLabel;
        juce::TextButton keyButton;
    };
} // namespace

//==============================================================================
Inspector::Inspector (CommandBridge& bridge, ProjectContext& projectContext)
    : commands (bridge),
      context (projectContext)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Inspector");
    view.setTitle ("Properties"); // what a screen reader says as the keys go to it, to scroll it
    view.setScrollBarsShown (true, false);
    view.setScrollBarThickness (6);
    addAndMakeVisible (view);
    context.addListener (this);
    context.getSongSelection().addChangeListener (this);
    showTarget();
    startTimerHz (10);
}

Inspector::~Inspector()
{
    context.getSongSelection().removeChangeListener (this);
    context.removeListener (this);
}

juce::String Inspector::getShown() const
{
    return page != nullptr ? page->kind : juce::String();
}

Inspector::Target Inspector::getTarget() const
{
    if (! context.hasProject())
        return {};

    const auto& selection = context.getSongSelection();

    // Notes and automation points are selected on their own, with nothing else.
    if (auto notes = selection.getNotes(); ! notes.empty())
        return { .kind = Target::Kind::notes, .notes = std::move (notes) };

    if (auto points = selection.getSelectedAutomationPoints(); points.parameter.isValid() && ! points.indices.empty())
        return { .kind = Target::Kind::automation, .parameter = points.parameter, .points = std::move (points.indices) };

    if (auto clips = selection.getClips(); ! clips.empty())
        return { .kind = Target::Kind::clips, .clips = std::move (clips) };

    if (const auto sections = selection.getSections(); ! sections.empty())
        return { .kind = Target::Kind::section, .section = sections.front() };

    // A device's track (the master's has none here: the Devices view shows it).
    if (const auto devices = selection.getDevices(); ! devices.empty())
        if (const auto channel = getDeviceChannel (context, devices.front()); channel.has_value() && channel->isValid())
            return { .kind = Target::Kind::track, .track = *channel };

    if (const auto tracks = selection.getTracks(); ! tracks.empty() && getChannel (context, tracks.front()).has_value())
        return { .kind = Target::Kind::track, .track = tracks.front() };

    return { .kind = Target::Kind::song };
}

void Inspector::showTarget()
{
    const auto target = getTarget();

    if (target == shown && page != nullptr)
        return;

    shown = target;
    view.setViewedComponent (nullptr, false);
    page = nullptr;

    switch (shown.kind)
    {
        case Target::Kind::track:
            page = std::make_unique<TrackPage> (commands, context, shown.track);
            break;
        case Target::Kind::clips:
            page = std::make_unique<ClipsPage> (commands, context, shown.clips);
            break;
        case Target::Kind::notes:
            page = std::make_unique<NotesPage> (commands, context, shown.notes);
            break;
        case Target::Kind::automation:
            page = std::make_unique<AutomationPage> (context, shown.parameter, shown.points);
            break;
        case Target::Kind::section:
            page = std::make_unique<SectionPage> (context, shown.section);
            break;
        case Target::Kind::song:
            page = std::make_unique<SongPage> (context);
            break;
        case Target::Kind::none:
        default:
            break;
    }

    if (page != nullptr)
    {
        view.setViewedComponent (page.get(), false);
        addAndMakeVisible (page->title); // over the rows, which scroll under it
    }

    resized();
}

void Inspector::layOutPage()
{
    if (page != nullptr)
    {
        const auto width = std::max (1, view.getWidth() - view.getScrollBarThickness());
        page->setSize (width, std::max (view.getHeight(), page->getPreferredHeight (width)));
    }

    repaint(); // the title's colour bar
}

void Inspector::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);

    // The color of what it shows, beside its title, as a track's header has its track's.
    if (page != nullptr && ! page->colour.isTransparent())
    {
        g.setColour (page->colour);
        g.fillRect (page->title.getBounds().withX (0).withWidth (colourBarHeight));
    }
}

void Inspector::resized()
{
    // The page's title over its rows, which scroll under it.
    auto area = getLocalBounds();
    area.removeFromTop (Space::m + titleHeight + Space::s);

    if (page != nullptr)
        page->title.setBounds (getTitleRow().withTrimmedLeft (Space::m).withTrimmedRight (std::max (Space::m, titleRoom)));

    view.setBounds (area);
    layOutPage();
}

juce::Rectangle<int> Inspector::getTitleRow() const
{
    return getLocalBounds().withTrimmedTop (Space::m).withHeight (titleHeight);
}

void Inspector::setTitleRoom (int room)
{
    if (std::exchange (titleRoom, room) != room)
        resized();
}

void Inspector::projectChanged()
{
    page = nullptr; // another project's items can have the same IDs
    showTarget();
}

void Inspector::songChanged (const SongChanges& changes)
{
    // What it shows may be gone, or changed.
    showTarget();

    if (page != nullptr
        && (changes.tracks || changes.clips || changes.sections || changes.chords || changes.devices || changes.automation
            || changes.tempo))
    {
        page->refresh();
        layOutPage();
    }
}

void Inspector::changeListenerCallback (juce::ChangeBroadcaster*)
{
    showTarget();
}

void Inspector::timerCallback()
{
    if (page != nullptr && isShowing())
        page->updateValues();
}
} // namespace phazite::ui
