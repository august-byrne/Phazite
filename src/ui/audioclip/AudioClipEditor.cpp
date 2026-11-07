// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioClipEditor.h"
#include "ui/components/ValueText.h"
#include "ui/input/CommandUI.h"
#include "ui/playlist/ClipComponent.h"
#include "ui/playlist/ClipWaveform.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/timeline/TimelineGesture.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    constexpr int panelWidth = 196;     // the clip's settings, down the left
    constexpr int labelWidth = 52;      // their names
    constexpr double viewMargin = 0.02; // how much of the audio's length shows on either side of it, for its edges' grips

    // How the fields show their values.
    constexpr devices::ParameterInfo gainInfo { "gain", "Gain", minClipGain, maxClipGain, 0.0f, devices::Unit::decibels };
    constexpr devices::ParameterInfo pitchInfo { "pitch", "Pitch", -maxClipPitch, maxClipPitch, 0.0f, devices::Unit::semitones };
    constexpr devices::ParameterInfo fineInfo { "fine", "Fine", -50.0f, 50.0f, 0.0f, devices::Unit::cents };

    // A pitch as whole semitones and the cents above or below them: within half a semitone, +50 cents rather than -50.
    std::pair<double, double> splitPitch (float pitch)
    {
        const auto semitones = std::ceil ((double) pitch - 0.5);
        return { semitones, std::round (((double) pitch - semitones) * 100.0) };
    }

    juce::String describeChannels (int channels)
    {
        return channels == 1 ? juce::String ("mono") : channels == 2 ? juce::String ("stereo") : juce::String (channels) + " channels";
    }
} // namespace

//==============================================================================
// The clip's whole audio, with its handles. The hint bar shows what a drag does where the pointer is.
struct AudioClipEditor::Waveform : public juce::Component
{
    explicit Waveform (AudioClipEditor& e)
        : editor (e)
    {
        setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
        setTitle ("Audio clip");
    }

    enum class Zone
    {
        none,
        trimStart,
        trimEnd,
        fadeIn,
        fadeOut,
        bendIn, // the handle in the middle of the fade's curve
        bendOut,
        audio // inside the clip: moves its audio
    };

    AudioClipEditor& editor;
    std::unique_ptr<ClipWaveform> drawing; // the shown clip's, made by the app (nothing in tests)
    ClipID drawnClip;
    Zone hovered = Zone::none, dragging = Zone::none;
    std::unique_ptr<TimelineGesture> gesture; // from mouse down to mouse up
    bool dragged = false;                     // past the threshold: not a click
    double slipStartOffset = 0.0;             // the clip's offset when the drag began, for moving its part along the audio
    FadeCurve curveAtStart;                   // the bent fade's, when the drag began

    static bool isBend (Zone zone) { return zone == Zone::bendIn || zone == Zone::bendOut; }

    float beatToX (BeatPosition beat) const { return (float) editor.getView().beatToX (beat); }
    BeatPosition xToBeat (float x) const { return editor.getView().xToBeat (x); }

    void clipChanged()
    {
        if (drawnClip != editor.clip)
        {
            drawnClip = editor.clip;
            drawing = editor.clip.isValid() ? makeClipWaveform (editor.context, editor.clip, [this] { repaint(); }) : nullptr;
        }

        repaint();
    }

    //==============================================================================
    // Where things are, in this component's coordinates.
    juce::Rectangle<float> getClipArea() const
    {
        const auto& range = editor.info->clip.range;
        const auto x0 = beatToX (range.getStart()), x1 = beatToX (range.getEnd());
        return { x0, 0.0f, x1 - x0, (float) getHeight() };
    }

    const FadeCurve& getCurve (bool fadeIn) const { return fadeIn ? editor.info->clip.fadeInCurve : editor.info->clip.fadeOutCurve; }

    // Where a fade reaches full volume; it starts, silent, at the clip's edge.
    float getFadeEnd (bool fadeIn) const
    {
        const auto area = getClipArea();
        const auto fade = (float) (fadeIn ? editor.info->clip.fadeIn : editor.info->clip.fadeOut) * area.getWidth();
        return fadeIn ? area.getX() + fade : area.getRight() - fade;
    }

