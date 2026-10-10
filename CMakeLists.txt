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
    src/Main.cpp
    src/MainComponent.cpp
    src/MainComponent.h)

target_compile_features(Phazite PRIVATE cxx_std_20)

target_compile_definitions(Phazite PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Phazite,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Phazite,JUCE_VERSION>"
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1)

if(WIN32)
    # JUCE's headers define these anyway; defining them here as well keeps any code that includes
    # <windows.h> before JUCE consistent.
    target_compile_definitions(Phazite PRIVATE UNICODE _UNICODE)
endif()

target_link_libraries(Phazite PRIVATE
    tracktion::tracktion_core
    tracktion::tracktion_engine
    tracktion::tracktion_graph
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_recommended_warning_flags)

# Tracktion's and JUCE's .cpp files compile as part of this target, so they get these warnings too. One
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
    target_link_libraries(Phazite PRIVATE juce::juce_recommended_config_flags)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(Phazite PRIVATE atomic)
endif()
