cmake_minimum_required(VERSION 3.22)
project(pcbu_bthelper)

# macOS only: IOBluetooth does not work for root processes outside the user's session (pcbu_auth),
# so this launch agent opens the Bluetooth connections for it. Linux and Windows connect directly.
if(NOT APPLE)
    message(FATAL_ERROR "pcbu_bthelper is only needed on macOS.")
endif()

set(CMAKE_CXX_STANDARD 23)

add_compile_definitions(APPLE)

add_executable(pcbu_bthelper
        src/main.Mac.cpp
)
target_link_libraries(pcbu_bthelper PRIVATE
        pcbu_common
)
# Embedded Info.plist: gives the helper its own identifier and the Bluetooth usage description
target_link_options(pcbu_bthelper PRIVATE "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/Info.plist")
set_property(TARGET pcbu_bthelper APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist)