    // A fade's handle: at the top, where the fade reaches full volume, within the clip.
    juce::Point<float> getFadeHandle (bool fadeIn) const
    {
        const auto area = getClipArea();
        const auto radius = AudioClipTint::handleRadius;
        const auto x = juce::jlimit (area.getX() + radius, std::max (area.getX() + radius, area.getRight() - radius), getFadeEnd (fadeIn));
        return { x, radius + (float) Space::xs };
    }

    // The handle that bends a fade: on its curve, half way along it, while the fade is wide enough to bend.
    std::optional<juce::Point<float>> getBendHandle (bool fadeIn) const
    {
        const auto area = getClipArea();
        const auto silent = fadeIn ? area.getX() : area.getRight();
        const auto full = getFadeEnd (fadeIn);

        if (std::abs (full - silent) < AudioClipTint::minimumBendWidth)
            return {};

        // As paintFade() draws it, from the bottom (silence) to the top.
        const auto height = (float) getHeight();
        return juce::Point<float> ((silent + full) / 2.0f, height - height * getFadeGain (getCurve (fadeIn), 0.5f));
    }

    // An edge's grip, in the middle of its height.
    juce::Rectangle<float> getGrip (bool start) const
    {
        const auto area = getClipArea();
        const auto x = start ? area.getX() : area.getRight();
        return juce::Rectangle<float> ((float) AudioClipTint::gripWidth, (float) AudioClipTint::gripHeight)
            .withCentre ({ x, area.getCentreY() });
    }

    Zone getZoneAt (juce::Point<float> point) const
    {
        if (! editor.info.has_value())
            return Zone::none;

        const auto grab = (float) AudioClipTint::grabDistance;
        const auto isNear = [point, grab] (juce::Point<float> centre, float radius)
        { return point.getDistanceFrom (centre) <= radius + grab / 2.0f; };

        // The handles first, which sit inside the clip's edges.
        for (const auto fadeIn : { true, false })
            if (const auto bend = getBendHandle (fadeIn); bend.has_value() && isNear (*bend, AudioClipTint::bendRadius))
                return fadeIn ? Zone::bendIn : Zone::bendOut;

        for (const auto fadeIn : { true, false })
            if (isNear (getFadeHandle (fadeIn), AudioClipTint::handleRadius))
                return fadeIn ? Zone::fadeIn : Zone::fadeOut;

        const auto area = getClipArea();

        if (std::abs (point.x - area.getX()) <= grab)
            return Zone::trimStart;

        if (std::abs (point.x - area.getRight()) <= grab)
            return Zone::trimEnd;

        return area.contains (point) ? Zone::audio : Zone::none;
    }

    static juce::String describe (Zone zone)
    {
        switch (zone)
        {
            case Zone::trimStart:
                return "Drag to trim the start of the clip. Hold Alt to place it freely.";
            case Zone::trimEnd:
                return "Drag to trim the end of the clip. Hold Alt to place it freely.";
            case Zone::fadeIn:
                return "Drag to fade the clip in, and double-click to take the fade away.";
            case Zone::fadeOut:
                return "Drag to fade the clip out, and double-click to take the fade away.";
            case Zone::bendIn:
                return "Drag up or down to bend the fade in, double-click to straighten it, and right-click to switch between a curve "
                       "and an S-curve.";
            case Zone::bendOut:
                return "Drag up or down to bend the fade out, double-click to straighten it, and right-click to switch between a "
                       "curve and an S-curve.";
            case Zone::audio:
                return "Drag the clip along its audio to play another part of it: the clip stays where it is in the song. Hold Alt "
                       "to move it freely. Right-click for the fades' shape.";
            case Zone::none:
            default:
                return {};
        }
    }

    void setHovered (Zone zone)
    {
        if (hovered != zone)
        {
            hovered = zone;
            repaint();
        }

        setDescription (describe (zone));
        setMouseCursor (zone == Zone::audio  ? juce::MouseCursor::DraggingHandCursor
                        : zone == Zone::none ? juce::MouseCursor::NormalCursor
                        : isBend (zone)      ? juce::MouseCursor::UpDownResizeCursor
                                             : juce::MouseCursor::LeftRightResizeCursor);
    }

