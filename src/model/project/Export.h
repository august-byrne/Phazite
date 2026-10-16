// SPDX-License-Identifier: AGPL-3.0-or-later
// Export (docs/design.md, section 18; roadmap step 1.8): the song rendered to a WAV or FLAC file, through the master's
// devices, from its start to its end (getSongEnd()) with the tails of notes and effects after it, and normalized to an
// integrated loudness (EBU R128, ITU-R BS.1770) if asked, never pushing its true peak over a ceiling. Tracktion's
// renderer does the work, into a temporary file next to the chosen one, which replaces it only when it is complete; the
// file is then measured again, for the report.
#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <optional>

namespace phazite
{
struct ExportSettings
{
    enum class Format
    {
        wav,
        flac
    };

    Format format = Format::wav;
    double sampleRate = 44100.0;
    int bitDepth = 24;                    // 16 or 24 (dithered when 16), or 32 for WAV's floating point
    std::optional<double> loudnessTarget; // integrated LUFS; none leaves the mix as it is
    double truePeakCeiling = -1.0;        // dBTP, which normalizing never pushes the true peak over
};

// The choices offered: sample rates, bit depths, and the range of loudness targets and ceilings.
inline constexpr double exportSampleRates[] { 44100.0, 48000.0, 88200.0, 96000.0 };
inline constexpr int exportBitDepths[] { 16, 24, 32 };
inline constexpr double minLoudnessTarget = -30.0, maxLoudnessTarget = -5.0;
inline constexpr double minTruePeakCeiling = -6.0, maxTruePeakCeiling = 0.0;

// Whether the settings can be written: a sample rate and bit depth offered (32 bits for WAV only), a loudness target and
// a ceiling in their ranges.
bool isValid (const ExportSettings&);

// ".wav" or ".flac".
juce::String getFileSuffix (ExportSettings::Format);

// What an export wrote: its file, measured again, or why there is none.
struct ExportReport
{
    juce::Result result = juce::Result::ok();
    juce::File file;
    double seconds = 0.0;
    double integratedLufs = -100.0; // -100 for silence, or for less than 400 ms of sound
    double truePeakDb = -100.0;
};

// How many LU the file is under the loudness target because the true-peak ceiling held its gain back (as Tracktion's
// normalizing does: it never limits), or 0 when it met the target, within half an LU, or had none.
double getLoudnessShortfall (const ExportSettings&, const ExportReport&);

// The song's time that an export covers is from its start to its end; the tails after it last until they fall silent
// (under -70 dB for a block of 512 samples), up to tailSeconds (internal/Export.h: getExportRange()).
inline constexpr double tailSeconds = 10.0;

// An export in progress, rendering on a thread of its own while this lives (Tracktion's EditRenderer; internal/Export.h
// starts one). It stops playback first, and lets the project play again when it is done. The report comes on the message
// thread, once, after the render thread has finished; deleting this first cancels the export, without a report, and
// leaves any file that was there. Delete it on the message thread; the project must outlive it.
class SongExport
{
public:
    ~SongExport();

    float getProgress() const; // from 0 to 1

    struct Impl; // the render, and what its thread shares with this (internal/Export.h)
    explicit SongExport (std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (SongExport)
};
} // namespace phazite
