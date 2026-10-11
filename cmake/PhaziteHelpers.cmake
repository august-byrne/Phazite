# Helper functions for Phazite's top-level CMakeLists.txt.

# Stops with a clear message if a git submodule has not been checked out.
function(phazite_require_submodules)
    foreach(_dir IN LISTS ARGN)
        if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${_dir}/CMakeLists.txt")
            message(FATAL_ERROR
                "${_dir} is empty because the git submodules are not checked out. Run:\n"
                "  git submodule update --init --recursive --depth 1\n"
                "On a new Windows machine run tools\\setup-windows.cmd first: it lets git fetch JUCE over HTTPS.")
        endif()
    endforeach()
endfunction()

# Limits parallel compiles by RAM, because Tracktion's and JUCE's large source files need a lot of memory
# each. Only Ninja supports job pools; other generators ignore this.
function(phazite_limit_parallel_compiles mib_per_job)
    if(NOT CMAKE_GENERATOR MATCHES "Ninja")
        return()
    endif()
    cmake_host_system_information(RESULT _ram_mib QUERY TOTAL_PHYSICAL_MEMORY)
    cmake_host_system_information(RESULT _cpus QUERY NUMBER_OF_LOGICAL_CORES)
    math(EXPR _jobs "${_ram_mib} / ${mib_per_job}")
    if(_jobs GREATER _cpus)
        set(_jobs ${_cpus})
    elseif(_jobs LESS 1)
        set(_jobs 1)
    endif()
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS phazite_compile=${_jobs})
    set(CMAKE_JOB_POOL_COMPILE phazite_compile PARENT_SCOPE)
    message(STATUS "Phazite: up to ${_jobs} parallel compiles (${_ram_mib} MiB RAM, ${_cpus} logical CPUs)")
endfunction()

# JUCE builds its juceaide helper in <build>/external/tracktion_engine/modules/juce/tools and reuses that
# folder on every configure, even after `cmake --preset <name> --fresh`. After a compiler update it would
# keep using the old (deleted) compiler, so delete the folder whenever the compiler, the toolchain file or
# the source location changes. The stamp file sits outside CMakeFiles/, so --fresh keeps it.
function(phazite_reset_stale_juceaide tools_dir)
    set(_key "${CMAKE_CXX_COMPILER}|${CMAKE_TOOLCHAIN_FILE}|${CMAKE_SOURCE_DIR}")
    set(_stamp "${CMAKE_BINARY_DIR}/juceaide-tree.stamp")
    set(_old "")
    if(EXISTS "${_stamp}")
        file(READ "${_stamp}" _old)
    endif()
    if(NOT _old STREQUAL _key)
        file(REMOVE_RECURSE "${tools_dir}")
        file(WRITE "${_stamp}" "${_key}")
    endif()
endfunction()

# Checks that JUCE's nested juceaide configure used the same toolchain file as this build. Without that
# hand-off a Windows machine without a Developer PowerShell fails, but a CI runner would silently build
# juceaide with whatever gcc or clang is on its PATH, so turn that case into an error everywhere.
function(phazite_check_juceaide_toolchain tools_dir)
    if(NOT CMAKE_HOST_WIN32 OR NOT CMAKE_TOOLCHAIN_FILE OR NOT EXISTS "${tools_dir}/CMakeCache.txt")
        return()
    endif()
    load_cache("${tools_dir}" READ_WITH_PREFIX _juceaide_ CMAKE_TOOLCHAIN_FILE)
    file(TO_CMAKE_PATH "${_juceaide_CMAKE_TOOLCHAIN_FILE}" _theirs)
    file(TO_CMAKE_PATH "${CMAKE_TOOLCHAIN_FILE}" _ours)
    string(TOLOWER "${_theirs}" _theirs)
    string(TOLOWER "${_ours}" _ours)
    if(NOT _theirs STREQUAL _ours)
        message(FATAL_ERROR "JUCE's juceaide helper was configured without ${CMAKE_TOOLCHAIN_FILE} "
                            "(it used '${_theirs}'). Delete ${CMAKE_BINARY_DIR} and configure again with cmake --preset.")
    endif()
endfunction()