    //==============================================================================
    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        g.fillAll (p.background);
        paintGridLines (g, editor, getLocalBounds());

        if (! editor.info.has_value())
        {
            g.setColour (p.textMuted);
            g.setFont (juce::FontOptions (TextSize::body));
            g.drawFittedText (editor.hasProject() ? "Select an audio clip in the playlist, or a track that holds audio, to edit it here"
                                                  : juce::String(),
                              getLocalBounds().reduced (Space::l), juce::Justification::centred, 2);
            return;
        }

        const auto& shown = *editor.info;
        const auto row = getTrackRow (editor.context, shown.clip.track);
        const auto colour = row.has_value() ? row->colour : p.accent;
        const auto clipArea = getClipArea();
        const auto audioArea = getLocalBounds().reduced (0, Space::s);

        // The audio its edges trim off, muted, and then the clip's own, in its track's color over a wash of it.
        g.setColour (colour.withAlpha (AudioClipTint::clipWash));
        g.fillRect (clipArea);

        // The file's audio in view, as one span that the waveform keeps between paints (ClipWaveform.h), and its parts in their
        // colours through clip regions.
        const auto left = std::max (0, juce::roundToInt (beatToX (shown.content.getStart())));
        const auto right = std::min (getWidth(), juce::roundToInt (beatToX (shown.content.getEnd())));

        if (drawing != nullptr && shown.state != AudioState::missing && right > left)
        {
            const auto shownAudio = audioArea.withX (left).withWidth (right - left);
            const BeatRange span { xToBeat ((float) left), xToBeat ((float) right) };

            const auto paintPart = [&] (BeatRange part, juce::Colour partColour)
            {
                const auto x0 = juce::roundToInt (beatToX (part.getStart())), x1 = juce::roundToInt (beatToX (part.getEnd()));
                juce::Graphics::ScopedSaveState state (g);

                if (g.reduceClipRegion (shownAudio.withX (x0).withWidth (std::max (0, x1 - x0))))
                {
                    g.setColour (partColour);
                    drawing->paintSpan (g, shownAudio, span);
                }
            };

            const auto trimmed = p.textMuted.withAlpha (AudioClipTint::trimmedAudio);
            paintPart ({ shown.content.getStart(), shown.clip.range.getStart() }, trimmed);
            paintPart ({ shown.clip.range.getEnd(), shown.content.getEnd() }, trimmed);
            paintPart (shown.clip.range, colour);
        }

        // The fades, over the clip's height.
        const auto width = clipArea.getWidth();

        if (shown.clip.fadeIn > 0.0)
            paintFade (g, clipArea.getX(), clipArea.getX() + (float) shown.clip.fadeIn * width, 0.0f, (float) getHeight(),
                       shown.clip.fadeInCurve);

        if (shown.clip.fadeOut > 0.0)
            paintFade (g, clipArea.getRight(), clipArea.getRight() - (float) shown.clip.fadeOut * width, 0.0f, (float) getHeight(),
                       shown.clip.fadeOutCurve);

        // Its edges as brackets, with their grips, and the fades' handles; the one under the pointer in the text color, which
        // stands out on a track of any color.
        const auto isLit = [this] (Zone zone) { return hovered == zone || dragging == zone; };
        const auto height = (float) getHeight();

        for (const auto start : { true, false })
        {
            const auto lit = isLit (start ? Zone::trimStart : Zone::trimEnd);
            const auto line = lit ? AudioClipTint::hoveredEdgeWidth : AudioClipTint::edgeWidth;
            const auto x = start ? clipArea.getX() - line / 2.0f : clipArea.getRight() + line / 2.0f;
            const auto tickX = start ? x : x - AudioClipTint::bracketLength;
            g.setColour (lit ? p.text : colour);
            g.fillRect (juce::Rectangle<float> (start ? x : x - line, 0.0f, line, height));
            g.fillRect (juce::Rectangle<float> (tickX, 0.0f, AudioClipTint::bracketLength, line));
            g.fillRect (juce::Rectangle<float> (tickX, height - line, AudioClipTint::bracketLength, line));
            g.fillRoundedRectangle (getGrip (start), cornerRadius);
        }

