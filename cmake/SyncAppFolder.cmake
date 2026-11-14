# Makes what goes next to the app (the folder of Phazite's executable) exactly what the build has now, every time it runs
# (the phazite_app_folder target in CMakeLists.txt runs it on every build of the app):
# - each folder of data the app reads (fonts/, presets/, licenses/) the same as its source, file for file: a file the source
#   no longer has goes, and a file that differs is copied again;
# - each single file (the plug-ins' helper, NOTICES.txt) copied when it differs.
# A copy made only after the app links, or a folder copied over the last one, would keep what was deleted since (known bug
# 132): the installer packs this folder.
#   cmake -DAPP_FOLDER=<folder> "-DFOLDERS=<source>=<name>;..." "-DFILES=<file>;..." -P cmake/SyncAppFolder.cmake

if(NOT DEFINED APP_FOLDER)
    message(FATAL_ERROR "SyncAppFolder.cmake needs -DAPP_FOLDER=...")
endif()

file(MAKE_DIRECTORY "${APP_FOLDER}")

foreach(_pair IN LISTS FOLDERS)
    string(FIND "${_pair}" "=" _equals REVERSE)
    string(SUBSTRING "${_pair}" 0 ${_equals} _source)
    math(EXPR _start "${_equals} + 1")
    string(SUBSTRING "${_pair}" ${_start} -1 _name)
    set(_target "${APP_FOLDER}/${_name}")

    if(NOT IS_DIRECTORY "${_source}")
        message(FATAL_ERROR "SyncAppFolder.cmake: ${_source} is not a folder.")
    endif()

    file(GLOB_RECURSE _wanted LIST_DIRECTORIES false RELATIVE "${_source}" "${_source}/*")
    set(_present "")

    if(IS_DIRECTORY "${_target}")
        file(GLOB_RECURSE _present LIST_DIRECTORIES false RELATIVE "${_target}" "${_target}/*")
    elseif(EXISTS "${_target}")
        file(REMOVE "${_target}")
    endif()

    # What the source no longer has goes, and then any folder left empty.
    foreach(_file IN LISTS _present)
        if(NOT _file IN_LIST _wanted)
            file(REMOVE "${_target}/${_file}")
            message(STATUS "Phazite: removed ${_name}/${_file}, which the build no longer has, from next to the app")
        endif()
    endforeach()

    if(IS_DIRECTORY "${_target}")
        file(GLOB_RECURSE _folders LIST_DIRECTORIES true RELATIVE "${_target}" "${_target}/*")
        list(SORT _folders ORDER DESCENDING) # the deepest first

        foreach(_folder IN LISTS _folders)
            if(IS_DIRECTORY "${_target}/${_folder}")
                file(GLOB _inside "${_target}/${_folder}/*")

                if(_inside STREQUAL "")
                    file(REMOVE_RECURSE "${_target}/${_folder}")
                endif()
            endif()
        endforeach()
    endif()

    foreach(_file IN LISTS _wanted)
        get_filename_component(_parent "${_target}/${_file}" DIRECTORY)
        file(MAKE_DIRECTORY "${_parent}")
        file(COPY_FILE "${_source}/${_file}" "${_target}/${_file}" ONLY_IF_DIFFERENT)
    endforeach()
endforeach()

foreach(_file IN LISTS FILES)
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "SyncAppFolder.cmake: ${_file} isn't there.")
    endif()

    get_filename_component(_name "${_file}" NAME)
    file(COPY_FILE "${_file}" "${APP_FOLDER}/${_name}" ONLY_IF_DIFFERENT)
endforeach()
