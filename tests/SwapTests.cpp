// SPDX-License-Identifier: AGPL-3.0-or-later
// Swap (docs/design.md, section 11; model/devices/SwapModel.h): a drum pad's sound and a device's preset tried in place in
// the song, then kept as one undo step or put back; and the undo convention's Trial under it, which keeps what it tried as
// soon as anything else changes the song.
#include "TestSong.h"
#include "devices/drums/DrumSampler.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/automation/AutomationModel.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/SwapModel.h"
#include "model/mixer/MixerModel.h"
#include "model/presets/PresetModel.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

using phazite::test::TestSong;

namespace
{
phazite::Song::Ptr newSong()
{
    auto song = phazite::createSong();
    song->getUndoManager().clearUndoHistory();
    return song;
}

// A change to the song, as the model makes one, to its own element.
void change (phazite::Song& song, const char* property, int value)
{
    song.getState().setProperty (property, value, &song.getUndoManager());
}

int valueOf (const phazite::Song& song, const char* property)
{
    return (int) song.getState().getProperty (property, -1);
}
} // namespace

TEST_CASE ("A trial's changes are one step, kept under a name of its own or put back with nothing to redo")
{
    auto song = newSong();
    auto& undo = song->getUndoManager();
    change (*song, "before", 1);

    SUBCASE ("Kept: one step, with the name it was kept by")
    {
        phazite::Trial trial (*song, "Try", nullptr);
        CHECK_FALSE (trial.hasChanges());
        trial.apply ([&] { change (*song, "tried", 1); });
        trial.apply ([&] { change (*song, "tried", 2); });
        CHECK (phazite::Trial::isInProgress (*song));
        trial.keep ("Kept It");
        CHECK_FALSE (trial.isOpen());
        CHECK_FALSE (phazite::Trial::isInProgress (*song));
        CHECK (undo.getUndoDescription() == juce::String ("Kept It"));
        phazite::undo (*song);
        CHECK (valueOf (*song, "tried") == -1);
        CHECK (valueOf (*song, "before") == 1);
    }

    SUBCASE ("Put back: as it was, the step before it still to undo")
    {
        phazite::Trial trial (*song, "Try", nullptr);
        trial.apply ([&] { change (*song, "tried", 1); });
        trial.putBack();
        CHECK (valueOf (*song, "tried") == -1);
        CHECK_FALSE (undo.canRedo());
        phazite::undo (*song);
        CHECK (valueOf (*song, "before") == -1);
    }

    SUBCASE ("From the start: what was tried before is taken back first")
    {
        phazite::Trial trial (*song, "Try", nullptr);
        trial.apply ([&] { change (*song, "a", 1); });
        trial.apply ([&] { change (*song, "b", 1); }, true);
        CHECK (valueOf (*song, "a") == -1);
        CHECK (valueOf (*song, "b") == 1);
        trial.keep ("B");
        CHECK (undo.getNumActionsInCurrentTransaction() == 1);
    }

    SUBCASE ("Steps begun in a change it applies join it")
    {
        phazite::Trial trial (*song, "Try", nullptr);
        trial.apply (
            [&]
            {
                const phazite::UndoStep step (*song, "Inner");
                change (*song, "inner", 1);
            });
        trial.keep ("Outer");
        CHECK (undo.getUndoDescription() == juce::String ("Outer"));
    }

    SUBCASE ("Destroyed while open, it puts back what it tried")
    {
        {
            phazite::Trial trial (*song, "Try", nullptr);
            trial.apply ([&] { change (*song, "tried", 1); });
        }

        CHECK (valueOf (*song, "tried") == -1);
    }
}