        for (const auto fadeIn : { true, false })
        {
            const auto lit = isLit (fadeIn ? Zone::fadeIn : Zone::fadeOut);
            const auto radius = lit ? AudioClipTint::hoveredHandleRadius : AudioClipTint::handleRadius;
            const auto circle = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (getFadeHandle (fadeIn));
            g.setColour (lit ? p.text : colour);
            g.fillEllipse (circle);
            g.setColour (p.text);
            g.drawEllipse (circle, AudioClipTint::handleRing);
        }

        // The bends' handles, hollow as an automation lane's, filled under the pointer.
        for (const auto fadeIn : { true, false })
        {
            if (const auto centre = getBendHandle (fadeIn))
            {
                const auto lit = isLit (fadeIn ? Zone::bendIn : Zone::bendOut);
                const auto radius = lit ? AudioClipTint::hoveredBendRadius : AudioClipTint::bendRadius;
                const auto ring = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (*centre);
                g.setColour (lit ? p.text : p.background);
                g.fillEllipse (ring);
                g.setColour (p.text);
                g.drawEllipse (ring, AudioClipTint::handleRing);
            }
        }

        // While Phazite renders the audio, or without its file, say so.
        if (shown.state != AudioState::ready)
        {
            g.setColour (p.textMuted);
            g.setFont (juce::FontOptions (TextSize::body));
            g.drawFittedText (shown.state == AudioState::preparing ? "Preparing the audio. The clip is silent until it is ready."
                                                                   : "The clip's audio file can't be found: " + shown.fileName,
                              clipArea.toNearestInt().getIntersection (getLocalBounds()).reduced (Space::s), juce::Justification::centred,
                              2);
        }
    }

    //==============================================================================
    void mouseMove (const juce::MouseEvent& e) override { setHovered (getZoneAt (e.position)); }
    void mouseExit (const juce::MouseEvent&) override { setHovered (Zone::none); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        setHovered (getZoneAt (e.position));
        dragged = false;
        dragging = Zone::none;

        if (! editor.info.has_value())
            return;

        if (e.mods.isPopupMenu())
        {
            if (isBend (hovered))
                showShapeMenu (hovered == Zone::bendIn);
            else
                showMenu();

            return;
        }

        dragging = hovered;
        gesture = std::make_unique<TimelineGesture>();
        gesture->startPosition = e.getPosition();
        gesture->startBeat = xToBeat (e.position.x);
        slipStartOffset = getSlipOffset (editor.context, editor.clip);

        if (isBend (dragging))
            curveAtStart = getCurve (dragging == Zone::bendIn);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (gesture == nullptr || dragging == Zone::none)
            return;

        if (! dragged)
        {
            if (! TimelineGesture::isPastDragThreshold (e))
                return;

            dragged = true;
            gesture->beginStep (editor.context, dragging == Zone::trimStart ? "Trim Clip Start"
                                                : dragging == Zone::trimEnd ? "Trim Clip End"
                                                : dragging == Zone::fadeIn  ? "Fade In"
                                                : dragging == Zone::fadeOut ? "Fade Out"
                                                : dragging == Zone::bendIn  ? "Bend Fade In"
                                                : dragging == Zone::bendOut ? "Bend Fade Out"
                                                                            : "Slip Clip");
        }

        const auto beat = xToBeat (e.position.x);
        auto& song = editor.context;
        const auto edited = editor.clip;

        switch (dragging)
        {
            case Zone::trimStart:
                trimClipStart (song, edited, editor.snap (beat, e.mods));
                break;

            case Zone::trimEnd:
                trimClipEnd (song, edited, editor.snap (beat, e.mods));
                break;

            case Zone::fadeIn:
            case Zone::fadeOut:
                fadeClipTo (song, edited, dragging == Zone::fadeIn, beat);
                break;

            case Zone::bendIn:
            case Zone::bendOut:
            {
                // Up bows the curve up, to full volume sooner, whichever way the fade goes; half the clip's height bends it all
                // the way from straight.
                const auto up = ((float) gesture->startPosition.y - e.position.y) / std::max (1.0f, (float) getHeight() / 2.0f);
                auto curve = curveAtStart;
                curve.bend = juce::jlimit (-1.0f, 1.0f, curveAtStart.bend + up * AudioClipTint::bendPerHalfHeight);
                setFadeCurve (song, edited, dragging == Zone::bendIn, curve);
                break;
            }

            case Zone::audio:
            {
                // The clip's part goes along the audio, which stays where it is on the screen (the view follows the audio):
                // so by the pointer's distance from where it was pressed, and the audio the other way in the song.
                const auto along =
                    BeatDuration::fromBeats ((e.position.x - (float) gesture->startPosition.x) / editor.getView().pixelsPerBeat);
                slipClip (song, edited, slipStartOffset, editor.snap (gesture->startBeat, e.mods),
                          editor.snap (gesture->startBeat - along, e.mods));
                break;
            }

            case Zone::none:
            default:
                break;
        }

        editor.refresh();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        gesture = nullptr; // keeps the drag's change as its undo step
        dragging = Zone::none;
        setHovered (getZoneAt (e.position));
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const auto zone = getZoneAt (e.position);

        if (! editor.info.has_value())
            return;

        // A bend's handle: straight again, its shape kept for the next bend.
        if (isBend (zone))
        {
            const auto fadeIn = zone == Zone::bendIn;
            auto curve = getCurve (fadeIn);
            curve.bend = 0.0f;
            setFadeCurve (editor.context, editor.clip, fadeIn, curve);
            editor.refresh();
            return;
        }

        // A fade's handle: no fade any more.
        if (zone != Zone::fadeIn && zone != Zone::fadeOut)
            return;

        const auto fadeIn = zone == Zone::fadeIn;
        const auto& range = editor.info->clip.range;
        TimelineGesture step;
        step.beginStep (editor.context, fadeIn ? "Remove Fade In" : "Remove Fade Out");
        fadeClipTo (editor.context, editor.clip, fadeIn, fadeIn ? range.getStart() : range.getEnd());
        step.endStep();
        editor.refresh();
    }

    // The fades' shapes, which the clip's menu in the playlist has too: commands for the selected clips, so the clip is
    // selected first.
    void showMenu()
    {
        auto& selection = editor.context.getSongSelection();

        if (selection.getClips() != std::vector<ClipID> { editor.clip })
        {
            selection.clear();
            selection.add (editor.clip);
        }

        juce::PopupMenu menu;
        menu.addSectionHeader ("Fade Shape");
        addCommandItems (menu, editor.getCommands(), { "clip.fadeLinear", "clip.fadeConvex", "clip.fadeConcave", "clip.fadeSCurve" });
        menu.showMenuAsync (juce::PopupMenu::Options());
    }

    // A bend's handle's: one fade's shape, a curve or an S-curve, bent as far as it is (a straight one all the way, so that
    // the change shows).
    void showShapeMenu (bool fadeIn)
    {
        const auto current = getCurve (fadeIn);
        auto& song = editor.context;
        const auto edited = editor.clip;
        juce::PopupMenu menu;
        menu.addSectionHeader (fadeIn ? "Fade In" : "Fade Out");

        for (const auto& [shape, name] : { std::pair { FadeShape::curve, "Curve" }, std::pair { FadeShape::sCurve, "S-Curve" } })
        {
            const FadeCurve chosen { shape, juce::exactlyEqual (current.bend, 0.0f) ? 1.0f : current.bend };
            menu.addItem (juce::PopupMenu::Item (name)
                              .setTicked (current.shape == shape)
                              .setAction ([&song, edited, fadeIn, chosen] { setFadeCurve (song, edited, fadeIn, chosen); }));
        }

        menu.showMenuAsync (juce::PopupMenu::Options());
    }
};

