# unix specific compile definitions
# put anything here that applies to both linux and macos

list(APPEND TUNASTREAM_EXTERNAL_LIBRARIES
        ${CURL_LIBRARIES})

# add install prefix to assets path if not already there
if(NOT TUNASTREAM_ASSETS_DIR MATCHES "^${CMAKE_INSTALL_PREFIX}")
    set(TUNASTREAM_ASSETS_DIR "${CMAKE_INSTALL_PREFIX}/${TUNASTREAM_ASSETS_DIR}")
endif()