TEST_CASE ("A trial gives way: any other step, gesture, recording or undo keeps what it tried first")
{
    auto song = newSong();
    auto& undo = song->getUndoManager();
    auto keptFirst = 0;
    std::unique_ptr<phazite::Trial> trial;
    const auto open = [&]
    {
        trial = std::make_unique<phazite::Trial> (*song, "Try",
                                                  [&]
                                                  {
                                                      ++keptFirst;
                                                      trial->keep ("Tried");
                                                  });
        trial->apply ([&] { change (*song, "tried", 1); });
    };

    SUBCASE ("A step: two steps, the trial's first")
    {
        open();

        {
            const phazite::UndoStep step (*song, "Other");
            change (*song, "other", 1);
        }

        CHECK (keptFirst == 1);
        CHECK_FALSE (trial->isOpen());
        CHECK (undo.getUndoDescription() == juce::String ("Other"));
        phazite::undo (*song);
        CHECK (valueOf (*song, "tried") == 1);
        CHECK (undo.getUndoDescription() == juce::String ("Tried"));
    }

    SUBCASE ("A gesture")
    {
        open();
        phazite::Gesture gesture (*song, "Drag");
        change (*song, "dragged", 1);
        gesture.commit();
        CHECK (keptFirst == 1);
        CHECK (undo.getUndoDescription() == juce::String ("Drag"));
    }

    SUBCASE ("An undo takes back what was tried, which can be redone")
    {
        open();
        phazite::undo (*song);
        CHECK (keptFirst == 1);
        CHECK (valueOf (*song, "tried") == -1);
        CHECK (undo.getRedoDescription() == juce::String ("Tried"));
        phazite::redo (*song);
        CHECK (valueOf (*song, "tried") == 1);
    }

    SUBCASE ("Before it changes anything, a step leaves it open, to begin again at its first change")
    {
        trial = std::make_unique<phazite::Trial> (*song, "Try", [&] { ++keptFirst; });

        {
            const phazite::UndoStep step (*song, "Other");
            change (*song, "other", 1);
        }

        CHECK (trial->isOpen());
        trial->apply ([&] { change (*song, "tried", 1); });
        trial->keep ("Tried");
        CHECK (keptFirst == 0);
        CHECK (undo.getUndoDescription() == juce::String ("Tried"));
        phazite::undo (*song);
        CHECK (valueOf (*song, "other") == 1); // the other step is a step of its own
    }
}

TEST_CASE ("Swap tries sounds on a drum pad and keeps one as one undo step, copied into the project only then")
{
    TestSong song;
    const auto drums = song.setInstrument (song.firstTrack, phazite::devices::DrumSampler::typeID);
    const auto one = phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Kick One.wav"), 0.2);
    const auto two = phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Kick Two.wav"), 0.3);
    const auto handle = phazite::getDeviceHandle (song.context, drums);
    const auto audio = song.getProject().getFile().getSiblingFile ("Audio");
    auto& undo = *song.context.getUndoManager();
    const auto before = handle.getPadName (36);

    {
        phazite::Swap swap (song.context, drums, 36);
        REQUIRE (swap.isOpen());
        CHECK (swap.getTargetName() == juce::String ("Pad 36"));
        CHECK (swap.getOldName() == before);
        CHECK (handle.getSwap() == std::optional<int> (36)); // the editor lights the pad
        CHECK (phazite::findOpenSwap (song.context, drums) == std::optional<int> (36));

        // Each sound in its place, played from where it is; a preset or anything else isn't a sound to try.
        CHECK (swap.canTry (one));
        CHECK_FALSE (swap.canTry (song.getSoundsFolder().getChildFile ("Missing.wav")));
        CHECK (swap.tryFile (one));
        CHECK (handle.getPadName (36) == juce::String ("Kick One"));
        CHECK (swap.tryFile (two));
        CHECK (handle.getPadName (36) == juce::String ("Kick Two"));
        CHECK (swap.getTried() == two);
        CHECK_FALSE (audio.getChildFile ("Kick Two.wav").existsAsFile());
        CHECK (undo.getUndoDescription() == juce::String ("Put Kick Two on Pad 36"));

        swap.keep();
        CHECK_FALSE (swap.isOpen());
        CHECK_FALSE (handle.getSwap().has_value());
    }

    // One step, its sound copied into the project as a sound put on a pad is; undone, the old one is back.
    CHECK (handle.getPadName (36) == juce::String ("Kick Two"));
    CHECK (handle.hasPadSound (36));
    CHECK (audio.getChildFile ("Kick Two.wav").existsAsFile());
    CHECK_FALSE (audio.getChildFile ("Kick One.wav").existsAsFile());
    CHECK (undo.getUndoDescription() == juce::String ("Put Kick Two on Pad 36"));
    phazite::undo (*phazite::findSong (song.context));
    CHECK (handle.getPadName (36) == before);

    // Put back: as it was, and what was there to redo is there still.
    {
        phazite::Swap swap (song.context, drums, 36);
        swap.tryFile (one);
        swap.putBack();
        CHECK_FALSE (swap.isOpen());
    }

    CHECK (handle.getPadName (36) == before);
    CHECK (undo.getRedoDescription() == juce::String ("Put Kick Two on Pad 36"));

    // A swap ended without either puts back what it tried.
    {
        phazite::Swap swap (song.context, drums, 36);
        swap.tryFile (one);
    }

    CHECK (handle.getPadName (36) == before);
}

