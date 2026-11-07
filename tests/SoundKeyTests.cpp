// SPDX-License-Identifier: AGPL-3.0-or-later
// Sounds' keys (docs/design.md, section 11), read from their names as sample packs write them and from their loop data: a
// WAV file's acid chunk names a root note, an AIFF file's Apple loop data a root and minor or major.
#include "TestHelpers.h"
#include "model/engine/AudioFiles.h"
#include "services/Library.h"

namespace
{
using phazite::SoundKey;
using Mode = SoundKey::Mode;

std::optional<SoundKey> keyOf (const juce::String& name)
{
    return phazite::getKeyFromName (name);
}

juce::String describe (const juce::String& name)
{
    const auto key = keyOf (name);
    return key.has_value() ? key->describe() : juce::String ("none");
}

// A second of silence in a WAV file, with these loop data.
juce::File writeWav (const juce::File& file, const std::unordered_map<juce::String, juce::String>& data)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::AudioBuffer<float> silence (1, 44100);
    silence.clear();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    if (auto writer = juce::WavAudioFormat().createWriterFor (
            stream,
            juce::AudioFormatWriterOptions {}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16).withMetadataValues (
                data)))
        writer->writeFromAudioSampleBuffer (silence, 0, silence.getNumSamples());

    return file;
}

// A second of silence in an AIFF file, with Apple loop data (a "basc" chunk, as Logic's loops have) added after its sound:
// its beats, its root note (0 for none) and its scale (1 minor, 2 major, 3 neither).
juce::File writeAppleLoop (const juce::File& file, int beats, int rootNote, int scale)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();

    {
        juce::AudioBuffer<float> silence (1, 44100);
        silence.clear();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

        if (auto writer = juce::AiffAudioFormat().createWriterFor (
                stream, juce::AudioFormatWriterOptions {}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16)))
            writer->writeFromAudioSampleBuffer (silence, 0, silence.getNumSamples());
    }

    juce::MemoryBlock bytes;
    file.loadFileAsData (bytes);
    juce::MemoryOutputStream out (bytes, true);
    out.writeText ("basc", false, false, nullptr);
    out.writeIntBigEndian (84);
    out.writeIntBigEndian (1);     // flags
    out.writeIntBigEndian (beats); // beats
    out.writeShortBigEndian ((short) rootNote);
    out.writeShortBigEndian ((short) scale);
    out.writeShortBigEndian (4); // time signature 4/4
    out.writeShortBigEndian (4);
    out.writeShortBigEndian (1); // a loop, not a one-shot (2)
    out.writeRepeatedByte (0, 66);
    out.flush();

    // The FORM chunk holds the new one too.
    auto* data = static_cast<char*> (bytes.getData());
    const auto formSize = juce::ByteOrder::swapIfLittleEndian ((juce::uint32) (bytes.getSize() - 8));
    std::memcpy (data + 4, &formSize, 4);
    file.replaceWithData (bytes.getData(), bytes.getSize());
    return file;
}
} // namespace

TEST_CASE ("A sound's key is read from its name as sample packs write it")
{
    // With minor or major: after the note, or as the next word; in sharps as Phazite names keys.
    CHECK (describe ("Bass Loop Am 120 BPM") == "A minor");
    CHECK (describe ("LS_FHT_120_Bass_Loop_Am") == "A minor");
    CHECK (describe ("Cymatics - Lofi Piano Loop 1 - 85 BPM F# Min") == "F# minor");
    CHECK (describe ("KSHMR_Piano_Chords_04_Bbm_128") == "A# minor");
    CHECK (describe ("Synth_Loop_128_Cmaj") == "C major");
    CHECK (describe ("pad_Gm_90bpm") == "G minor");
    CHECK (describe ("Keys - E minor - 100 BPM") == "E minor");
    CHECK (describe ("Piano Chord Ebmaj7") == "D# major");
    CHECK (describe ("Pad C#min") == "C# minor");
    CHECK (describe ("Lead F Sharp Minor") == "F# minor");
    CHECK (describe ("PadAm") == "A minor");
    CHECK (describe ("BASS_LOOP_AMIN_120") == "A minor");
    CHECK (describe ("BASS_LOOP_F#M") == "F# minor");
    CHECK (describe ("bass_loop_ebm_140") == "D# minor");

    // A note alone, where the name sets it apart: the last word, in brackets, with a sharp or a flat, after "key".
    CHECK (describe ("808 Long F") == "F");
    CHECK (describe ("KSHMR Kick 05 (F)") == "F");
    CHECK (describe ("Cymatics - Titan Kick 1 - F") == "F");
    CHECK (describe ("808_C#_Sub") == "C#");
    CHECK (describe ("Melody (Key of G) 90 BPM") == "G");
    CHECK (describe ("kick_f") == "F");

    // A minor or major key wins over a note before it; the first of either wins otherwise.
    CHECK (describe ("Kick (F) Bass Am") == "A minor");
    CHECK (describe ("Chords Am F C G") == "A minor");

    // Not a key: words that start with a note, FM and AM, a note in the middle of a name, decibels, chords without minor or
    // major, a tempo.
    for (const auto* name : { "Ambient Bass", "Deep Kick", "FM Bass Loop", "AM Radio Noise", "Bass C Long", "Kick -3dB", "kick_-6db",
                              "Asus2 Pad", "Break 120 BPM", "Cdim Stab 2", "BD 909", "Ebb and Flow" })
    {
        INFO (std::string (name));
        CHECK_FALSE (keyOf (name).has_value());
    }

    // A folder's name counts only for a key with minor or major (a folder called "A" is likely just a letter).
    CHECK (phazite::getKeyFromName ("F# Minor", false) == SoundKey { 6, Mode::minor });
    CHECK (phazite::getKeyFromName ("Am Loops", false) == SoundKey { 9, Mode::minor });
    CHECK_FALSE (phazite::getKeyFromName ("A", false).has_value());
    CHECK_FALSE (phazite::getKeyFromName ("C#", false).has_value());
}

