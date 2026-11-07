# Writes what a release of Phazite carries beside its installer (the installer target in CMakeLists.txt runs this after
# Inno Setup): the installer's SHA-256, as sha256sum checks it (<installer>.sha256), and the update file that Phazite's update
# check reads (phazite-update.json: src/services/Updates.h), which names the installer by its address in the release
# v<VERSION> on GitHub, with its size and its SHA-256.
#   cmake -DINSTALLER=<installer.exe> -DVERSION=<version> -DSOURCE_URL=<repository> -P cmake/WriteUpdateFile.cmake

foreach(_variable INSTALLER VERSION SOURCE_URL)
    if(NOT DEFINED ${_variable})
        message(FATAL_ERROR "WriteUpdateFile.cmake needs -D${_variable}=...")
    endif()
endforeach()

if(NOT EXISTS "${INSTALLER}")
    message(FATAL_ERROR "There is no installer at ${INSTALLER}.")
endif()

file(SHA256 "${INSTALLER}" _sha256)
file(SIZE "${INSTALLER}" _size)
get_filename_component(_name "${INSTALLER}" NAME)
get_filename_component(_folder "${INSTALLER}" DIRECTORY)
set(_release "${SOURCE_URL}/releases/download/v${VERSION}")

file(WRITE "${INSTALLER}.sha256" "${_sha256}  ${_name}\n")
file(WRITE "${_folder}/phazite-update.json"
"{
    \"app\": \"Phazite\",
    \"version\": \"${VERSION}\",
    \"notes\": \"${SOURCE_URL}/releases/tag/v${VERSION}\",
    \"windows\": {
        \"installer\": \"${_release}/${_name}\",
        \"size\": ${_size},
        \"sha256\": \"${_sha256}\"
    }
}
")

message(STATUS "Phazite's installer: ${INSTALLER}, ${_size} bytes, SHA-256 ${_sha256}")