//==============================================================================
AudioClipEditor::AudioClipEditor (CommandBridge& bridge, ProjectContext& projectContext)
    : TimelineEditor (bridge, projectContext, GridOwner::pianoRoll), // the editor dock's one grid, which the piano roll keeps
      context (projectContext),
      gainField ("Gain", minClipGain, maxClipGain, 0.5, 0.0),
      pitchField ("Pitch", -maxClipPitch, maxClipPitch, 1.0, 0.0),
      fineField ("Fine tuning", fineInfo.minimum, fineInfo.maximum, 1.0, 0.0),
      tempoField ("Clip tempo", minClipTempo, maxClipTempo, 1.0, 120.0),
      waveform (std::make_unique<Waveform> (*this)),
      preparingCheck ([this] { refresh(); })
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Audio clip editor");
    setDescription ("The selected audio clip's whole audio. Drag the clip's edges to trim it, the round handles at its top to fade "
                    "it, the handle in a fade's middle to bend it, and the clip along its audio to play another part of it");

    // The view shows the clip's whole audio, always: no scroll bar, and no turning pages while the song plays.
    setFollowingPlayback (false);
    horizontalScrollBar.setVisible (false);

    showAsParameter (gainField, gainInfo);
    showAsParameter (pitchField, pitchInfo);
    showAsParameter (fineField, fineInfo);
    tempoField.textFromValueFunction = [] (double bpm) { return formatTempo (bpm); };
    tempoField.valueFromTextFunction = [this] (const juce::String& text) { return parseTempo (text).value_or (tempoField.getValue()); };

    gainField.setTooltip ("How loud the clip plays, in decibels: drag it up or down, scroll, or click it and type");
    normalizeButton.setTooltip ("Normalize: sets the gain so that the clip's loudest peak plays at " + juce::String (normalizedPeak, 0)
                                + " dB. Its audio file doesn't change");
    pitchField.setTooltip ("Moves the clip's pitch up or down in semitones, keeping its length");
    fineField.setTooltip ("Tunes the clip's pitch in cents, hundredths of a semitone");
    tempoField.setTooltip ("The tempo the clip's audio was played at. It plays at the song's tempo, so setting this right puts it "
                           "in time");
    reverseButton.setTooltip ("Plays the clip's audio backwards");
    followButton.setTooltip ("Stretches the clip to follow the song's tempo, keeping its pitch. Set the tempo its audio was played at "
                             "below");

    // Each drag of a field is one undo step; a typed value, a notch of the wheel and a key press are one each.
    setUpField (gainField, "Change Clip Gain", [this] { setClipGain (context, clip, (float) gainField.getValue()); });
    setUpField (pitchField, "Change Clip Pitch", [this] { changePitch(); });
    setUpField (fineField, "Change Clip Pitch", [this] { changePitch(); });
    setUpField (tempoField, "Change Clip Tempo", [this] { setClipTempo (context, clip, tempoField.getValue()); });
    normalizeButton.onClick = [this] { normalizeClip (context, clip); };

    reverseButton.setTitle ("Reverse");
    reverseButton.setClickingTogglesState (true);
    reverseButton.onClick = [this] { setClipReversed (context, clip, reverseButton.getToggleState()); };
    followButton.setTitle ("Follow tempo");
    followButton.setClickingTogglesState (true);
    followButton.onClick = [this] { setClipFollowsTempo (context, clip, followButton.getToggleState()); };

    for (auto [label, text] : { std::pair { &gainLabel, "Gain" }, std::pair { &pitchLabel, "Pitch" }, std::pair { &fineLabel, "Fine" },
                                std::pair { &tempoLabel, "Tempo" } })
    {
        label->setText (text, juce::dontSendNotification);
        label->setFont (juce::FontOptions (TextSize::small));
        addAndMakeVisible (label);
    }

    clipLabel.setJustificationType (juce::Justification::centredRight);
    clipLabel.setFont (juce::FontOptions (TextSize::small));

    for (auto* c : std::initializer_list<juce::Component*> { &gainField, &normalizeButton, &pitchField, &fineField, &tempoField,
                                                             &reverseButton, &followButton, &clipLabel, waveform.get() })
        addAndMakeVisible (c);

    context.getSongSelection().addChangeListener (this);
    context.addListener (this);
    projectChanged();
}

