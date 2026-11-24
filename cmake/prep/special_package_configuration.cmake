# Only Windows packages are produced; see cmake/packaging/windows.cmake.

# return if configure only is set
if(${TUNASTREAM_CONFIGURE_ONLY})
    # message
    message(STATUS "TUNASTREAM_CONFIGURE_ONLY: ON, exiting...")
    set(END_BUILD ON)
else()
    set(END_BUILD OFF)
endif()
