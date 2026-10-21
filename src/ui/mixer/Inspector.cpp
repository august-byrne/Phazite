// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Inspector.h"
#include "StripParts.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/SongSelection.h"
#include "model/arrangement/TimelineModel.h"
#include "model/automation/AutomationModel.h"
#include "model/chords/ChordModel.h"
#include "model/mixer/MixerModel.h"
#include "model/transport/TransportModel.h"
#include "ui/components/Controls.h"
#include "ui/devices/DeviceMenus.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <cmath>

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

    // "Bar 5", or "Bar 5, beat 3" for a place after a bar's first beat; bars count from 1, as the time line's do.
    juce::String describePlace (const ProjectContext& context, BeatPosition place)
    {
        const auto bar = getBarAt (context, place); // from 0
        const auto intoBar = (place - getBarStart (context, bar)).inBeats();

        if (intoBar < 0.001)
            return "Bar " + juce::String (bar + 1);

        return "Bar " + juce::String (bar + 1) + ", beat "
               + juce::String (1.0 + intoBar, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    }

    // "2 bars", "1 bar, 2 beats" or "3 beats", in the time signature where it starts.
    juce::String describeLength (const ProjectContext& context, BeatRange range)
    {
        const auto beats = range.getLength().inBeats();
        const auto perBar = std::max (1, getBeatsPerBar (context, range.getStart()));
        const auto bars = (int) std::floor (beats / perBar + 0.0001);
        const auto rest = std::max (0.0, beats - bars * perBar);
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

    // A single-line field that changes something when Enter is pressed or the focus leaves it.
    void setUpNameEditor (juce::TextEditor& editor, std::function<void (const juce::String&)> apply)
    {
        editor.setFont (juce::FontOptions (TextSize::body));
        editor.setSelectAllWhenFocused (true);
        editor.onReturnKey = [&editor, apply] { apply (editor.getText()); };
        editor.onFocusLost = [&editor, apply] { apply (editor.getText()); };
        editor.onEscapeKey = [&editor] { editor.giveAwayKeyboardFocus(); };
    }
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
        label->setColour (juce::Label::textColourId, getPalette (*this).textMuted);
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
            level.setTooltip ("How much of this track the send feeds the return: off until you turn it. Double-click for 0 dB");
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
            colourButton.setTooltip ("The track's color, which its clips and its channel show. Click for another");
            colourButton.onClick = [this]
            {
                showColourMenu (colourButton, colourButton.colour, [&projectContext = context, id = track] (juce::Colour chosen)
                                { setTrackColours (projectContext, { id }, chosen); });
            };

            showAsDecibels (volume);
            volume.setTooltip ("The track's volume. Double-click for 0 dB; drag with Shift held for fine steps");
            volumeAttachment.setParameter (getVolume (context, track));
            showAsPan (pan);
            pan.setTooltip ("Where the track sits between the left and right speakers. Double-click to center it");
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
        MuteSoloButtons muteSolo;
        juce::TextButton instrumentButton, effectsButton, lanesButton, addSendButton { "+ Send" };
        juce::ComboBox modeBox;
        juce::OwnedArray<SendRow> sendRows;
        std::vector<TrackID> shownReturns; // the returns the send rows are for
        int sendsHeadingIndex = 0;
    };

    //==============================================================================
    // Clips: one's name, kind, place, length, whether it plays, and whether it is an alias; an audio clip's fades. Several
    // share the switches, which act on all of them.
    struct ClipsPage : public Inspector::Page
    {
        ClipsPage (CommandBridge& bridge, ProjectContext& c, std::vector<ClipID> shown)
            : Page (shown.size() == 1 ? "Clip" : "Clips", juce::Colours::transparentBlack),
              commands (bridge),
              context (c),
              clips (std::move (shown))
        {
            for (auto* label : { &kindLabel, &trackLabel, &startLabel, &lengthLabel, &aliasLabel, &fadeInLabel, &fadeOutLabel })
                setUpValueLabel (*label);

            mutedToggle.setTitle ("Muted");
            mutedToggle.setTooltip ("A muted clip doesn't play");
            mutedToggle.onClick = [this] { toggleClipsMuted (context, clips); };
            makeUniqueButton.setTooltip ("Makes the clips copies of their own, so that changing them no longer changes the clips "
                                         "they were aliases of");
            makeUniqueButton.onClick = [this] { invokeOnClips ("clip.makeUnique"); };
            followsChordsToggle.setTitle ("Follows the chords");
            followsChordsToggle.setTooltip ("A part made from the chord lane changes with the chords until its notes are edited. "
                                            "Turned on again, its notes are made from the chords again.");
            followsChordsToggle.onClick = [this] { setFollowsChords (context, clips.front(), followsChordsToggle.getToggleState()); };

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

            addRow ("Kind", &kindLabel);
            addRow ("Track", &trackLabel);
            addRow ("Starts", &startLabel);
            addRow ("Length", &lengthLabel);
            addRow ("Muted", &mutedToggle);
            addRow ("Alias", &aliasLabel);
            addRow ({}, &makeUniqueButton);
            addRow ("Follows the chords", &followsChordsToggle);
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

            auto range = first.range;

            for (const auto& info : infos)
                range = BeatRange (std::min (range.getStart(), info.range.getStart()), std::max (range.getEnd(), info.range.getEnd()));

            startLabel.setText (describePlace (context, range.getStart()), juce::dontSendNotification);
            lengthLabel.setText (describeLength (context, range), juce::dontSendNotification);
            mutedToggle.setToggleState (std::all_of (infos.begin(), infos.end(), [] (const ClipInfo& info) { return info.muted; }),
                                        juce::dontSendNotification);

            const auto linked = std::any_of (infos.begin(), infos.end(), [] (const ClipInfo& info) { return info.linked; });
            aliasLabel.setText (linked ? "Yes: plays what its copies play" : "No", juce::dontSendNotification);
            setRowVisible (makeUniqueButton, linked);

            // A part made from the chord lane: whether its notes change with the chords.
            const auto part = infos.size() == 1 ? getChordPart (context, first.id) : std::nullopt;
            followsChordsToggle.setToggleState (part.has_value() && followsChords (context, first.id), juce::dontSendNotification);
            setRowVisible (followsChordsToggle, part.has_value());

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
        juce::Label kindLabel, trackLabel, startLabel, lengthLabel, aliasLabel, fadeInLabel, fadeOutLabel;
        juce::ToggleButton mutedToggle, followsChordsToggle;
        juce::TextButton makeUniqueButton { "Make Unique" };
        juce::ComboBox shapeBox;
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
    // With nothing selected, the song: its tempo, time signature, length, tracks and sections.
    struct SongPage : public Inspector::Page
    {
        explicit SongPage (ProjectContext& c)
            : Page ("Song", juce::Colours::transparentBlack),
              context (c)
        {
            title.setText ("Song", juce::dontSendNotification);

            for (auto* label : { &tempoLabel, &signatureLabel, &lengthLabel, &tracksLabel, &sectionsLabel, &hintLabel })
                setUpValueLabel (*label);

            hintLabel.setText ("Select a track, a clip or a section to see its properties here.", juce::dontSendNotification);
            hintLabel.setJustificationType (juce::Justification::topLeft);
            addRow ("Tempo", &tempoLabel);
            addRow ("Time", &signatureLabel);
            addRow ("Length", &lengthLabel);
            addRow ("Tracks", &tracksLabel);
            addRow ("Sections", &sectionsLabel);
            addRow ({}, &hintLabel, 2 * controlHeight);
            refresh();
        }

        void refresh() override
        {
            tempoLabel.setText (juce::String (getTempoHandle (context).getValue(), 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".")
                                    + " BPM",
                                juce::dontSendNotification);
            const auto signature = getTimeSignature (context).value_or (TimeSignature {});
            signatureLabel.setText (juce::String (signature.numerator) + "/" + juce::String (signature.denominator),
                                    juce::dontSendNotification);
            lengthLabel.setText (describeLength (context, { BeatPosition(), getTimelineEnd (context) }), juce::dontSendNotification);
            tracksLabel.setText (juce::String ((int) getAllPlaylistTracks (context).size()), juce::dontSendNotification);
            sectionsLabel.setText (juce::String ((int) getSongSections (context).size()), juce::dontSendNotification);
        }

        ProjectContext& context;
        juce::Label tempoLabel, signatureLabel, lengthLabel, tracksLabel, sectionsLabel, hintLabel;
    };
} // namespace

//==============================================================================
Inspector::Inspector (CommandBridge& bridge, ProjectContext& projectContext)
    : commands (bridge),
      context (projectContext)
{
    setTitle ("Inspector");
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

    if (auto clips = selection.getClips(); ! clips.empty())
        return { .kind = Target::Kind::clips, .clips = std::move (clips) };

    // Notes are their clip's.
    if (const auto notes = selection.getNotes(); ! notes.empty())
        return { .kind = Target::Kind::clips, .clips = { notes.front().clip } };

    if (const auto sections = selection.getSections(); ! sections.empty())
        return { .kind = Target::Kind::section, .section = sections.front() };

    // A device's track (the master's has none here: the Devices view shows it), or an automation point's.
    if (const auto devices = selection.getDevices(); ! devices.empty())
        if (const auto channel = getDeviceChannel (context, devices.front()); channel.has_value() && channel->isValid())
            return { .kind = Target::Kind::track, .track = *channel };

    if (const auto points = selection.getSelectedAutomationPoints(); points.parameter.isValid())
        if (const auto target = getAutomationTarget (context, points.parameter); target.has_value() && getChannel (context, target->track))
            return { .kind = Target::Kind::track, .track = target->track };

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
    // The page's title over its rows, which scroll under it; the panel's buttons beside the title, in its top right corner.
    auto area = getLocalBounds();
    auto header = area.removeFromTop (Space::m + titleHeight + Space::s);

    if (page != nullptr)
        page->title.setBounds (header.reduced (Space::m, 0)
                                   .withTrimmedTop (Space::m)
                                   .withHeight (titleHeight)
                                   .withTrimmedRight (Shell::getPanelCornerWidth (3) + Space::xs - Space::m));

    view.setBounds (area);
    layOutPage();
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