AudioClipEditor::~AudioClipEditor()
{
    fieldDrag = nullptr; // ends a drag in progress, keeping its change
    context.removeListener (this);
    context.getSongSelection().removeChangeListener (this);
}

void AudioClipEditor::setUpField (NumberField& field, const juce::String& undoName, std::function<void()> change)
{
    field.updateText();
    field.onDragStart = [this, undoName] { fieldDrag = std::make_unique<SongGesture> (context, undoName); };
    field.onDragEnd = [this] { fieldDrag = nullptr; };
    field.onValueChange = std::move (change);
}

void AudioClipEditor::changePitch()
{
    setClipPitch (context, clip, (float) (pitchField.getValue() + fineField.getValue() / 100.0));
}

//==============================================================================
void AudioClipEditor::projectChanged()
{
    // The same project again (after a save, say) keeps the view as it is.
    if (shownProject == context.getProjectNumber())
        return;

    shownProject = context.getProjectNumber();
    fieldDrag = nullptr;
    showProject();
    setClip ({});
}

void AudioClipEditor::songChanged (const SongChanges& changes)
{
    // The clip and its fades, where it is (a tempo change moves it), and its track's color.
    if (changes.clips || changes.tempo || changes.tracks)
        refresh();
}

void AudioClipEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (const auto toShow = getAudioClipToEdit (context, getPlayheadBeat()); toShow.has_value() && *toShow != clip)
        setClip (*toShow);
}

