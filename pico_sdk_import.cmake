# This is a copy of <PICO_SDK_PATH>/external/pico_sdk_import.cmake

if (NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif ()

if (NOT PICO_SDK_PATH)
    # Check if we are a submodule
    if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/pico-sdk/pico_sdk_init.cmake")
        set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/pico-sdk")
        message("Using git submodule pico-sdk at ${PICO_SDK_PATH}")
    else()
        message(FATAL_ERROR "SDK location was not specified. Please set PICO_SDK_PATH or PICO_SDK_FETCH_FROM_GIT to on to fetch from git.")
    endif()
endif ()

get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" REALPATH)

set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH "Path to the Pico SDK" FORCE)

if (NOT EXISTS "${PICO_SDK_PATH}/pico_sdk_init.cmake")
    message(FATAL_ERROR "Directory '${PICO_SDK_PATH}' does not appear to contain the Pico SDK")
endif ()

set(PICO_SDK_IMPORT_INCLUDED 1)

include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
