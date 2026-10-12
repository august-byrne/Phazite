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

# --- phazite_core: Tracktion Engine, JUCE and Phazite's model code, compiled once ---
# A static library that the app and the tests both link. JUCE's modules are INTERFACE libraries that
# add their .cpp files to whichever target links them, so only this library links them (PRIVATE, which
# for a static library still passes their system libraries on to the executables). The executables get
# the module headers and the same definitions through phazite_use_core() below, as JUCE does for its
# plug-in wrapper targets. All JUCE and Tracktion options therefore go here, so every file agrees.
add_library(phazite_core STATIC)

target_sources(phazite_core PRIVATE
    src/model/AppInfo.cpp
    src/model/AppInfo.h
    src/model/Clips.cpp
    src/model/Clips.h
    src/model/Commands.cpp
    src/model/Commands.h
    src/model/ComputerKeyboard.cpp
    src/model/ComputerKeyboard.h
    src/model/EngineSetup.cpp
    src/model/EngineSetup.h
    src/model/LiveInput.cpp
    src/model/LiveInput.h
    src/model/Project.cpp
    src/model/Project.h
    src/model/Selection.cpp
    src/model/Selection.h
    src/model/SmartTool.cpp
    src/model/SmartTool.h
    src/model/StarterProject.cpp
    src/model/StarterProject.h
    src/model/Timeline.cpp
    src/model/Timeline.h
    src/model/Tracks.cpp
    src/model/Tracks.h
    src/model/ValueText.cpp
    src/model/ValueText.h)

# Small data files compiled into the program (PhaziteData.h, namespace PhaziteData). Fonts don't go
# here: CLAUDE.md requires them as separate files next to the app.
juce_add_binary_data(phazite_data
    HEADER_NAME PhaziteData.h
    NAMESPACE PhaziteData
    SOURCES
        resources/keymaps/phazite.json
        resources/keymaps/fl_studio.json
        # Lucide 1.55.0 icons (ISC; those that come from Feather: MIT): resources/icons/lucide/LICENSE
        resources/icons/lucide/app-window.svg
        resources/icons/lucide/eraser.svg
        resources/icons/lucide/keyboard-music.svg
        resources/icons/lucide/magnet.svg
        resources/icons/lucide/mouse-pointer-2.svg
        resources/icons/lucide/move-horizontal.svg
        resources/icons/lucide/paintbrush.svg
        resources/icons/lucide/pencil.svg
        resources/icons/lucide/play.svg
        resources/icons/lucide/plus.svg
        resources/icons/lucide/scissors.svg
        resources/icons/lucide/search.svg
        resources/icons/lucide/square-dashed-mouse-pointer.svg
        resources/icons/lucide/square.svg
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

# Links an executable to phazite_core and gives it the module headers and phazite_core's definitions
# (including JUCE's JUCE_MODULE_AVAILABLE_* ones), without compiling the modules a second time.
function(phazite_use_core target)
    target_include_directories(${target} PRIVATE $<TARGET_PROPERTY:phazite_core,INCLUDE_DIRECTORIES>)
    target_compile_definitions(${target} PRIVATE $<TARGET_PROPERTY:phazite_core,COMPILE_DEFINITIONS>)
    target_link_libraries(${target} PRIVATE phazite_core)
endfunction()

# --- The app ---
juce_add_gui_app(Phazite
    PRODUCT_NAME "Phazite"
    COMPANY_NAME "Phazite"
    BUNDLE_ID "io.github.august-byrne.phazite"
    MICROPHONE_PERMISSION_ENABLED TRUE
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE
    PLUGINHOST_AU TRUE)

target_sources(Phazite PRIVATE
    src/app/Main.cpp
    src/app/MainWindow.cpp
    src/app/MainWindow.h
    src/app/ProjectSession.cpp
    src/app/ProjectSession.h
    src/ui/AppShell.cpp
    src/ui/AppShell.h
    src/ui/ChannelStrip.cpp
    src/ui/ChannelStrip.h
    src/ui/ClipComponent.cpp
    src/ui/ClipComponent.h
    src/ui/CommandPalette.cpp
    src/ui/CommandPalette.h
    src/ui/Controls.cpp
    src/ui/Controls.h
    src/ui/DesignTokens.h
    src/ui/HintBar.cpp
    src/ui/HintBar.h
    src/ui/Icons.cpp
    src/ui/Icons.h
    src/ui/LookAndFeel.cpp
    src/ui/LookAndFeel.h
    src/ui/Panels.cpp
    src/ui/Panels.h
    src/ui/Playlist.cpp
    src/ui/Playlist.h
    src/ui/SettingsPanel.cpp
    src/ui/SettingsPanel.h
    src/ui/TopBar.cpp
    src/ui/TopBar.h
    src/ui/TrackHeader.cpp
    src/ui/TrackHeader.h)

target_compile_definitions(Phazite PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Phazite,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Phazite,JUCE_VERSION>")

phazite_use_core(Phazite)

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

# --- Tests: doctest (MIT), bundled with Tracktion Engine. Run them with `ctest --preset <preset>`. ---
option(PHAZITE_BUILD_TESTS "Build Phazite's unit tests (phazite_tests)" ON)

if(PHAZITE_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(phazite_tests)

    target_sources(phazite_tests PRIVATE
        tests/TestMain.cpp
        tests/TestHelpers.h
        tests/AppInfoTests.cpp
        tests/AudioThreadMonitor.cpp
        tests/AudioThreadMonitor.h
        tests/ClipTests.cpp
        tests/CommandTests.cpp
        tests/ComputerKeyboardTests.cpp
        tests/ControlsTests.cpp
        tests/DesignTokenTests.cpp
        tests/EngineSetupTests.cpp
        tests/IconTests.cpp
        tests/LiveInputTests.cpp
        tests/ProjectTests.cpp
        tests/RealtimeAuditTests.cpp
        tests/TimelineTests.cpp
        tests/TrackTests.cpp
        tests/ValueTextTests.cpp
        # The UI files the tests use: the base controls, which link to the engine's parameters, and the icons.
        src/ui/Controls.cpp
        src/ui/Icons.cpp
        src/ui/LookAndFeel.cpp)

    phazite_use_core(phazite_tests)

    add_test(NAME phazite_tests COMMAND phazite_tests)
endif()