void AudioClipEditor::showClip (ClipID toShow)
{
    setClip (toShow);
}

ClipID AudioClipEditor::getClip() const
{
    return getAudioClip (context, clip).has_value() ? clip : ClipID();
}

void AudioClipEditor::setClip (ClipID toShow)
{
    fieldDrag = nullptr;
    clip = getAudioClip (context, toShow).has_value() ? toShow : ClipID();
    refresh();
}

void AudioClipEditor::refresh()
{
    info = clip.isValid() ? getAudioClip (context, clip) : std::nullopt;
    updateControls();
    fitView(); // wherever the clip and its audio are now in the song
    waveform->clipChanged();

    // Nothing tells the editor when the audio's render is ready: look again until it is.
    if (info.has_value() && info->state == AudioState::preparing)
    {
        if (! preparingCheck.isTimerRunning())
            preparingCheck.startTimerHz (4);
    }
    else
    {
        preparingCheck.stopTimer();
    }
}

bool AudioClipEditor::isTypingInField() const
{
    const auto* focused = juce::Component::getCurrentlyFocusedComponent();

    if (dynamic_cast<const juce::TextEditor*> (focused) == nullptr)
        return false;

    for (const auto* field : { &gainField, &pitchField, &fineField, &tempoField })
        if (field->isParentOf (focused))
            return true;

    return false;
}

void AudioClipEditor::updateControls()
{
    const auto shown = info.has_value();

    for (auto* c : std::initializer_list<juce::Component*> { &gainField, &pitchField, &fineField, &reverseButton, &followButton })
        c->setEnabled (shown);

    tempoField.setEnabled (shown && info->followsTempo);
    tempoLabel.setEnabled (tempoField.isEnabled());
    normalizeButton.setEnabled (shown && info->state != AudioState::missing); // which it reads the peak of

    if (! shown)
    {
        clipLabel.setText ({}, juce::dontSendNotification);
        return;
    }

    // Not while a field is dragged or typed in: what it shows is what is being set.
    if (fieldDrag == nullptr && ! isTypingInField())
    {
        const auto [semitones, cents] = splitPitch (info->pitch);
        gainField.setValue (info->gain, juce::dontSendNotification);
        pitchField.setValue (semitones, juce::dontSendNotification);
        fineField.setValue (cents, juce::dontSendNotification);

        if (info->followsTempo)
            tempoField.setValue (info->tempo, juce::dontSendNotification);
    }

    reverseButton.setToggleState (info->reversed, juce::dontSendNotification);
    followButton.setToggleState (info->followsTempo, juce::dontSendNotification);

    // Which clip, where in the song, and its file.
    const auto row = getTrackRow (context, info->clip.track);
    auto text = info->clip.name + (row.has_value() ? " on " + row->name : juce::String()) + ", at bar "
                + juce::String (getBarAt (context, info->clip.range.getStart()) + 1) + " of the song";

    if (info->state != AudioState::missing)
        text << "  |  " << info->fileName << ", " << formatSampleRate (info->sampleRate) << " " << describeChannels (info->channels) << ", "
             << formatDuration (info->seconds);

    clipLabel.setText (text, juce::dontSendNotification);
}

