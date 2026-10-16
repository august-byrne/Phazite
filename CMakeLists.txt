# Phazite: an open-source DAW (AGPL-3.0) built on Tracktion Engine and JUCE.
#
# Configure and build through the presets, with the same two commands on every OS:
#   cmake --preset windows-debug
#   cmake --build --preset windows-debug
# `cmake --list-presets` shows the presets available on the current machine.

cmake_minimum_required(VERSION 3.28...4.4)

project(Phazite VERSION 0.1.0 LANGUAGES C CXX)   # JUCE needs the C language enabled as well

include("${CMAKE_CURRENT_LIST_DIR}/cmake/PhaziteHelpers.cmake")

if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR "On Windows, configure through the windows-* presets (MSVC). "
                        "This configure found ${CMAKE_CXX_COMPILER_ID} instead.")
endif()

# --- MSVC settings. They only affect targets created after this point; other compilers ignore them. ---
# Static C runtime, as in Tracktion's own examples: the app runs without a VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Debug info inside each .obj (/Z7) instead of one shared .pdb that parallel compiles queue up for.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")

if(MSVC)
    message(STATUS "Phazite: MSVC ${CMAKE_CXX_COMPILER_VERSION} from ${VS_INSTALLATION_PATH}, "
                   "Windows SDK ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
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

# --- Dependencies: Tracktion Engine, which also adds the JUCE version it pins (modules/juce). ---
phazite_require_submodules(external/tracktion_engine external/tracktion_engine/modules/juce)

set(_phazite_juceaide_dir "${CMAKE_BINARY_DIR}/external/tracktion_engine/modules/juce/tools")
phazite_reset_stale_juceaide("${_phazite_juceaide_dir}")

set(TE_ADD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(external/tracktion_engine EXCLUDE_FROM_ALL)   # keeps their install rules out of ours

phazite_check_juceaide_toolchain("${_phazite_juceaide_dir}")

# --- phazite_core: Tracktion Engine, JUCE and Phazite's model and services code, compiled once ---
# A static library that the app and the tests both link. JUCE's modules are INTERFACE libraries that
# add their .cpp files to whichever target links them, so only this library links them (PRIVATE, which
# for a static library still passes their system libraries on to the executables). The executables get
# the module headers and the same definitions through phazite_use_core() below, as JUCE does for its
# plug-in wrapper targets. All JUCE and Tracktion options therefore go here, so every file agrees.
add_library(phazite_core STATIC)

target_sources(phazite_core PRIVATE
    src/devices/core/DeviceInfo.cpp
    src/devices/core/DeviceInfo.h
    src/devices/core/MeterValue.h
    src/devices/core/Processor.h
    src/devices/core/SampleTap.h
    src/devices/core/Spectrum.cpp
    src/devices/core/Spectrum.h
    src/devices/drums/DrumSampler.cpp
    src/devices/drums/DrumSampler.h
    src/devices/drums/DrumSynthesis.cpp
    src/devices/drums/DrumSynthesis.h
    src/devices/dsp/DelayLine.h
    src/devices/dsp/Envelopes.h
    src/devices/dsp/Filters.cpp
    src/devices/dsp/Filters.h
    src/devices/dsp/Math.h
    src/devices/dsp/Oscillators.h
    src/devices/dsp/Random.h
    src/devices/dynamics/Compressor.cpp
    src/devices/dynamics/Compressor.h
    src/devices/dynamics/Limiter.cpp
    src/devices/dynamics/Limiter.h
    src/devices/eq/Equalizer.cpp
    src/devices/eq/Equalizer.h
    src/devices/reverb/Reverb.cpp
    src/devices/reverb/Reverb.h
    src/devices/synth/Synth.cpp
    src/devices/synth/Synth.h
    src/model/arrangement/Arrangement.cpp
    src/model/arrangement/Arrangement.h
    src/model/arrangement/Clips.cpp
    src/model/arrangement/Clips.h
    src/model/arrangement/Grid.cpp
    src/model/arrangement/Grid.h
    src/model/arrangement/Import.cpp
    src/model/arrangement/Import.h
    src/model/arrangement/Outlines.cpp
    src/model/arrangement/Outlines.h
    src/model/arrangement/Sections.cpp
    src/model/arrangement/Sections.h
    src/model/arrangement/Selection.cpp
    src/model/arrangement/Selection.h
    src/model/arrangement/SongSelection.cpp
    src/model/arrangement/SongSelection.h
    src/model/arrangement/TrackColours.cpp
    src/model/arrangement/TrackColours.h
    src/model/arrangement/Tracks.cpp
    src/model/arrangement/Tracks.h
    src/model/arrangement/internal/SongSelection.h
    src/model/base/Beats.h
    src/model/base/IDs.h
    src/model/base/Migrations.cpp
    src/model/base/Migrations.h
    src/model/base/Schema.cpp
    src/model/base/Schema.h
    src/model/base/Toggles.h
    src/model/base/Tracktion.h
    src/model/commands/CommandRegistry.cpp
    src/model/commands/CommandRegistry.h
    src/model/commands/Undo.cpp
    src/model/commands/Undo.h
    src/model/commands/UndoCommands.cpp
    src/model/commands/UndoCommands.h
    src/model/devices/DeviceHandle.cpp
    src/model/devices/DeviceHandle.h
    src/model/devices/DevicePlugin.cpp
    src/model/devices/DevicePlugin.h
    src/model/devices/DrumKit.cpp
    src/model/devices/DrumKit.h
    src/model/devices/DrumSamplerPlugin.cpp
    src/model/devices/DrumSamplerPlugin.h
    src/model/devices/Devices.cpp
    src/model/devices/Devices.h
    src/model/devices/ParameterHandle.cpp
    src/model/devices/ParameterHandle.h
    src/model/devices/internal/DeviceHandle.h
    src/model/devices/internal/ParameterHandle.h
    src/model/engine/AppInfo.cpp
    src/model/engine/AppInfo.h
    src/model/engine/EngineSetup.cpp
    src/model/engine/EngineSetup.h
    src/model/engine/LiveInput.cpp
    src/model/engine/LiveInput.h
    src/model/engine/LoudnessMeterPlugin.cpp
    src/model/engine/LoudnessMeterPlugin.h
    src/model/engine/Preview.cpp
    src/model/engine/Preview.h
    src/model/mixer/Buses.cpp
    src/model/mixer/Buses.h
    src/model/mixer/DeviceType.h
    src/model/mixer/LevelSource.cpp
    src/model/mixer/LevelSource.h
    src/model/mixer/MixHandle.cpp
    src/model/mixer/MixHandle.h
    src/model/mixer/Mixer.cpp
    src/model/mixer/Mixer.h
    src/model/mixer/MixerModel.cpp
    src/model/mixer/MixerModel.h
    src/model/mixer/Sidechain.cpp
    src/model/mixer/Sidechain.h
    src/model/mixer/internal/LevelSource.h
    src/model/mixer/internal/MixHandle.h
    src/model/notes/Notes.cpp
    src/model/notes/Notes.h
    src/model/project/Export.cpp
    src/model/project/Export.h
    src/model/project/NewProject.cpp
    src/model/project/NewProject.h
    src/model/project/Project.cpp
    src/model/project/Project.h
    src/model/project/ProjectContext.cpp
    src/model/project/ProjectContext.h
    src/model/project/StarterProject.cpp
    src/model/project/StarterProject.h
    src/model/project/UnsavedProjects.cpp
    src/model/project/UnsavedProjects.h
    src/model/transport/Capture.cpp
    src/model/transport/Capture.h
    src/model/transport/Metronome.cpp
    src/model/transport/Metronome.h
    src/model/transport/Playback.cpp
    src/model/transport/Playback.h
    src/model/transport/Recording.cpp
    src/model/transport/Recording.h
    src/model/transport/Tempo.cpp
    src/model/transport/Tempo.h
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
    src/services/SoundPack.h)

# Small data files compiled into the program (PhaziteData.h, namespace PhaziteData). Fonts don't go
# here: CLAUDE.md requires them as separate files next to the app.
juce_add_binary_data(phazite_data
    HEADER_NAME PhaziteData.h
    NAMESPACE PhaziteData
    SOURCES
        resources/keymaps/phazite.json
        resources/keymaps/fl_studio.json
        # Song outlines: every file named *.outline.json here is one (src/model/arrangement/Outlines.h)
        resources/outlines/pop.outline.json
        resources/outlines/rock.outline.json
        resources/outlines/electronic.outline.json
        resources/outlines/hip_hop.outline.json
        resources/outlines/lofi.outline.json
        resources/outlines/rnb.outline.json
        resources/outlines/drum_and_bass.outline.json
        resources/outlines/techno.outline.json
        # Lucide 1.55.0 icons (ISC; those that come from Feather: MIT): resources/icons/lucide/LICENSE
        resources/icons/lucide/app-window.svg
        resources/icons/lucide/audio-lines.svg
        resources/icons/lucide/chevron-down.svg
        resources/icons/lucide/chevron-right.svg
        resources/icons/lucide/circle.svg
        resources/icons/lucide/clock-arrow-down.svg
        resources/icons/lucide/clock.svg
        resources/icons/lucide/download.svg
        resources/icons/lucide/eraser.svg
        resources/icons/lucide/folder-plus.svg
        resources/icons/lucide/folder.svg
        resources/icons/lucide/grip-vertical.svg
        resources/icons/lucide/headphones.svg
        resources/icons/lucide/keyboard-music.svg
        resources/icons/lucide/magnet.svg
        resources/icons/lucide/metronome.svg
        resources/icons/lucide/mouse-pointer-2.svg
        resources/icons/lucide/move-horizontal.svg
        resources/icons/lucide/music.svg
        resources/icons/lucide/paintbrush.svg
        resources/icons/lucide/pencil.svg
        resources/icons/lucide/piano.svg
        resources/icons/lucide/play.svg
        resources/icons/lucide/plus.svg
        resources/icons/lucide/repeat.svg
        resources/icons/lucide/scissors.svg
        resources/icons/lucide/search.svg
        resources/icons/lucide/sliders-horizontal.svg
        resources/icons/lucide/square-dashed-mouse-pointer.svg
        resources/icons/lucide/square.svg
        resources/icons/lucide/star.svg
        resources/icons/lucide/volume-x.svg
        resources/icons/lucide/zoom-in.svg)

target_link_libraries(phazite_core PRIVATE phazite_data)

target_include_directories(phazite_core PUBLIC src)
target_compile_features(phazite_core PUBLIC cxx_std_20)

target_compile_definitions(phazite_core PRIVATE
    PHAZITE_VERSION_STRING="${PROJECT_VERSION}"
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1)

if(WIN32)
    # JUCE's headers define these anyway; defining them here as well keeps any code that includes
    # <windows.h> before JUCE consistent.
    target_compile_definitions(phazite_core PRIVATE UNICODE _UNICODE)
    # ASIO drivers, the low-latency standard for audio interfaces on Windows. JUCE bundles Steinberg's
    # ASIO SDK 2.3.4 headers, which Phazite uses under the SDK's GPLv3 option (AGPL-compatible).
    target_compile_definitions(phazite_core PRIVATE JUCE_ASIO=1)
endif()

target_link_libraries(phazite_core
    PRIVATE
        tracktion::tracktion_core
        tracktion::tracktion_engine
        tracktion::tracktion_graph
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_cryptography # SHA-256, to check downloads
    PUBLIC
        juce::juce_recommended_warning_flags) # for Phazite's own files in every target, too

# Tracktion's and JUCE's .cpp files compile as part of phazite_core, so they get these warnings too. One
# Tracktion line is unreachable while JUCE_CATCH_UNHANDLED_EXCEPTIONS is 0 (JUCE's default), and MSVC
# reports it (C4702). Silence that warning for that one file, so every warning a build prints is ours.
if(MSVC)
    set_source_files_properties(external/tracktion_engine/modules/tracktion_engine/tracktion_engine_playback.cpp
        PROPERTIES COMPILE_OPTIONS "/wd4702")
endif()

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
    src/ui/browser/Browser.cpp
    src/ui/browser/Browser.h
    src/ui/browser/PluginPicker.cpp
    src/ui/browser/PluginPicker.h
    src/ui/components/Controls.cpp
    src/ui/components/Controls.h
    src/ui/components/ValueText.cpp
    src/ui/components/ValueText.h
    src/ui/devices/CompressorEditor.cpp
    src/ui/devices/CompressorEditor.h
    src/ui/devices/DeviceEditors.cpp
    src/ui/devices/DeviceEditors.h
    src/ui/devices/DeviceMenus.cpp
    src/ui/devices/DeviceMenus.h
    src/ui/devices/DevicePanel.cpp
    src/ui/devices/DevicePanel.h
    src/ui/devices/DrumSamplerEditor.cpp
    src/ui/devices/DrumSamplerEditor.h
    src/ui/devices/EqEditor.cpp
    src/ui/devices/EqEditor.h
    src/ui/devices/LimiterEditor.cpp
    src/ui/devices/LimiterEditor.h
    src/ui/devices/ReverbEditor.cpp
    src/ui/devices/ReverbEditor.h
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
    src/ui/mixer/Inspector.cpp
    src/ui/mixer/Inspector.h
    src/ui/mixer/MasterStrip.cpp
    src/ui/mixer/MasterStrip.h
    src/ui/mixer/MixerView.cpp
    src/ui/mixer/MixerView.h
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
    src/ui/playlist/ClipComponent.cpp
    src/ui/playlist/ClipComponent.h
    src/ui/playlist/Playlist.cpp
    src/ui/playlist/Playlist.h
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
    src/ui/shell/AppShell.cpp
    src/ui/shell/AppShell.h
    src/ui/shell/CommandPalette.cpp
    src/ui/shell/CommandPalette.h
    src/ui/shell/ExportPanel.cpp
    src/ui/shell/ExportPanel.h
    src/ui/shell/Guide.cpp
    src/ui/shell/Guide.h
    src/ui/shell/HintBar.cpp
    src/ui/shell/HintBar.h
    src/ui/shell/NewProjectPanel.cpp
    src/ui/shell/NewProjectPanel.h
    src/ui/shell/Panels.cpp
    src/ui/shell/Panels.h
    src/ui/shell/SettingsPanel.cpp
    src/ui/shell/SettingsPanel.h
    src/ui/shell/TopBar.cpp
    src/ui/shell/TopBar.h
    src/ui/shell/Welcome.cpp
    src/ui/shell/Welcome.h
    src/ui/theme/DesignTokens.h
    src/ui/theme/Icons.cpp
    src/ui/theme/Icons.h
    src/ui/theme/LookAndFeel.cpp
    src/ui/theme/LookAndFeel.h
    src/ui/timeline/SmartTool.cpp
    src/ui/timeline/SmartTool.h
    src/ui/timeline/TimelineEditor.cpp
    src/ui/timeline/TimelineEditor.h
    src/ui/timeline/TimelineGesture.cpp
    src/ui/timeline/TimelineGesture.h
    src/ui/timeline/TimelineView.cpp
    src/ui/timeline/TimelineView.h)

phazite_use_core(phazite_ui)

# --- phazite_app: the app's parts (the main window, the open project, the song's commands), in a library the tests
# link, so the app's logic can be tested without a window. The Phazite executable is Main.cpp. ---
add_library(phazite_app STATIC)

target_sources(phazite_app PRIVATE
    src/app/AppCommands.cpp
    src/app/AppCommands.h
    src/app/AppServices.cpp
    src/app/AppServices.h
    src/app/AppUIBehaviour.cpp
    src/app/AppUIBehaviour.h
    src/app/MainWindow.cpp
    src/app/MainWindow.h
    src/app/PluginWindow.cpp
    src/app/PluginWindow.h
    src/app/ProjectSession.cpp
    src/app/ProjectSession.h)

phazite_use_core(phazite_app)
target_link_libraries(phazite_app PRIVATE phazite_ui)

# --- The app ---
juce_add_gui_app(Phazite
    PRODUCT_NAME "Phazite"
    COMPANY_NAME "Phazite"
    BUNDLE_ID "io.github.august-byrne.phazite"
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

# The UI font, Inter (SIL Open Font License 1.1), goes next to the app as separate files with its licence
# text (fonts/Inter), as CLAUDE.md asks of fonts. ui::LookAndFeel loads it from there.
add_custom_command(TARGET Phazite POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts" "$<TARGET_FILE_DIR:Phazite>/fonts"
    VERBATIM)

# The icons are compiled in, so their licence, which asks to go with every copy, goes next to the app.
add_custom_command(TARGET Phazite POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/lucide/LICENSE" "$<TARGET_FILE_DIR:Phazite>/licenses/Lucide.txt"
    VERBATIM)

# --- phazite_soundkit: renders Phazite Sounds, the pack Phazite offers to download, from the recipes in the
# phazite-sounds repository (CC0 sample libraries and its own synthesis), and zips it for a release. It is run by hand
# when the sounds change, not as part of the build. ---
juce_add_console_app(phazite_soundkit)
target_sources(phazite_soundkit PRIVATE tools/soundkit/SoundKit.cpp)
phazite_use_core(phazite_soundkit)

# --- Tests: doctest (MIT), bundled with Tracktion Engine. Run them with `ctest --preset <preset>`. ---
option(PHAZITE_BUILD_TESTS "Build Phazite's unit tests (phazite_tests)" ON)

if(PHAZITE_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(phazite_tests)

    target_sources(phazite_tests PRIVATE
        tests/TestMain.cpp
        tests/TestHelpers.h
        tests/AppInfoTests.cpp
        tests/AppShellTests.cpp
        tests/AppTests.cpp
        tests/AudioThreadMonitor.cpp
        tests/AudioThreadMonitor.h
        tests/BeatsTests.cpp
        tests/BrowserModelTests.cpp
        tests/BrowserTests.cpp
        tests/ClipTests.cpp
        tests/CommandGoldenTests.cpp
        tests/CommandTests.cpp
        tests/CompressorEditorTests.cpp
        tests/CompressorTests.cpp
        tests/ComputerKeyboardTests.cpp
        tests/ControlsTests.cpp
        tests/DesignTokenTests.cpp
        tests/DeviceEditorTests.cpp
        tests/DeviceInfoTests.cpp
        tests/DevicePanelTests.cpp
        tests/DevicePluginTests.cpp
        tests/DrumRowTests.cpp
        tests/DrumSamplerEditorTests.cpp
        tests/DrumSamplerTests.cpp
        tests/DspTests.cpp
        tests/EngineSetupTests.cpp
        tests/EqEditorTests.cpp
        tests/EqualizerTests.cpp
        tests/ExportTests.cpp
        tests/GoldenFiles.cpp
        tests/GoldenFiles.h
        tests/GoldenProjectTests.cpp
        tests/GoldenRenderTests.cpp
        tests/GuideTests.cpp
        tests/IconTests.cpp
        tests/ImportTests.cpp
        tests/LibraryTests.cpp
        tests/LimiterEditorTests.cpp
        tests/LimiterTests.cpp
        tests/LiveInputTests.cpp
        tests/MessageQueueTests.cpp
        tests/MetronomeTests.cpp
        tests/MigrationTests.cpp
        tests/MixerModelTests.cpp
        tests/MixerTests.cpp
        tests/MixerViewTests.cpp
        tests/NewProjectTests.cpp
        tests/NoteTests.cpp
        tests/ParameterHandleTests.cpp
        tests/PianoRollTests.cpp
        tests/PlaybackTests.cpp
        tests/PluginKeysTests.cpp
        tests/PlaylistTests.cpp
        tests/ProjectContextTests.cpp
        tests/ProjectTests.cpp
        tests/RealtimeAuditTests.cpp
        tests/RecordingTests.cpp
        tests/ReverbEditorTests.cpp
        tests/ReverbTests.cpp
        tests/SchemaTests.cpp
        tests/SectionTests.cpp
        tests/SelectionTests.cpp
        tests/SettingsTests.cpp
        tests/ShortcutTests.cpp
        tests/SidechainTests.cpp
        tests/SoundPackTestHelpers.h
        tests/SoundPackTests.cpp
        tests/SynthTests.cpp
        tests/TestDevices.h
        tests/TempoTests.cpp
        tests/TimelineGestureTests.cpp
        tests/TimelineTests.cpp
        tests/TimelineViewTests.cpp
        tests/ToggleTests.cpp
        tests/TopBarTests.cpp
        tests/TrackTests.cpp
        tests/UiTestHelpers.cpp
        tests/UiTestHelpers.h
        tests/UndoTests.cpp
        tests/ValueTextTests.cpp
        tests/WelcomeTests.cpp
        tests/WindowKeysTests.cpp)

    phazite_use_core(phazite_tests)
    target_link_libraries(phazite_tests PRIVATE phazite_app phazite_ui)

    add_test(NAME phazite_tests COMMAND phazite_tests)
endif()