TEST_CASE ("Anything else that changes the song keeps what a swap tries, as a step of its own, and ends the swap")
{
    TestSong song;
    const auto drums = song.setInstrument (song.firstTrack, phazite::devices::DrumSampler::typeID);
    const auto kick = phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Kick One.wav"), 0.2);
    const auto handle = phazite::getDeviceHandle (song.context, drums);
    const auto before = handle.getPadName (36);
    auto& undo = *song.context.getUndoManager();
    auto& model = *phazite::findSong (song.context);

    SUBCASE ("Another change")
    {
        phazite::Swap swap (song.context, drums, 36);
        swap.tryFile (kick);
        song.rename (song.firstTrack, "Beat");
        CHECK_FALSE (swap.isOpen());
        CHECK (handle.getPadName (36) == juce::String ("Kick One"));

        phazite::undo (model); // the rename
        CHECK (handle.getPadName (36) == juce::String ("Kick One"));
        CHECK (undo.getUndoDescription() == juce::String ("Put Kick One on Pad 36"));
        phazite::undo (model);
        CHECK (handle.getPadName (36) == before);
    }

    SUBCASE ("An undo, which then takes it back")
    {
        phazite::Swap swap (song.context, drums, 36);
        swap.tryFile (kick);
        phazite::undo (model);
        CHECK_FALSE (swap.isOpen());
        CHECK (handle.getPadName (36) == before);
        CHECK (undo.getRedoDescription() == juce::String ("Put Kick One on Pad 36"));
    }

    SUBCASE ("A save, which saves it kept, copied into the project")
    {
        phazite::Swap swap (song.context, drums, 36);
        swap.tryFile (kick);
        REQUIRE (song.getProject().save().wasOk());
        CHECK_FALSE (swap.isOpen());
        CHECK (song.getProject().getFile().getSiblingFile ("Audio").getChildFile ("Kick One.wav").existsAsFile());
    }

    SUBCASE ("Its device going")
    {
        phazite::Swap swap (song.context, drums, 36);
        song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
        CHECK_FALSE (swap.isOpen());
        CHECK_FALSE (swap.tryFile (kick));
    }
}

TEST_CASE ("Swap tries presets on one of Phazite's own devices, and the device is as it was once put back")
{
    TestSong song;
    const auto synth = song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
    auto pitch = phazite::getParameter (song.context, { synth, "osc2Pitch" });
    const auto folder = song.getSoundsFolder();

    // Two presets of the synth, a fifth up and a fourth down, and one of another device.
    song.setParameter (synth, "osc2Pitch", 7.0f);
    REQUIRE (phazite::saveDevicePreset (song.context, synth, folder.getChildFile ("Fifth.phazitepreset")));
    song.setParameter (synth, "osc2Pitch", -5.0f);
    REQUIRE (phazite::saveDevicePreset (song.context, synth, folder.getChildFile ("Fourth.phazitepreset")));
    const auto eq = song.addEffect (song.firstTrack, phazite::devices::Equalizer::typeID);
    REQUIRE (phazite::saveDevicePreset (song.context, eq, folder.getChildFile ("Bright.phazitepreset")));
    song.setParameter (synth, "osc2Pitch", 12.0f);
    auto& undo = *song.context.getUndoManager();

    {
        phazite::Swap swap (song.context, synth);
        REQUIRE (swap.isOpen());
        CHECK (swap.getPad() == -1);
        CHECK (swap.getTargetName() == juce::String ("Synth"));
        CHECK (swap.canTry (folder.getChildFile ("Fifth.phazitepreset")));
        CHECK_FALSE (swap.canTry (folder.getChildFile ("Bright.phazitepreset")));

        CHECK (swap.tryFile (folder.getChildFile ("Fifth.phazitepreset")));
        CHECK (pitch.getValue() == doctest::Approx (7.0f));
        CHECK (swap.tryFile (folder.getChildFile ("Fourth.phazitepreset")));
        CHECK (pitch.getValue() == doctest::Approx (-5.0f));
        swap.putBack();
    }

    CHECK (pitch.getValue() == doctest::Approx (12.0f));

    // Kept: one step, named after the preset, which undo takes back.
    {
        phazite::Swap swap (song.context, synth);
        swap.tryFile (folder.getChildFile ("Fourth.phazitepreset"));
        swap.tryFile (folder.getChildFile ("Fifth.phazitepreset"));
        swap.keep();
    }

    CHECK (pitch.getValue() == doctest::Approx (7.0f));
    CHECK (undo.getUndoDescription() == juce::String ("Load Preset Fifth"));
    phazite::undo (*phazite::findSong (song.context));
    CHECK (pitch.getValue() == doctest::Approx (12.0f));
}

TEST_CASE ("A swap is in progress for Auto Save, which waits for it, once it has tried something and until it ends")
{
    TestSong song;
    const auto drums = song.setInstrument (song.firstTrack, phazite::devices::DrumSampler::typeID);
    const auto kick = phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Kick One.wav"), 0.2);
    auto& model = *phazite::findSong (song.context);

    phazite::Swap swap (song.context, drums, 36);
    CHECK_FALSE (phazite::Trial::isInProgress (model)); // nothing tried yet
    swap.tryFile (kick);
    CHECK (phazite::Trial::isInProgress (model));
    swap.keep();
    CHECK_FALSE (phazite::Trial::isInProgress (model));
}