void AudioClipEditor::fitView()
{
    if (! info.has_value() || waveform->getWidth() <= 0)
        return;

    const auto shown = getContentRange();
    const auto margin = shown.getLength() * viewMargin;
    zoomTo ({ shown.getStart() - margin, shown.getEnd() + margin });
}

//==============================================================================
juce::Rectangle<int> AudioClipEditor::getHandleBounds (Handle handle) const
{
    if (! info.has_value())
        return {};

    const auto around = [] (juce::Point<float> centre, float radius)
    { return juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre).toNearestInt(); };

    switch (handle)
    {
        case Handle::trimStart:
            return waveform->getGrip (true).toNearestInt();
        case Handle::trimEnd:
            return waveform->getGrip (false).toNearestInt();
        case Handle::fadeIn:
            return around (waveform->getFadeHandle (true), AudioClipTint::handleRadius);
        case Handle::fadeOut:
            return around (waveform->getFadeHandle (false), AudioClipTint::handleRadius);
        case Handle::bendIn:
        case Handle::bendOut:
        default:
            if (const auto centre = waveform->getBendHandle (handle == Handle::bendIn))
                return around (*centre, AudioClipTint::bendRadius);

            return {};
    }
}

juce::Component& AudioClipEditor::getWaveformComponent() noexcept
{
    return *waveform;
}

juce::Rectangle<int> AudioClipEditor::getTimelineArea() const
{
    return waveform->getBounds();
}

BeatRange AudioClipEditor::getContentRange() const
{
    if (! info.has_value())
        return { getTimeOrigin(), getTimeOrigin() + BeatDuration::fromBeats (16.0) };

    // The clip and all of its audio.
    const auto& clipRange = info->clip.range;
    return { std::max (BeatPosition(), std::min (clipRange.getStart(), info->content.getStart())),
             std::max (clipRange.getEnd(), info->content.getEnd()) };
}

void AudioClipEditor::timelineChanged()
{
    waveform->repaint();
}

//==============================================================================
void AudioClipEditor::paint (juce::Graphics& g)
{
    paintEditorBackground (g);

    // The settings' panel, down the left under the toolbar.
    const auto& p = getPalette (*this);
    g.setColour (p.border);
    g.fillRect (panelWidth - 1, toolbarHeight, 1, getHeight() - toolbarHeight);
}

void AudioClipEditor::resized()
{
    auto area = getLocalBounds();
    const auto toolbar = layOutToolbarControls (area.removeFromTop (toolbarHeight).reduced (Space::s, Space::xs));
    clipLabel.setBounds (toolbar.withTrimmedLeft (Space::s));

    auto panel = area.removeFromLeft (panelWidth).reduced (Space::s);

    const auto nextRow = [&panel]
    {
        const auto row = panel.removeFromTop (controlHeight);
        panel.removeFromTop (Space::xs);
        return row;
    };

    const auto layOutField = [] (juce::Label& label, juce::Component& field, juce::Rectangle<int> row)
    {
        label.setBounds (row.removeFromLeft (labelWidth));
        field.setBounds (row);
    };

    // Normalize at the gain's right, which it sets.
    auto gainRow = nextRow();
    normalizeButton.setBounds (gainRow.removeFromRight (controlHeight));
    gainRow.removeFromRight (Space::xs);
    layOutField (gainLabel, gainField, gainRow);
    layOutField (pitchLabel, pitchField, nextRow());
    layOutField (fineLabel, fineField, nextRow());
    reverseButton.setBounds (nextRow());
    followButton.setBounds (nextRow());
    layOutField (tempoLabel, tempoField, nextRow());

    ruler.setBounds (area.removeFromTop (rulerHeight));
    waveform->setBounds (area);

    if (info.has_value())
        fitView();
    else
        setViewStart (getView().start);
}
} // namespace phazite::ui