TEST_CASE ("Keys are described in full and in short, and sort by their roots")
{
    CHECK (SoundKey { 6, Mode::minor }.describe() == "F# minor");
    CHECK (SoundKey { 0, Mode::major }.describe() == "C major");
    CHECK (SoundKey { 5, Mode::note }.describe() == "F");
    CHECK (SoundKey { 6, Mode::minor }.getShortName() == "F#m");
    CHECK (SoundKey { 0, Mode::major }.getShortName() == "Cmaj");
    CHECK (SoundKey { 5, Mode::note }.getShortName() == "F");

    CHECK (SoundKey { 0, Mode::minor } < SoundKey { 1, Mode::note });
    CHECK (SoundKey { 5, Mode::note } < SoundKey { 5, Mode::major });
    CHECK (SoundKey { 5, Mode::major } < SoundKey { 5, Mode::minor });
}

TEST_CASE ("Loop data name a sound's root note, and Apple loop data minor or major")
{
    phazite::test::TemporaryFolder temp;

    // A WAV file's acid chunk: a loop of eight beats on A (57), whatever its name says of it.
    const auto wav =
        phazite::getAudioFileInfo (writeWav (temp.folder.getChildFile ("Pad.wav"), { { juce::WavAudioFormat::acidBeats, "8" },
                                                                                     { juce::WavAudioFormat::acidOneShot, "0" },
                                                                                     { juce::WavAudioFormat::acidRootSet, "1" },
                                                                                     { juce::WavAudioFormat::acidRootNote, "57" } }));
    CHECK (wav.loopData == phazite::AudioFileInfo::LoopData::loop);
    CHECK (wav.loopBeats == doctest::Approx (8.0));
    CHECK (wav.key == SoundKey { 9, Mode::note });

    // One that plays once, without a root.
    const auto hit = phazite::getAudioFileInfo (writeWav (
        temp.folder.getChildFile ("Hit.wav"), { { juce::WavAudioFormat::acidBeats, "0" }, { juce::WavAudioFormat::acidOneShot, "1" } }));
    CHECK (hit.loopData == phazite::AudioFileInfo::LoopData::oneShot);
    CHECK_FALSE (hit.key.has_value());

    // An AIFF file's Apple loop data: four beats on D (62), minor.
    const auto aiff = phazite::getAudioFileInfo (writeAppleLoop (temp.folder.getChildFile ("Guitar.aif"), 4, 62, 1));
    CHECK (aiff.loopData == phazite::AudioFileInfo::LoopData::loop);
    CHECK (aiff.loopBeats == doctest::Approx (4.0));
    CHECK (aiff.key == SoundKey { 2, Mode::minor });

    // And none: no loop data at all.
    CHECK (phazite::getAudioFileInfo (phazite::test::writeSound (temp.folder.getChildFile ("Kick.wav"), 0.3)).loopData
           == phazite::AudioFileInfo::LoopData::none);
}

TEST_CASE ("A sound's tags name its key, from its name, a folder's or its loop data")
{
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("PhaziteKeys");
    const auto tagsOf = [&root] (const char* path, const phazite::SoundFacts* facts = nullptr)
    { return phazite::getSoundTags (root.getChildFile (path), root, facts); };

    CHECK (tagsOf ("808s/808 Long F.wav") == juce::StringArray { "Bass", "One-shot", "F" });
    CHECK (tagsOf ("Loops/C Minor/Keys 90 BPM.wav") == juce::StringArray { "Synth", "Loop", "90 BPM", "C minor" });
    CHECK (phazite::getSoundKey (root.getChildFile ("Loops/C Minor/Keys 90 BPM.wav"), root) == SoundKey { 0, Mode::minor });

    // Loop data make a sound a loop with a tempo and a key that its name doesn't give: eight beats in four seconds.
    phazite::SoundFacts facts;
    facts.loopData = phazite::AudioFileInfo::LoopData::loop;
    facts.loopBeats = 8.0;
    facts.seconds = 4.0;
    facts.key = SoundKey { 9, Mode::minor };
    CHECK (tagsOf ("Guitar.wav", &facts) == juce::StringArray { "Loop", "120 BPM", "A minor" });

    // Its name's key and tempo go first; loop data that say it plays once make it a one-shot.
    CHECK (tagsOf ("Guitar Em 100 BPM.wav", &facts) == juce::StringArray { "Loop", "100 BPM", "E minor" });
    facts.loopData = phazite::AudioFileInfo::LoopData::oneShot;
    facts.key.reset();
    CHECK (tagsOf ("Guitar Loop.wav", &facts) == juce::StringArray { "One-shot" });

    CHECK (phazite::getSoundTypesIn ("808 Kick") == juce::StringArray { "Kick" });
    CHECK (phazite::getSoundTypesIn ("Closed Hat") == juce::StringArray { "Hat" });
    CHECK (phazite::getSoundTypesIn ("808") == juce::StringArray { "Bass" });
}
