# Phazite's notices: its own licence and the notices of the code and fonts from others it is built with
# (resources/licenses/notices.json), as they go next to the app and into its installer.
#
# phazite_write_notices(<folder>) writes, as CMake configures:
#   <folder>/licenses/Phazite.txt   Phazite's licence (LICENSE: the GNU AGPL, version 3)
#   <folder>/licenses/<file>        each entry's sources, joined, for this platform's entries
#   <folder>/NOTICES.txt            Phazite's notice, every entry's name, version, licence and copyright, and the trademarks
# and removes any other file in <folder>/licenses. Each file is written only when what it holds changes, and a change to the
# list or to any of its sources configures again. The list's and the copyright's texts are read from the variables
# PROJECT_VERSION, PHAZITE_COPYRIGHT and PHAZITE_SOURCE_URL.

# Writes a text with its lines ended as the system ends them: file(WRITE) ends each "\n" with CR LF on Windows, so lines
# that end so already (a file checked out with CR LF) are LF first.
function(_phazite_write_if_changed path content)
    string(REPLACE "\r\n" "\n" content "${content}")

    if(EXISTS "${path}")
        file(READ "${path}" _old)
        string(REPLACE "\r\n" "\n" _old "${_old}")

        if(_old STREQUAL content)
            return()
        endif()
    endif()

    file(WRITE "${path}" "${content}")
endfunction()

# Whether a notice or trademark (the JSON object <json>) is for this build: one without "platforms" is for all.
function(_phazite_is_for_this_platform json result)
    string(JSON _count ERROR_VARIABLE _missing LENGTH "${json}" platforms)

    if(_missing)
        set(${result} TRUE PARENT_SCOPE)
        return()
    endif()

    if(WIN32)
        set(_this windows)
    elseif(APPLE)
        set(_this macos)
    else()
        set(_this linux)
    endif()

    set(${result} FALSE PARENT_SCOPE)

    if(_count GREATER 0)
        math(EXPR _last "${_count} - 1")

        foreach(_i RANGE ${_last})
            string(JSON _platform GET "${json}" platforms ${_i})

            if(_platform STREQUAL _this)
                set(${result} TRUE PARENT_SCOPE)
            endif()
        endforeach()
    endif()
endfunction()

function(phazite_write_notices folder)
    set(_list "${CMAKE_SOURCE_DIR}/resources/licenses/notices.json")
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_list}" "${CMAKE_SOURCE_DIR}/LICENSE")
    file(READ "${_list}" _json)
    file(MAKE_DIRECTORY "${folder}/licenses")

    # Windows' Notepad and the installer's pages read a UTF-8 file by its byte order mark (and lines by CR LF, which
    # _phazite_write_if_changed() gives them there).
    set(_nl "\n")
    string(ASCII 239 187 191 _bom)
    file(READ "${CMAKE_SOURCE_DIR}/LICENSE" _licence)
    _phazite_write_if_changed("${folder}/licenses/Phazite.txt" "${_licence}")
    set(_written "Phazite.txt")

    set(_text "${_bom}Phazite ${PROJECT_VERSION}${_nl}${PHAZITE_COPYRIGHT}${_nl}${_nl}")
    string(APPEND _text "Phazite is free software: you can redistribute it and/or modify it under the terms of the GNU Affero${_nl}"
                        "General Public License as published by the Free Software Foundation, either version 3 of the License,${_nl}"
                        "or (at your option) any later version. It is distributed in the hope that it will be useful, but${_nl}"
                        "WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR${_nl}"
                        "PURPOSE. See the GNU Affero General Public License for more details: licenses/Phazite.txt.${_nl}"
                        "Its source code: ${PHAZITE_SOURCE_URL}${_nl}${_nl}"
                        "Phazite is built with this code and these fonts from others, each under its own license, whose text${_nl}"
                        "is in the licenses folder next to Phazite:${_nl}")

    string(JSON _count LENGTH "${_json}" notices)
    math(EXPR _last "${_count} - 1")

    foreach(_i RANGE ${_last})
        string(JSON _notice GET "${_json}" notices ${_i})
        _phazite_is_for_this_platform("${_notice}" _included)

        if(NOT _included)
            continue()
        endif()

        string(JSON _name GET "${_notice}" name)
        string(JSON _version GET "${_notice}" version)
        string(JSON _kind GET "${_notice}" licence)
        string(JSON _copyright GET "${_notice}" copyright)
        string(JSON _use GET "${_notice}" use)
        string(JSON _file GET "${_notice}" file)
        string(JSON _sources LENGTH "${_notice}" sources)

        set(_heading "${_name}")

        if(NOT _version STREQUAL "")
            string(APPEND _heading " ${_version}")
        endif()

        string(APPEND _text "${_nl}${_heading} (${_kind})")

        if(NOT _file STREQUAL "")
            string(APPEND _text ": licenses/${_file}")
        endif()

        string(APPEND _text "${_nl}  ${_copyright}${_nl}  ${_use}.${_nl}")

        # The entry's file: its sources, one after another, a blank line between them.
        if(NOT _file STREQUAL "" AND _sources GREATER 0)
            set(_content "")
            math(EXPR _lastSource "${_sources} - 1")

            foreach(_j RANGE ${_lastSource})
                string(JSON _source GET "${_notice}" sources ${_j})
                set(_path "${CMAKE_SOURCE_DIR}/${_source}")

                if(NOT EXISTS "${_path}")
                    message(FATAL_ERROR "resources/licenses/notices.json: ${_name}'s notice, ${_source}, isn't there. If it is in a "
                                        "submodule, run: git submodule update --init --recursive --depth 1")
                endif()

                set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path}")
                file(READ "${_path}" _part)

                if(NOT _content STREQUAL "")
                    string(APPEND _content "${_nl}${_nl}")
                endif()

                string(APPEND _content "${_part}")
            endforeach()

            _phazite_write_if_changed("${folder}/licenses/${_file}" "${_content}")
            list(APPEND _written "${_file}")
        endif()
    endforeach()

    string(APPEND _text "${_nl}Trademarks${_nl}")
    string(JSON _count LENGTH "${_json}" trademarks)
    math(EXPR _last "${_count} - 1")

    foreach(_i RANGE ${_last})
        string(JSON _trademark GET "${_json}" trademarks ${_i})
        _phazite_is_for_this_platform("${_trademark}" _included)

        if(_included)
            string(JSON _sentence GET "${_trademark}" text)
            string(APPEND _text "${_sentence}${_nl}")
        endif()
    endforeach()

    _phazite_write_if_changed("${folder}/NOTICES.txt" "${_text}")

    # Nothing left over from an entry that went or moved to another file.
    file(GLOB _present RELATIVE "${folder}/licenses" "${folder}/licenses/*")

    foreach(_file IN LISTS _present)
        if(NOT _file IN_LIST _written)
            file(REMOVE "${folder}/licenses/${_file}")
        endif()
    endforeach()
endfunction()
