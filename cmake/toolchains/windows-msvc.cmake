# Phazite's Windows toolchain file: a thin wrapper around MarkSchofield/WindowsToolchain (MIT licence,
# external/WindowsToolchain). It locates MSVC and the Windows SDK itself, so builds need no Developer
# PowerShell and no vcvars.
#
# CMake reads this file for the main configure, for every try_compile check, and (through the
# CMAKE_TOOLCHAIN_FILE environment variable set in CMakePresets.json and CMakeLists.txt) for nested
# CMake runs such as JUCE's juceaide bootstrap. The values resolved here are passed down to those nested
# runs through PHAZITE_* environment variables, so all of them use the same Visual Studio and MSVC.
include_guard()

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "cmake/toolchains/windows-msvc.cmake only supports Windows hosts.")
endif()

set(_phazite_upstream "${CMAKE_CURRENT_LIST_DIR}/../../external/WindowsToolchain/Windows.MSVC.toolchain.cmake")
if(NOT EXISTS "${_phazite_upstream}")
    message(FATAL_ERROR "external/WindowsToolchain is missing. Run: git submodule update --init --recursive --depth 1")
endif()

set(_phazite_shared_vars CMAKE_VS_VERSION_RANGE VS_INSTALLATION_PATH VS_PLATFORM_TOOLSET_VERSION)

# Inside a nested CMake run, take the values the parent run already chose.
foreach(_phazite_var IN LISTS _phazite_shared_vars)
    if(NOT ${_phazite_var} AND DEFINED ENV{PHAZITE_${_phazite_var}})
        set(${_phazite_var} "$ENV{PHAZITE_${_phazite_var}}")
    endif()
endforeach()

# Visual Studio 2026 (version 18.x) only: Build Tools locally, Enterprise on GitHub's windows-2025 runners.
if(NOT CMAKE_VS_VERSION_RANGE)
    set(CMAKE_VS_VERSION_RANGE "[18.0,19.0)")
endif()

# Upstream asks vswhere for the newest instance without checking that it has the C++ tools.
# Choose the instance here and require the x64/x86 MSVC compiler. vswhere only reports complete
# instances, so an install that is still waiting for a reboot is not found.
if(NOT VS_INSTALLATION_PATH)
    set(_phazite_vswhere "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer/vswhere.exe")
    if(NOT EXISTS "${_phazite_vswhere}")
        message(FATAL_ERROR "vswhere.exe was not found, so Visual Studio Build Tools 2026 is not installed. "
                            "Run tools/setup-windows.cmd.")
    endif()
    execute_process(
        COMMAND "${_phazite_vswhere}" -latest -products * -version "${CMAKE_VS_VERSION_RANGE}"
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                -property installationPath
        OUTPUT_VARIABLE VS_INSTALLATION_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT VS_INSTALLATION_PATH)
        message(FATAL_ERROR "No complete Visual Studio ${CMAKE_VS_VERSION_RANGE} installation with the MSVC "
                            "x64/x86 tools was found. Run tools/setup-windows.cmd, and reboot first if it said "
                            "a reboot is required.")
    endif()
    file(TO_CMAKE_PATH "${VS_INSTALLATION_PATH}" VS_INSTALLATION_PATH)
endif()

# Upstream picks the highest folder under VC/Tools/MSVC, which would include a Preview toolset if one
# were ever installed. Use the installation's default toolset instead, as vcvars does.
if(NOT VS_PLATFORM_TOOLSET_VERSION)
    set(_phazite_default_txt "${VS_INSTALLATION_PATH}/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt")
    if(EXISTS "${_phazite_default_txt}")
        file(STRINGS "${_phazite_default_txt}" VS_PLATFORM_TOOLSET_VERSION LIMIT_COUNT 1)
        string(STRIP "${VS_PLATFORM_TOOLSET_VERSION}" VS_PLATFORM_TOOLSET_VERSION)
    endif()
endif()

# Hand the resolved values down to nested CMake runs (they inherit this process's environment).
foreach(_phazite_var IN LISTS _phazite_shared_vars)
    set(ENV{PHAZITE_${_phazite_var}} "${${_phazite_var}}")
endforeach()

# Let CMake find the winget-installed Ninja even from a program that was started before Ninja was
# installed (these folders are searched after PATH, so a Ninja on PATH still wins).
list(APPEND CMAKE_SYSTEM_PROGRAM_PATH "$ENV{ProgramFiles}/WinGet/Links" "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Links")

include("${_phazite_upstream}")
