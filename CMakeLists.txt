# Phazite: an open-source DAW (AGPL-3.0) built on JUCE.
#
# Configure and build through the presets, with the same two commands on every OS:
#   cmake --preset windows-debug
#   cmake --build --preset windows-debug
# `cmake --list-presets` shows the presets available on the current machine.

cmake_minimum_required(VERSION 3.28...4.4)

project(Phazite VERSION 0.1.0 LANGUAGES C CXX)   # JUCE needs the C language enabled as well

include("${CMAKE_CURRENT_LIST_DIR}/cmake/PhaziteHelpers.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/PhaziteNotices.cmake")

# Phazite's copyright notice, and where its source code and its releases are: the About box, the installer, the app's version
# information, NOTICES.txt and the update check say them (src/model/engine/AppInfo.h, src/services/Updates.h).
set(PHAZITE_COPYRIGHT "Copyright (C) 2026 the Phazite contributors")
set(PHAZITE_SOURCE_URL "https://github.com/august-byrne/Phazite")

if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR "On Windows, configure through the windows-* presets (MSVC). "
                        "This configure found ${CMAKE_CXX_COMPILER_ID} instead.")
endif()

# --- MSVC settings. They only affect targets created after this point; other compilers ignore them. ---
# Static C runtime: the app runs without a VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Debug info inside each .obj (/Z7) instead of one shared .pdb that parallel compiles queue up for.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
# Phazite uses no C++20 modules, so its files aren't scanned for them: the scan reads each file through the preprocessor
# once more before compiling it, and without the compiler cache that CI uses (ccache).
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(MSVC)
    message(STATUS "Phazite: MSVC ${CMAKE_CXX_COMPILER_VERSION} from ${VS_INSTALLATION_PATH}, "
                   "Windows SDK ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")

    # RelWithDebInfo (the windows-profile preset) is Release with symbols: it inlines as much as Release does (CMake's
    # default for it is /Ob1) and links as Release does (CMake's default links it incrementally, which sends every call
    # through a jump stub and turns /OPT off), so that it plays, and measures, as Release does.
    string(REPLACE "/Ob1" "/Ob2" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
    string(REPLACE "/Ob1" "/Ob2" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")

    foreach(_phazite_kind EXE SHARED MODULE)
        set(_phazite_flags "${CMAKE_${_phazite_kind}_LINKER_FLAGS_RELWITHDEBINFO}")

        if(NOT _phazite_flags MATCHES "/INCREMENTAL:NO")
            string(REGEX REPLACE "/INCREMENTAL(:YES)?" "/INCREMENTAL:NO" _phazite_flags "${_phazite_flags}")
        endif()

        set(CMAKE_${_phazite_kind}_LINKER_FLAGS_RELWITHDEBINFO "${_phazite_flags} /OPT:REF /OPT:ICF")
    endforeach()
endif()

# JUCE builds its "juceaide" helper during configure with a second, nested CMake run that is not given
# our toolchain file. CMake reads the CMAKE_TOOLCHAIN_FILE environment variable when it creates a new
# build folder, so exporting it lets that nested run find MSVC too. CMakePresets.json sets the same
# variable; this also covers configures that don't go through the presets.
if(CMAKE_HOST_WIN32 AND CMAKE_TOOLCHAIN_FILE AND IS_ABSOLUTE "${CMAKE_TOOLCHAIN_FILE}")
    set(ENV{CMAKE_TOOLCHAIN_FILE} "${CMAKE_TOOLCHAIN_FILE}")
endif()

set(PHAZITE_MIB_PER_COMPILE_JOB 2560 CACHE STRING
    "Peak memory (MiB) assumed per compile job when limiting parallel compiles")
phazite_limit_parallel_compiles(${PHAZITE_MIB_PER_COMPILE_JOB})

# --- Dependencies: JUCE, and doctest for the tests. ---
phazite_require_submodules(external/JUCE external/doctest)

set(_phazite_juceaide_dir "${CMAKE_BINARY_DIR}/external/JUCE/tools")
phazite_reset_stale_juceaide("${_phazite_juceaide_dir}")

add_subdirectory(external/JUCE EXCLUDE_FROM_ALL)   # keeps its install rules out of ours

phazite_check_juceaide_toolchain("${_phazite_juceaide_dir}")

# --- phazite_core: JUCE and Phazite's model and services code, compiled once ---
# A static library that the app and the tests both link. JUCE's modules are INTERFACE libraries that
# add their .cpp files to whichever target links them, so only this library links them (PRIVATE, which
# for a static library still passes their system libraries on to the executables). The executables get
# the module headers and the same definitions through phazite_use_core() below, as JUCE does for its
# plug-in wrapper targets. All JUCE options therefore go here, so every file agrees.
add_library(phazite_core STATIC)

target_sources(phazite_core PRIVATE
    src/devices/core/AudioThreads.cpp
    src/devices/core/AudioThreads.h
    src/devices/core/DeviceInfo.cpp
    src/devices/core/DeviceInfo.h
    src/devices/core/DeviceScope.h
    src/devices/core/FallingValue.h
    src/devices/core/LiveValue.h
    src/devices/core/MeterValue.h
    src/devices/core/Processor.h
    src/devices/core/SampleTap.h
    src/devices/core/Spectrum.cpp
    src/devices/core/Spectrum.h
    src/devices/delay/Delay.cpp
    src/devices/delay/Delay.h
    src/devices/drums/DrumSampler.cpp
    src/devices/drums/DrumSampler.h
    src/devices/drums/DrumSynthesis.cpp
    src/devices/drums/DrumSynthesis.h
    src/devices/drums/FactoryDrums.cpp
    src/devices/drums/FactoryDrums.h
    src/devices/dsp/CombFilter.cpp
    src/devices/dsp/CombFilter.h
    src/devices/dsp/Crossover.h
    src/devices/dsp/DelayLine.h
    src/devices/dsp/Envelopes.h
    src/devices/dsp/Filters.cpp
    src/devices/dsp/Filters.h
    src/devices/dsp/Math.h
    src/devices/dsp/MultimodeFilter.cpp
    src/devices/dsp/MultimodeFilter.h
    src/devices/dsp/NoteValues.h
    src/devices/dsp/Oscillators.cpp
    src/devices/dsp/Oscillators.h
    src/devices/dsp/Oversampling.cpp
    src/devices/dsp/Oversampling.h
    src/devices/dsp/Random.h
    src/devices/dsp/Saturation.cpp
    src/devices/dsp/Saturation.h
    src/devices/dsp/SincReader.cpp
    src/devices/dsp/SincReader.h
    src/devices/dsp/SyncedLfo.h
    src/devices/dynamics/Compressor.cpp
    src/devices/dynamics/Compressor.h
    src/devices/dynamics/DynamicsCurve.cpp
    src/devices/dynamics/DynamicsCurve.h
    src/devices/dynamics/Limiter.cpp
    src/devices/dynamics/Limiter.h
    src/devices/dynamics/Multiband.cpp
    src/devices/dynamics/Multiband.h
    src/devices/dynamics/TruePeak.cpp
    src/devices/dynamics/TruePeak.h
    src/devices/eq/Equalizer.cpp
    src/devices/eq/Equalizer.h
    src/devices/filter/FilterEffect.cpp
    src/devices/filter/FilterEffect.h
    src/devices/modulation/Modulation.cpp
    src/devices/modulation/Modulation.h
    src/devices/modulation/Modulators.cpp
    src/devices/modulation/Modulators.h
    src/devices/motion/Flanger.cpp
    src/devices/motion/Flanger.h
    src/devices/motion/Phaser.cpp
    src/devices/motion/Phaser.h
    src/devices/reverb/Reverb.cpp
    src/devices/reverb/Reverb.h
    src/devices/saturation/Saturator.cpp
    src/devices/saturation/Saturator.h
    src/devices/stereo/StereoExpander.cpp
    src/devices/stereo/StereoExpander.h
    src/devices/stretch/RubberBandLibrary.cpp
    src/devices/stretch/Stretch.cpp
    src/devices/stretch/Stretch.h
    src/devices/synth/Synth.cpp
    src/devices/synth/Synth.h
    src/devices/vocoder/Vocoder.cpp
    src/devices/vocoder/Vocoder.h
    src/devices/wavetable/FactoryWavetables.cpp
    src/devices/wavetable/FactoryWavetables.h
    src/devices/wavetable/Wavetable.cpp
    src/devices/wavetable/Wavetable.h
    src/devices/wavetable/WavetableSynth.cpp
    src/devices/wavetable/WavetableSynth.h
    src/engine/audio/AudioFile.cpp
    src/engine/audio/AudioFile.h
    src/engine/audio/AudioRegion.cpp
    src/engine/audio/AudioRegion.h
    src/engine/audio/FileReader.cpp
    src/engine/audio/FileReader.h
    src/engine/audio/Resampler.cpp
    src/engine/audio/Resampler.h
    src/engine/automation/Curve.cpp
    src/engine/automation/Curve.h
    src/engine/core/Engine.cpp
    src/engine/core/Engine.h
    src/engine/core/EventQueue.h
    src/engine/core/Handover.h
    src/engine/core/Plan.h
    src/engine/core/Recording.cpp
    src/engine/core/Recording.h
    src/engine/core/TempoMap.cpp
    src/engine/core/TempoMap.h
    src/engine/core/Workers.cpp
    src/engine/core/Workers.h
    src/engine/modulation/ModulatorNode.cpp
    src/engine/modulation/ModulatorNode.h
    src/engine/nodes/Channel.cpp
    src/engine/nodes/Channel.h
    src/engine/nodes/Comparison.cpp
    src/engine/nodes/Comparison.h
    src/engine/nodes/DeviceNode.cpp
    src/engine/nodes/DeviceNode.h
    src/engine/nodes/DeviceRunner.cpp
    src/engine/nodes/DeviceRunner.h
    src/engine/nodes/LevelMeter.cpp
    src/engine/nodes/LevelMeter.h
    src/engine/nodes/LoudnessMeter.cpp
    src/engine/nodes/LoudnessMeter.h
    src/engine/nodes/NoteSequence.cpp
    src/engine/nodes/NoteSequence.h
    src/engine/nodes/Strip.cpp
    src/engine/nodes/Strip.h
    src/engine/transport/Click.cpp
    src/engine/transport/Click.h
    src/model/arrangement/Arrangement.cpp
    src/model/arrangement/AudioClipModel.cpp
    src/model/arrangement/AudioClipModel.h
    src/model/arrangement/AudioClips.cpp
    src/model/arrangement/ClipRenders.cpp
    src/model/arrangement/Clipboard.cpp
    src/model/arrangement/Clips.cpp
    src/model/arrangement/Grid.cpp
    src/model/arrangement/GridSettings.h
    src/model/arrangement/Import.cpp
    src/model/arrangement/Outlines.cpp
    src/model/arrangement/Outlines.h
    src/model/arrangement/PlaylistModel.cpp
    src/model/arrangement/PlaylistModel.h
    src/model/arrangement/PlaylistValues.h
    src/model/arrangement/Sections.cpp
    src/model/arrangement/Selection.cpp
    src/model/arrangement/SongRange.cpp
    src/model/arrangement/SongRange.h
    src/model/arrangement/SongSelection.cpp
    src/model/arrangement/SongSelection.h
    src/model/arrangement/TimelineModel.cpp
    src/model/arrangement/TimelineModel.h
    src/model/arrangement/TrackColours.cpp
    src/model/arrangement/TrackColours.h
    src/model/arrangement/Tracks.cpp
    src/model/arrangement/internal/Arrangement.h
    src/model/arrangement/internal/AudioClips.h
    src/model/arrangement/internal/ClipRenders.h
    src/model/arrangement/internal/Clipboard.h
    src/model/arrangement/internal/Clips.h
    src/model/arrangement/internal/Grid.h
    src/model/arrangement/internal/Import.h
    src/model/arrangement/internal/Outlines.h
    src/model/arrangement/internal/Sections.h
    src/model/arrangement/internal/Selection.h
    src/model/arrangement/internal/SongRange.h
    src/model/arrangement/internal/SongSelection.h
    src/model/arrangement/internal/Tracks.h
    src/model/automation/Automation.cpp
    src/model/automation/AutomationModel.cpp
    src/model/automation/AutomationModel.h
    src/model/automation/AutomationValues.cpp
    src/model/automation/AutomationValues.h
    src/model/automation/internal/Automation.h
    src/model/base/Beats.h
    src/model/base/IDs.h
    src/model/base/Selection.cpp
    src/model/base/Schema.cpp
    src/model/base/Song.cpp
    src/model/base/Song.h
    src/model/base/Toggles.h
    src/model/base/internal/Schema.h
    src/model/base/internal/Selection.h
    src/model/chords/ChordModel.cpp
    src/model/chords/ChordModel.h
    src/model/chords/ChordValues.cpp
    src/model/chords/ChordValues.h
    src/model/chords/Chords.cpp
    src/model/chords/Progressions.cpp
    src/model/chords/Progressions.h
    src/model/chords/internal/Chords.h
    src/model/commands/CommandRegistry.cpp
    src/model/commands/CommandRegistry.h
    src/model/commands/SongGesture.cpp
    src/model/commands/SongGesture.h
    src/model/commands/Undo.cpp
    src/model/commands/UndoCommands.cpp
    src/model/commands/internal/Undo.h
    src/model/commands/internal/UndoCommands.h
    src/model/devices/DeviceHandle.cpp
    src/model/devices/DeviceHandle.h
    src/model/devices/DeviceFiles.cpp
    src/model/devices/DrumKit.cpp
    src/model/devices/Devices.cpp
    src/model/devices/ParameterHandle.cpp
    src/model/devices/ParameterHandle.h
    src/model/devices/ParameterRef.h
    src/model/devices/Parameters.cpp
    src/model/devices/Wavetables.cpp
    src/model/devices/internal/DeviceFiles.h
    src/model/devices/internal/DeviceHandle.h
    src/model/devices/internal/Devices.h
    src/model/devices/internal/DrumKit.h
    src/model/devices/internal/ParameterHandle.h
    src/model/devices/internal/Parameters.h
    src/model/devices/internal/Wavetables.h
    src/model/engine/AppInfo.cpp
    src/model/engine/AppInfo.h
    src/model/engine/AudioDevices.cpp
    src/model/engine/AudioDevices.h
    src/model/engine/AudioFiles.cpp
    src/model/engine/AudioFiles.h
    src/model/engine/AudioIO.cpp
    src/model/engine/EngineContext.cpp
    src/model/engine/EngineContext.h
    src/model/engine/LiveInput.cpp
    src/model/engine/PluginScanner.cpp
    src/model/engine/Preview.cpp
    src/model/engine/Preview.h
    src/model/engine/PreviewLoop.cpp
    src/model/engine/StartupSound.cpp
    src/model/engine/StartupSound.h
    src/model/engine/internal/AudioIO.h
    src/model/engine/internal/EngineContext.h
    src/model/engine/internal/LiveInput.h
    src/model/engine/internal/PluginScanner.h
    src/model/engine/internal/Preview.h
    src/model/engine/internal/PreviewLoop.h
    src/model/mixer/Buses.cpp
    src/model/mixer/ContainerModel.cpp
    src/model/mixer/ContainerModel.h
    src/model/mixer/Containers.cpp
    src/model/mixer/DeviceType.h
    src/model/mixer/Freeze.cpp
    src/model/mixer/FreezeModel.h
    src/model/mixer/LevelSource.cpp
    src/model/mixer/LevelSource.h
    src/model/mixer/MixHandle.cpp
    src/model/mixer/MixHandle.h
    src/model/mixer/Mixer.cpp
    src/model/mixer/MixerModel.cpp
    src/model/mixer/MixerModel.h
    src/model/mixer/Sidechain.cpp
    src/model/mixer/SidechainChoice.h
    src/model/mixer/internal/Buses.h
    src/model/mixer/internal/Containers.h
    src/model/mixer/internal/Freeze.h
    src/model/mixer/internal/LevelSource.h
    src/model/mixer/internal/MixHandle.h
    src/model/mixer/internal/Mixer.h
    src/model/mixer/internal/Sidechain.h
    src/model/modulation/Modulation.cpp
    src/model/modulation/ModulationModel.cpp
    src/model/modulation/ModulationModel.h
    src/model/modulation/ModulationValues.cpp
    src/model/modulation/ModulationValues.h
    src/model/modulation/internal/Modulation.h
    src/model/notes/Notes.cpp
    src/model/notes/NoteValues.h
    src/model/notes/NotesModel.cpp
    src/model/notes/NotesModel.h
    src/model/notes/internal/Notes.h
    src/model/playback/AutomationWriter.cpp
    src/model/playback/OfflineRender.cpp
    src/model/playback/PlaybackHost.cpp
    src/model/playback/PlaybackSession.cpp
    src/model/playback/SongPlayer.cpp
    src/model/playback/internal/AutomationWriter.h
    src/model/playback/internal/OfflineRender.h
    src/model/playback/internal/PlaybackHost.h
    src/model/playback/internal/PlaybackSession.h
    src/model/playback/internal/SongPlayer.h
    src/model/plugins/PluginHost.cpp
    src/model/plugins/PluginInstances.cpp
    src/model/plugins/SandboxedInstance.cpp
    src/model/plugins/SandboxedProcessor.cpp
    src/model/plugins/internal/PluginHost.h
    src/model/plugins/internal/PluginInstances.h
    src/model/plugins/internal/SandboxedInstance.h
    src/model/plugins/internal/SandboxedProcessor.h
    src/model/presets/PresetModel.h
    src/model/presets/Presets.cpp
    src/model/presets/internal/Presets.h
    src/model/project/DawProject.cpp
    src/model/project/ExampleSongs.cpp
    src/model/project/ExampleSongs.h
    src/model/project/Export.cpp
    src/model/project/Export.h
    src/model/project/NewProject.cpp
    src/model/project/NewProject.h
    src/model/project/Project.cpp
    src/model/project/ProjectContents.cpp
    src/model/project/ProjectContents.h
    src/model/project/ProjectContext.cpp
    src/model/project/ProjectContext.h
    src/model/project/UnsavedProjects.cpp
    src/model/project/UnusedAudio.cpp
    src/model/project/UnusedAudio.h
    src/model/project/internal/DawProject.h
    src/model/project/internal/ExampleSongs.h
    src/model/project/internal/Export.h
    src/model/project/internal/NewProject.h
    src/model/project/internal/Project.h
    src/model/project/internal/ProjectContext.h
    src/model/project/internal/UnsavedProjects.h
    src/model/transport/AudioRecorder.cpp
    src/model/transport/Capture.cpp
    src/model/transport/Metronome.cpp
    src/model/transport/Playback.cpp
    src/model/transport/Recording.cpp
    src/model/transport/RecordingModel.cpp
    src/model/transport/RecordingModel.h
    src/model/transport/Take.cpp
    src/model/transport/Tempo.cpp
    src/model/transport/TempoValues.cpp
    src/model/transport/TempoValues.h
    src/model/transport/TransportModel.cpp
    src/model/transport/TransportModel.h
    src/model/transport/internal/AudioRecorder.h
    src/model/transport/internal/Capture.h
    src/model/transport/internal/Metronome.h
    src/model/transport/internal/Playback.h
    src/model/transport/internal/Recording.h
    src/model/transport/internal/Take.h
    src/model/transport/internal/Tempo.h
    src/model/versions/ApplySong.cpp
    src/model/versions/PieceStore.cpp
    src/model/versions/SongDiff.cpp
    src/model/versions/SongMerge.cpp
    src/model/versions/SongPieces.cpp
    src/model/versions/VersionHistory.cpp
    src/model/versions/VersionStore.cpp
    src/model/versions/VersionsModel.cpp
    src/model/versions/VersionsModel.h
    src/model/versions/internal/ApplySong.h
    src/model/versions/internal/PieceStore.h
    src/model/versions/internal/SongDiff.h
    src/model/versions/internal/SongMerge.h
    src/model/versions/internal/SongPieces.h
    src/model/versions/internal/VersionHistory.h
    src/model/versions/internal/VersionStore.h
    # sandbox: what Phazite and the helpers that host plug-ins share, engine-neutral
    src/sandbox/client/Helper.cpp
    src/sandbox/client/Helper.h
    src/sandbox/client/RemoteInstance.cpp
    src/sandbox/client/RemoteInstance.h
    src/sandbox/core/BlockChannel.cpp
    src/sandbox/core/BlockChannel.h
    src/sandbox/core/Messages.cpp
    src/sandbox/core/Messages.h
    src/sandbox/core/PluginInfo.cpp
    src/sandbox/core/PluginInfo.h
    src/sandbox/core/SharedMemory.cpp
    src/sandbox/core/SharedMemory.h
    # services: what the whole app shares (they use the model; the model never uses them)
    src/services/BrowserModel.cpp
    src/services/BrowserModel.h
    src/services/Library.cpp
    src/services/Library.h
    src/services/RecentProjects.cpp
    src/services/RecentProjects.h
    src/services/Settings.cpp
    src/services/Settings.h
    src/services/SoundPack.cpp
    src/services/SoundPack.h
    src/services/Updates.cpp
    src/services/Updates.h)

# Small data files compiled into the program (PhaziteData.h, namespace PhaziteData). Fonts don't go
# here: CLAUDE.md requires them as separate files next to the app.
juce_add_binary_data(phazite_data
    HEADER_NAME PhaziteData.h
    NAMESPACE PhaziteData
    SOURCES
        resources/keymaps/phazite.json
        resources/keymaps/fl_studio.json
        resources/keymaps/ableton_live.json
        # Song outlines: every file named *.outline.json here is one (src/model/arrangement/Outlines.h)
        resources/outlines/pop.outline.json
        resources/outlines/rock.outline.json
        resources/outlines/electronic.outline.json
        resources/outlines/hip_hop.outline.json
        resources/outlines/lofi.outline.json
        resources/outlines/rnb.outline.json
        resources/outlines/drum_and_bass.outline.json
        resources/outlines/techno.outline.json
        # Chord progressions (src/model/chords/Progressions.h)
        resources/progressions/progressions.json
        # The notices of the code and fonts from others that Phazite is built with, for the About box (src/model/engine/AppInfo.h)
        resources/licenses/notices.json
        # Phazite's logo, its own drawing (ui::createLogo()), from which the app's icon is made too (juce_add_gui_app below)
        resources/logo/phazite.svg
        # Lucide 1.55.0 icons (ISC; those that come from Feather: MIT): resources/icons/lucide/LICENSE
        resources/icons/lucide/app-window.svg
        resources/icons/lucide/audio-lines.svg
        resources/icons/lucide/check.svg
        resources/icons/lucide/chevron-down.svg
        resources/icons/lucide/chevron-right.svg
        resources/icons/lucide/chevrons-down-up.svg
        resources/icons/lucide/chevrons-up-down.svg
        resources/icons/lucide/circle.svg
        resources/icons/lucide/clipboard-list.svg
        resources/icons/lucide/circle-user-round.svg
        resources/icons/lucide/clock-arrow-down.svg
        resources/icons/lucide/clock.svg
        resources/icons/lucide/corner-down-left.svg
        resources/icons/lucide/copy.svg
        resources/icons/lucide/download.svg
        resources/icons/lucide/ellipsis.svg
        resources/icons/lucide/eraser.svg
        resources/icons/lucide/file-music.svg
        resources/icons/lucide/folder-open.svg
        resources/icons/lucide/folder-plus.svg
        resources/icons/lucide/folder.svg
        resources/icons/lucide/git-branch.svg
        resources/icons/lucide/headphones.svg
        resources/icons/lucide/keyboard-music.svg
        resources/icons/lucide/layout-dashboard.svg
        resources/icons/lucide/library.svg
        resources/icons/lucide/magnet.svg
        resources/icons/lucide/maximize-2.svg
        resources/icons/lucide/menu.svg
        resources/icons/lucide/metronome.svg
        resources/icons/lucide/minimize-2.svg
        resources/icons/lucide/minus.svg
        resources/icons/lucide/mouse-pointer-2.svg
        resources/icons/lucide/move-horizontal.svg
        resources/icons/lucide/music.svg
        resources/icons/lucide/paintbrush.svg
        resources/icons/lucide/pencil.svg
        resources/icons/lucide/piano.svg
        resources/icons/lucide/play.svg
        resources/icons/lucide/plus.svg
        resources/icons/lucide/redo-2.svg
        resources/icons/lucide/repeat.svg
        resources/icons/lucide/rotate-cw.svg
        resources/icons/lucide/scissors.svg
        resources/icons/lucide/search.svg
        resources/icons/lucide/settings.svg
        resources/icons/lucide/sliders-horizontal.svg
        resources/icons/lucide/snowflake.svg
        resources/icons/lucide/square-dashed-mouse-pointer.svg
        resources/icons/lucide/square.svg
        resources/icons/lucide/star.svg
        resources/icons/lucide/undo-2.svg
        resources/icons/lucide/volume-x.svg
        resources/icons/lucide/x.svg
        resources/icons/lucide/zap.svg
        resources/icons/lucide/zoom-in.svg)

target_link_libraries(phazite_core PRIVATE phazite_data)

target_include_directories(phazite_core PUBLIC src)
target_compile_features(phazite_core PUBLIC cxx_std_20)

# Rubber Band Library 4.0 (GPL-2.0-or-later), which stretches audio clips (src/devices/stretch/), compiled from its
# single-file build in RubberBandLibrary.cpp. Its code is third-party: MSVC's warnings are off for it there, GCC's and
# Clang's here.
target_include_directories(phazite_core SYSTEM PRIVATE external/rubberband)

if(NOT MSVC)
    set_source_files_properties(src/devices/stretch/RubberBandLibrary.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()

# r8brain-free-src 7.6 (MIT), header-only, which resamples audio files at another rate than the engine's
# (src/engine/audio/Resampler.cpp). Its code is third-party: its folder is a system one, so that GCC and Clang keep its
# warnings to themselves, and Resampler.cpp turns MSVC's off around it.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/r8brain-free-src/CDSPResampler.h")
    message(FATAL_ERROR
        "external/r8brain-free-src is empty because the git submodules are not checked out. Run:\n"
        "  git submodule update --init --recursive --depth 1")
endif()

target_include_directories(phazite_core SYSTEM PRIVATE external/r8brain-free-src)

# GCC 13 and later read `const auto& button = opt.getButtons().front();` in JUCE 9.0.5's juce_LookAndFeel_V2.cpp as a
# reference to a temporary (-Wdangling-reference, in -Wall): getButtons() returns a Span by value, but the button is in the
# options' own list, which outlives the line. Nothing dangles and JUCE's code is read-only here, so the warning is off for
# the one unit of JUCE's that includes that file, as JUCE's own hb.hh turns it off for HarfBuzz.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
    set_source_files_properties(external/JUCE/modules/juce_gui_basics/juce_gui_basics_2.cpp
        PROPERTIES COMPILE_OPTIONS "-Wno-dangling-reference")
endif()

target_compile_definitions(phazite_core PRIVATE
    PHAZITE_VERSION_STRING="${PROJECT_VERSION}"
    PHAZITE_COPYRIGHT_STRING="${PHAZITE_COPYRIGHT}"
    PHAZITE_SOURCE_URL="${PHAZITE_SOURCE_URL}"
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

if(WIN32)
    # JUCE's headers define these anyway; defining them here as well keeps any code that includes
    # <windows.h> before JUCE consistent.
    target_compile_definitions(phazite_core PRIVATE UNICODE _UNICODE)
    # ASIO drivers, the low-latency standard for audio interfaces on Windows. JUCE bundles Steinberg's
    # ASIO SDK 2.3.4 headers, which Phazite uses under the SDK's GPLv3 option (AGPL-compatible).
    target_compile_definitions(phazite_core PRIVATE JUCE_ASIO=1)
    # Windows' device tree, to tell a Bluetooth output (src/model/engine/AudioDevices.cpp).
    target_link_libraries(phazite_core PRIVATE cfgmgr32)
endif()

target_link_libraries(phazite_core
    PRIVATE
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_extra
        juce::juce_cryptography # SHA-256, to check downloads
    PUBLIC
        juce::juce_recommended_warning_flags) # for Phazite's own files in every target, too

# JUCE's recommended config flags add /MP, which MSVC refuses alongside the /showIncludes flag that
# Ninja relies on (warning D9030 for every file). CMake's own MSVC defaults already provide the same
# /Od, /O2 and /EHsc, so these flags are only used with GCC and Clang.
if(NOT MSVC)
    target_link_libraries(phazite_core PUBLIC juce::juce_recommended_config_flags)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(phazite_core PUBLIC atomic)
endif()

# Links a target (an executable, phazite_ui or phazite_app) to phazite_core and gives it the module headers
# and phazite_core's definitions (including JUCE's JUCE_MODULE_AVAILABLE_* ones), without compiling the
# modules a second time.
function(phazite_use_core target)
    target_include_directories(${target} PRIVATE $<TARGET_PROPERTY:phazite_core,INCLUDE_DIRECTORIES>)
    target_compile_definitions(${target} PRIVATE $<TARGET_PROPERTY:phazite_core,COMPILE_DEFINITIONS>)
    target_link_libraries(${target} PRIVATE phazite_core)
endfunction()

# --- phazite_ui: the window's components and the look ---
# A static library like phazite_core, so that the tests can build the real playlist, piano roll and mixer
# and drive them with the mouse.
add_library(phazite_ui STATIC)

target_sources(phazite_ui PRIVATE
    src/ui/audioclip/AudioClipEditor.cpp
    src/ui/audioclip/AudioClipEditor.h
    src/ui/browser/Browser.cpp
    src/ui/browser/Browser.h
    src/ui/browser/PluginPicker.cpp
    src/ui/browser/PluginPicker.h
    src/ui/browser/SoundView.cpp
    src/ui/browser/SoundView.h
    src/ui/components/ArmButton.cpp
    src/ui/components/ArmButton.h
    src/ui/components/Controls.cpp
    src/ui/components/Controls.h
    src/ui/components/DisplayTicker.cpp
    src/ui/components/DisplayTicker.h
    src/ui/components/ListRowHover.h
    src/ui/components/ModulatorDrag.h
    src/ui/components/PeakBar.cpp
    src/ui/components/PeakBar.h
    src/ui/components/PowerSwitch.cpp
    src/ui/components/PowerSwitch.h
    src/ui/components/SectionHeader.cpp
    src/ui/components/SectionHeader.h
    src/ui/components/TrackSwitch.cpp
    src/ui/components/TrackSwitch.h
    src/ui/components/ValueText.cpp
    src/ui/components/ValueText.h
    src/ui/components/ViewTitle.cpp
    src/ui/components/ViewTitle.h
    src/ui/devices/CompressorEditor.cpp
    src/ui/devices/CompressorEditor.h
    src/ui/devices/DelayEditor.cpp
    src/ui/devices/DelayEditor.h
    src/ui/devices/DeviceEditorBase.cpp
    src/ui/devices/DeviceEditorBase.h
    src/ui/devices/DeviceEditors.cpp
    src/ui/devices/DeviceEditors.h
    src/ui/devices/DeviceGraph.cpp
    src/ui/devices/DeviceGraph.h
    src/ui/devices/DeviceLayout.cpp
    src/ui/devices/DeviceLayout.h
    src/ui/devices/DeviceMenus.cpp
    src/ui/devices/DeviceMenus.h
    src/ui/devices/DeviceTint.cpp
    src/ui/devices/DeviceTint.h
    src/ui/devices/DeviceWindowContent.cpp
    src/ui/devices/DeviceWindowContent.h
    src/ui/devices/DrumSamplerEditor.cpp
    src/ui/devices/DrumSamplerEditor.h
    src/ui/devices/DynamicsCurve.cpp
    src/ui/devices/DynamicsCurve.h
    src/ui/devices/EqEditor.cpp
    src/ui/devices/EqEditor.h
    src/ui/devices/FilterEditor.cpp
    src/ui/devices/FilterEditor.h
    src/ui/devices/FlangerEditor.cpp
    src/ui/devices/FlangerEditor.h
    src/ui/devices/LevelHistoryView.cpp
    src/ui/devices/LevelHistoryView.h
    src/ui/devices/LimiterEditor.cpp
    src/ui/devices/LimiterEditor.h
    src/ui/devices/ModulatorEditor.cpp
    src/ui/devices/ModulatorEditor.h
    src/ui/devices/MultibandEditor.cpp
    src/ui/devices/MultibandEditor.h
    src/ui/devices/ParameterControls.cpp
    src/ui/devices/ParameterControls.h
    src/ui/devices/PhaserEditor.cpp
    src/ui/devices/PhaserEditor.h
    src/ui/devices/ReverbEditor.cpp
    src/ui/devices/ReverbEditor.h
    src/ui/devices/SaturatorEditor.cpp
    src/ui/devices/SaturatorEditor.h
    src/ui/devices/SidechainMenus.cpp
    src/ui/devices/SidechainMenus.h
    src/ui/devices/StereoExpanderEditor.cpp
    src/ui/devices/StereoExpanderEditor.h
    src/ui/devices/SweepPicture.cpp
    src/ui/devices/SweepPicture.h
    src/ui/devices/SynthEditor.cpp
    src/ui/devices/SynthEditor.h
    src/ui/devices/SynthParts.cpp
    src/ui/devices/SynthParts.h
    src/ui/devices/VocoderEditor.cpp
    src/ui/devices/VocoderEditor.h
    src/ui/devices/WavetableEditor.cpp
    src/ui/devices/WavetableEditor.h
    src/ui/input/CommandBridge.cpp
    src/ui/input/CommandBridge.h
    src/ui/input/CommandUI.cpp
    src/ui/input/CommandUI.h
    src/ui/input/ComputerKeyboard.cpp
    src/ui/input/ComputerKeyboard.h
    src/ui/input/KeyDispatcher.cpp
    src/ui/input/KeyDispatcher.h
    src/ui/input/Keymap.cpp
    src/ui/input/Keymap.h
    src/ui/input/KeymapStore.cpp
    src/ui/input/KeymapStore.h
    src/ui/input/LibraryDrag.cpp
    src/ui/input/LibraryDrag.h
    src/ui/input/PluginKeys.cpp
    src/ui/input/PluginKeys.h
    src/ui/input/WindowKeys.cpp
    src/ui/input/WindowKeys.h
    src/ui/mixer/ChannelStrip.cpp
    src/ui/mixer/ChannelStrip.h
    src/ui/mixer/DeviceSlots.cpp
    src/ui/mixer/DeviceSlots.h
    src/ui/mixer/DevicesView.cpp
    src/ui/mixer/DevicesView.h
    src/ui/mixer/Inspector.cpp
    src/ui/mixer/Inspector.h
    src/ui/mixer/MasterStrip.cpp
    src/ui/mixer/MasterStrip.h
    src/ui/mixer/MixerView.cpp
    src/ui/mixer/MixerView.h
    src/ui/mixer/ModulatorSlots.cpp
    src/ui/mixer/ModulatorSlots.h
    src/ui/mixer/StripParts.cpp
    src/ui/mixer/StripParts.h
    src/ui/pianoroll/DrumRows.cpp
    src/ui/pianoroll/DrumRows.h
    src/ui/pianoroll/PianoRoll.cpp
    src/ui/pianoroll/PianoRoll.h
    src/ui/pianoroll/PianoRollCommands.cpp
    src/ui/pianoroll/PianoRollLanes.cpp
    src/ui/pianoroll/PianoRollNoteTools.cpp
    src/ui/pianoroll/PianoRollNotes.cpp
    src/ui/pianoroll/PianoRollParts.h
    src/ui/pianoroll/PianoRollRows.cpp
    src/ui/playlist/ChordLane.cpp
    src/ui/playlist/ChordLane.h
    src/ui/playlist/ClipComponent.cpp
    src/ui/playlist/ClipComponent.h
    src/ui/playlist/ClipWaveform.cpp
    src/ui/playlist/ClipWaveform.h
    src/ui/playlist/Playlist.cpp
    src/ui/playlist/Playlist.h
    src/ui/playlist/PlaylistAutomation.cpp
    src/ui/playlist/PlaylistDrops.cpp
    src/ui/playlist/PlaylistParts.h
    src/ui/playlist/PlaylistRows.cpp
    src/ui/playlist/PlaylistTools.cpp
    src/ui/playlist/PlaylistTracks.cpp
    src/ui/playlist/SectionsStrip.cpp
    src/ui/playlist/SectionsStrip.h
    src/ui/playlist/TrackHeader.cpp
    src/ui/playlist/TrackHeader.h
    src/ui/playlist/TrackHeights.cpp
    src/ui/playlist/TrackHeights.h
    src/ui/project/ProjectView.cpp
    src/ui/project/ProjectView.h
    src/ui/shell/AboutPanel.cpp
    src/ui/shell/AboutPanel.h
    src/ui/shell/ActivityBar.cpp
    src/ui/shell/ActivityBar.h
    src/ui/shell/AppShell.cpp
    src/ui/shell/AppShell.h
    src/ui/shell/CleanUpPanel.cpp
    src/ui/shell/CleanUpPanel.h
    src/ui/shell/CommandPalette.cpp
    src/ui/shell/CommandPalette.h
    src/ui/shell/ExportPanel.cpp
    src/ui/shell/ExportPanel.h
    src/ui/shell/Guide.cpp
    src/ui/shell/Guide.h
    src/ui/shell/HintBar.cpp
    src/ui/shell/HintBar.h
    src/ui/shell/LessonCard.cpp
    src/ui/shell/LessonCard.h
    src/ui/shell/Lessons.cpp
    src/ui/shell/Lessons.h
    src/ui/shell/NewProjectPanel.cpp
    src/ui/shell/NewProjectPanel.h
    src/ui/shell/PanelLayout.cpp
    src/ui/shell/PanelLayout.h
    src/ui/shell/Panels.cpp
    src/ui/shell/Panels.h
    src/ui/shell/ProgressionsPanel.cpp
    src/ui/shell/ProgressionsPanel.h
    src/ui/shell/RenderPanel.cpp
    src/ui/shell/RenderPanel.h
    src/ui/shell/SettingsPanel.cpp
    src/ui/shell/SettingsPanel.h
    src/ui/shell/StatusBar.cpp
    src/ui/shell/StatusBar.h
    src/ui/shell/TempoStretchPanel.cpp
    src/ui/shell/TempoStretchPanel.h
    src/ui/shell/TitleBar.cpp
    src/ui/shell/TitleBar.h
    src/ui/shell/TopBar.cpp
    src/ui/shell/TopBar.h
    src/ui/shell/UpdatePanel.cpp
    src/ui/shell/UpdatePanel.h
    src/ui/shell/Welcome.cpp
    src/ui/shell/Welcome.h
    src/ui/theme/DesignTokens.h
    src/ui/theme/Icons.cpp
    src/ui/theme/Icons.h
    src/ui/theme/LookAndFeel.cpp
    src/ui/theme/LookAndFeel.h
    src/ui/theme/Popups.cpp
    src/ui/theme/Popups.h
    src/ui/timeline/SmartTool.cpp
    src/ui/timeline/SmartTool.h
    src/ui/timeline/TimelineEditor.cpp
    src/ui/timeline/TimelineEditor.h
    src/ui/timeline/TimelineGesture.cpp
    src/ui/timeline/TimelineGesture.h
    src/ui/timeline/TimelineOverview.cpp
    src/ui/timeline/TimelineView.cpp
    src/ui/timeline/TimelineView.h
    src/ui/versions/BranchPicker.cpp
    src/ui/versions/BranchPicker.h
    src/ui/versions/VersionsView.cpp
    src/ui/versions/VersionsView.h)

phazite_use_core(phazite_ui)

# --- phazite_app: the app's parts (the main window, the open project, the song's commands), in a library the tests
# link, so the app's logic can be tested without a window. The Phazite executable is Main.cpp. ---
add_library(phazite_app STATIC)

target_sources(phazite_app PRIVATE
    src/app/AppCommands.cpp
    src/app/AppCommands.h
    src/app/AppServices.cpp
    src/app/AppServices.h
    src/app/ClipWaveforms.cpp
    src/app/ClipWaveforms.h
    src/app/MainWindow.cpp
    src/app/MainWindow.h
    src/app/PluginWindow.cpp
    src/app/PluginWindow.h
    src/app/ProjectSession.cpp
    src/app/ProjectSession.h
    src/app/RunMarker.cpp
    src/app/RunMarker.h
    src/app/SandboxedEditor.cpp
    src/app/SandboxedEditor.h)

phazite_use_core(phazite_app)
target_link_libraries(phazite_app PRIVATE phazite_ui)

# --- The app ---
# Its icon (Windows' .ico, macOS's .icns) is made from Phazite's logo by JUCE's juceaide as CMake configures, so a change to
# the logo configures again.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS resources/logo/phazite.svg)

juce_add_gui_app(Phazite
    PRODUCT_NAME "Phazite"
    COMPANY_NAME "Phazite"
    COMPANY_COPYRIGHT "${PHAZITE_COPYRIGHT}"
    BUNDLE_ID "io.github.august-byrne.phazite"
    ICON_BIG resources/logo/phazite.svg
    MICROPHONE_PERMISSION_ENABLED TRUE
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE
    PLUGINHOST_AU TRUE)

target_sources(Phazite PRIVATE src/app/Main.cpp)

target_compile_definitions(Phazite PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Phazite,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Phazite,JUCE_VERSION>")

phazite_use_core(Phazite)
target_link_libraries(Phazite PRIVATE phazite_app phazite_ui)

# --- PhaziteHost: the helper program that hosts plug-ins for Phazite, each in a process of its own or with others, so
# that one that crashes or hangs can't take the song with it. It links phazite_core, as phazite_soundkit does, for JUCE
# and the sandbox's code; its code is in phazite_host, which the tests' helper links too. It goes next to the app, which
# starts it. ---
add_library(phazite_host STATIC)
target_sources(phazite_host PRIVATE
    src/host/core/Editor.cpp
    src/host/core/Editor.h
    src/host/core/Host.cpp
    src/host/core/Host.h
    src/host/core/Instance.cpp
    src/host/core/Instance.h
    src/host/core/Platform.cpp)
phazite_use_core(phazite_host)

juce_add_gui_app(PhaziteHost
    PRODUCT_NAME "PhaziteHost"
    COMPANY_NAME "Phazite"
    COMPANY_COPYRIGHT "${PHAZITE_COPYRIGHT}"
    BUNDLE_ID "io.github.august-byrne.phazite.host"
    ICON_BIG resources/logo/phazite.svg # the app's, so that Task Manager shows the helper as Phazite's
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE
    PLUGINHOST_AU TRUE)

target_sources(PhaziteHost PRIVATE src/host/app/HostMain.cpp src/host/app/Formats.cpp)
target_compile_definitions(PhaziteHost PRIVATE JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:PhaziteHost,JUCE_VERSION>")
phazite_use_core(PhaziteHost)
target_link_libraries(PhaziteHost PRIVATE phazite_host)

add_dependencies(Phazite PhaziteHost)
add_custom_command(TARGET Phazite POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:PhaziteHost>" "$<TARGET_FILE_DIR:Phazite>"
    VERBATIM)

# The UI font, Inter (SIL Open Font License 1.1), goes next to the app as separate files with its licence
# text (fonts/Inter), as CLAUDE.md asks of fonts. ui::LookAndFeel loads it from there.
add_custom_command(TARGET Phazite POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts" "$<TARGET_FILE_DIR:Phazite>/fonts"
    VERBATIM)

# Phazite's own presets for its devices, data files that the browser and the devices' Presets menus list (presets/<device>/<kind>),
# go next to the app, as the fonts do. services/Library.h finds them there.
add_custom_command(TARGET Phazite POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/presets" "$<TARGET_FILE_DIR:Phazite>/presets"
    VERBATIM)

# Phazite's licence, and the notices of the code and fonts from others that it is built with, which their licences ask to go
# with every copy (resources/licenses/notices.json: JUCE and what it bundles, Rubber Band's GPL, r8brain-free-src, Inter's
# OFL, Lucide, VS Code's themes and the rest): written as CMake configures (cmake/PhaziteNotices.cmake) and put next to the
# app, licenses/ and NOTICES.txt, which the About box reads and the installer installs.
phazite_write_notices("${CMAKE_BINARY_DIR}/notices")

add_custom_command(TARGET Phazite POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${CMAKE_BINARY_DIR}/notices" "$<TARGET_FILE_DIR:Phazite>"
    VERBATIM)

# --- The installer, on Windows: Inno Setup 6.6 or newer (tools\setup-windows.cmd installs it) compiles
# packaging/windows/Phazite.iss into build/<preset>/installer/Phazite-<version>-Setup.exe, from the app's folder and the
# notices, and writes beside it the installer's SHA-256 and the update file that a release carries for the update check
# (cmake/WriteUpdateFile.cmake; src/services/Updates.h). Build it from the release preset:
#   cmake --build --preset windows-release --target installer
if(WIN32)
    set(_phazite_programs_x86 "ProgramFiles(x86)")
    find_program(PHAZITE_ISCC ISCC
        HINTS "$ENV{${_phazite_programs_x86}}/Inno Setup 6" "$ENV{ProgramFiles}/Inno Setup 6" "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6"
        DOC "Inno Setup's compiler, which builds Phazite's installer (the installer target)")

    set(_phazite_installer_name "Phazite-${PROJECT_VERSION}-Setup")
    set(_phazite_installer_folder "${CMAKE_BINARY_DIR}/installer")
    get_target_property(_phazite_generated Phazite JUCE_GENERATED_SOURCES_DIRECTORY) # where juceaide put the app's icon.ico

    if(PHAZITE_ISCC)
        add_custom_target(installer
            COMMAND "${PHAZITE_ISCC}" /Q
                    "/DAppVersion=${PROJECT_VERSION}"
                    "/DCopyright=${PHAZITE_COPYRIGHT}"
                    "/DSourceUrl=${PHAZITE_SOURCE_URL}"
                    "/DAppFolder=$<PATH:NATIVE_PATH,$<TARGET_FILE_DIR:Phazite>>"
                    "/DNoticesFolder=$<PATH:NATIVE_PATH,${CMAKE_BINARY_DIR}/notices>"
                    "/DIconFile=$<PATH:NATIVE_PATH,${_phazite_generated}/icon.ico>"
                    "/DOutputFolder=$<PATH:NATIVE_PATH,${_phazite_installer_folder}>"
                    "/DOutputName=${_phazite_installer_name}"
                    "$<PATH:NATIVE_PATH,${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/Phazite.iss>"
            COMMAND "${CMAKE_COMMAND}" "-DINSTALLER=${_phazite_installer_folder}/${_phazite_installer_name}.exe"
                    "-DVERSION=${PROJECT_VERSION}" "-DSOURCE_URL=${PHAZITE_SOURCE_URL}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteUpdateFile.cmake"
            COMMENT "Building Phazite's installer with Inno Setup"
            VERBATIM)
    else()
        add_custom_target(installer
            COMMAND "${CMAKE_COMMAND}" -E echo "Phazite's installer needs Inno Setup 6.6 or newer, which CMake didn't find. Run tools\\setup-windows.cmd, then configure again."
            COMMAND "${CMAKE_COMMAND}" -E false
            VERBATIM)
    endif()

    add_dependencies(installer Phazite) # and so PhaziteHost, which goes next to it
endif()

# --- phazite_soundkit: renders Phazite Sounds, the pack Phazite offers to download, from the recipes in the
# phazite-sounds repository (CC0 sample libraries and its own synthesis), and zips it for a release. It is run by hand
# when the sounds change, not as part of the build. ---
juce_add_console_app(phazite_soundkit)
target_sources(phazite_soundkit PRIVATE tools/soundkit/SoundKit.cpp)
phazite_use_core(phazite_soundkit)

# --- Tests: doctest (MIT, external/doctest). Run them with `ctest --preset <preset>`. ---
option(PHAZITE_BUILD_TESTS "Build Phazite's unit tests (phazite_tests)" ON)

if(PHAZITE_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(phazite_tests)

    target_sources(phazite_tests PRIVATE
        tests/TestMain.cpp
        tests/TestHelpers.h
        tests/AboutPanelTests.cpp
        tests/ActivityBarTests.cpp
        tests/AntialiasingTests.cpp
        tests/AppInfoTests.cpp
        tests/AppScreen.h
        tests/AppShellTests.cpp
        tests/AppTests.cpp
        tests/AudioClipEditorTests.cpp
        tests/AudioDevicesTests.cpp
        tests/AudioSettingsTests.cpp
        tests/AudioClipModelTests.cpp
        tests/AudioGlitchTests.cpp
        tests/AudioRecordingTests.cpp
        tests/AudioThreadMonitor.cpp
        tests/AudioThreadMonitor.h
        tests/AutomationFollowsClipsTests.cpp
        tests/AutomationLaneTests.cpp
        tests/AutomationModelTests.cpp
        tests/BeatsTests.cpp
        tests/Benchmarks.cpp
        tests/BrowserModelTests.cpp
        tests/BrowserTests.cpp
        tests/ChordLaneTests.cpp
        tests/CleanUpTests.cpp
        tests/ChordModelTests.cpp
        tests/ChordValueTests.cpp
        tests/ClipboardTests.cpp
        tests/ClipTests.cpp
        tests/ClipWaveformTests.cpp
        tests/CommandGoldenTests.cpp
        tests/CommandTests.cpp
        tests/CompressorEditorTests.cpp
        tests/CompressorTests.cpp
        tests/ComputerKeyboardTests.cpp
        tests/ContainerTests.cpp
        tests/ContainerUiTests.cpp
        tests/ControlsTests.cpp
        tests/DawProjectTests.cpp
        tests/DesignTokenTests.cpp
        tests/DeviceEditorTests.cpp
        tests/DeviceInfoTests.cpp
        tests/DeviceStatesTests.cpp
        tests/DevicesViewTests.cpp
        tests/DrumRowTests.cpp
        tests/DrumSamplerEditorTests.cpp
        tests/DrumSamplerTests.cpp
        tests/DspTests.cpp
        tests/EnginePlayback.h
        tests/EnginePlaybackTests.cpp
        tests/EngineTests.cpp
        tests/EffectDeviceTests.cpp
        tests/EffectEditorTests.cpp
        tests/EqEditorTests.cpp
        tests/EqualizerTests.cpp
        tests/ExampleSongTests.cpp
        tests/ExportTests.cpp
        tests/FactoryPresetTests.cpp
        tests/FreezeTests.cpp
        tests/GoldenFiles.cpp
        tests/GoldenFiles.h
        tests/GoldenMetronomeTests.cpp
        tests/GoldenMixerTests.cpp
        tests/GoldenModulationTests.cpp
        tests/GoldenProjectTests.cpp
        tests/GoldenRenderTests.cpp
        tests/GraphEditingTests.cpp
        tests/GuideTests.cpp
        tests/HostedDevice.h
        tests/IconTests.cpp
        tests/ImportTests.cpp
        tests/InspectorTests.cpp
        tests/LibraryTests.cpp
        tests/LimiterEditorTests.cpp
        tests/LimiterTests.cpp
        tests/LessonsTests.cpp
        tests/LiveInputTests.cpp
        tests/MessageQueueTests.cpp
        tests/MetronomeTests.cpp
        tests/ModulationModelTests.cpp
        tests/ModulationUiTests.cpp
        tests/MixerModelTests.cpp
        tests/MixerTests.cpp
        tests/MixerViewTests.cpp
        tests/MotionEffectTests.cpp
        tests/MultibandEditorTests.cpp
        tests/MultibandTests.cpp
        tests/NewProjectTests.cpp
        tests/NoteIDTests.cpp
        tests/NoteTests.cpp
        tests/NotesModelTests.cpp
        tests/PanelLayoutTests.cpp
        tests/PanelsTests.cpp
        tests/ParameterHandleTests.cpp
        tests/PianoRollTests.cpp
        tests/PlaybackTests.cpp
        tests/PlaylistModelTests.cpp
        tests/PluginKeysTests.cpp
        tests/PluginParametersTests.cpp
        tests/PluginWindowTests.cpp
        tests/PlaylistTests.cpp
        tests/PopupsTests.cpp
        tests/PresetTests.cpp
        tests/ProjectContentsTests.cpp
        tests/ProjectContextTests.cpp
        tests/ProjectTests.cpp
        tests/ProjectViewTests.cpp
        tests/ReadmePicture.cpp
        tests/RealTimePlayback.h
        tests/RealtimeAuditTests.cpp
        tests/RenderHarness.cpp
        tests/RenderHarness.h
        tests/RecordingTests.cpp
        tests/RoutingTests.cpp
        tests/ReverbEditorTests.cpp
        tests/ReverbTests.cpp
        tests/SandboxBenchmarks.cpp
        tests/SandboxTests.cpp
        tests/SandboxedPluginTests.cpp
        tests/SandboxedPlugins.h
        tests/SchemaTests.cpp
        tests/SectionTests.cpp
        tests/SelectionTests.cpp
        tests/SettingsTests.cpp
        tests/ShortcutTests.cpp
        tests/SidechainTests.cpp
        tests/SoundPackTestHelpers.h
        tests/SoundPackTests.cpp
        tests/StartupSoundTests.cpp
        tests/StatusBarTests.cpp
        tests/StereoExpanderTests.cpp
        tests/StretchTests.cpp
        tests/SynthEditorTests.cpp
        tests/SynthTests.cpp
        tests/TestAudioDevice.cpp
        tests/TestAudioDevice.h
        tests/TestClips.h
        tests/TestDevices.h
        tests/TestPlugin.cpp
        tests/TestPlugin.h
        tests/TempoTests.cpp
        tests/TestSong.cpp
        tests/TestSong.h
        tests/TimelineGestureTests.cpp
        tests/TimelineModelTests.cpp
        tests/TimelineTests.cpp
        tests/TimelineViewTests.cpp
        tests/TitleBarTests.cpp
        tests/ToggleTests.cpp
        tests/TopBarTests.cpp
        tests/TrackTests.cpp
        tests/TransportModelTests.cpp
        tests/TruePeakTests.cpp
        tests/UiTestHelpers.cpp
        tests/UiTestHelpers.h
        tests/UndoTests.cpp
        tests/UpdatesTests.cpp
        tests/ValueTextTests.cpp
        tests/VersionStoreTests.cpp
        tests/VersionsBranchTests.cpp
        tests/VersionsGoingBackTests.cpp
        tests/VersionsHearingTests.cpp
        tests/VersionsMergeTests.cpp
        tests/VersionsTests.cpp
        tests/VocoderTests.cpp
        tests/WavetableEditorTests.cpp
        tests/WavetableModelTests.cpp
        tests/WavetableTests.cpp
        tests/WelcomeTests.cpp
        tests/WindowKeysTests.cpp
        tests/host/SandboxTestHelpers.h
        tests/host/TestPlugins.cpp
        tests/host/TestPlugins.h
        tests/host/ThreadCpu.cpp
        tests/host/ThreadCpu.h)

    phazite_use_core(phazite_tests)
    target_link_libraries(phazite_tests PRIVATE phazite_app phazite_ui)
    target_include_directories(phazite_tests SYSTEM PRIVATE external/doctest) # its warnings off: it is third-party

    # The tests' helper: the app's, with the tests' stand-ins for plug-ins (tests/host/TestPlugins.h), which the tests
    # start in its place.
    juce_add_gui_app(phazite_test_host
        PRODUCT_NAME "phazite_test_host"
        COMPANY_NAME "Phazite"
        BUNDLE_ID "io.github.august-byrne.phazite.testhost"
        NEEDS_CURL FALSE
        NEEDS_WEB_BROWSER FALSE)

    target_sources(phazite_test_host PRIVATE
        src/host/app/HostMain.cpp
        tests/host/TestFormats.cpp
        tests/host/TestPlugins.cpp
        tests/host/TestPlugins.h)
    target_compile_definitions(phazite_test_host PRIVATE JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:phazite_test_host,JUCE_VERSION>")
    phazite_use_core(phazite_test_host)
    target_link_libraries(phazite_test_host PRIVATE phazite_host)

    add_dependencies(phazite_tests phazite_test_host)
    target_compile_definitions(phazite_tests PRIVATE PHAZITE_TEST_HOST="$<TARGET_FILE:phazite_test_host>")

    # The licences' folder as the build writes it for the app (cmake/PhaziteNotices.cmake), which the tests check against the
    # list of notices.
    target_compile_definitions(phazite_tests PRIVATE PHAZITE_NOTICES_DIR="${CMAKE_BINARY_DIR}/notices")

    # A CTest test for each test file, which runs phazite_tests with that file's tests only, so that CTest can run them
    # side by side (the test presets run 4 at once; -j <n> changes that). Each test program has a temporary folder of its
    # own (tests/TestMain.cpp). OtherTestFiles runs the tests in every other file, so that no test is left out.
    get_target_property(_phazite_test_sources phazite_tests SOURCES)
    list(FILTER _phazite_test_sources INCLUDE REGEX "Tests\\.cpp$")
    set(_phazite_test_patterns "")
    set(_phazite_test_names "")

    foreach(_phazite_source IN LISTS _phazite_test_sources)
        get_filename_component(_phazite_test_name "${_phazite_source}" NAME_WE)
        # doctest matches the whole path the compiler was given, with either slash: *tests?MixerTests.cpp leaves out
        # tests/GoldenMixerTests.cpp.
        string(REPLACE "/" "?" _phazite_pattern "*${_phazite_source}")
        add_test(NAME ${_phazite_test_name} COMMAND phazite_tests "--source-file=${_phazite_pattern}" --duration=true)
        list(APPEND _phazite_test_patterns "${_phazite_pattern}")
        list(APPEND _phazite_test_names ${_phazite_test_name})
    endforeach()

    list(JOIN _phazite_test_patterns "," _phazite_test_patterns)
    add_test(NAME OtherTestFiles COMMAND phazite_tests "--source-file-exclude=${_phazite_test_patterns}" --duration=true)

    # A test file that hangs fails after 10 minutes (the slowest takes about 80 s in CI's slowest builds) rather than
    # holding CI until its job's limit; what it printed (each test's duration as it ends, --duration) names the last test
    # that ended, and the file's next one hung.
    set_tests_properties(${_phazite_test_names} OtherTestFiles PROPERTIES TIMEOUT 600)
endif()
