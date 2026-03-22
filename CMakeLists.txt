cmake_minimum_required(VERSION 3.21)

project(spike_viewer_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH
        "/opt/homebrew/opt/qt@5"
        "/usr/local/opt/qt@5"
    )
endif()

find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui Widgets Concurrent Xml)

add_executable(spike_viewer_native
    main.cpp
    main_window.cpp
    main_window.h
    recording.cpp
    recording.h
    widgets.cpp
    widgets.h
)

target_link_libraries(spike_viewer_native
    PRIVATE
        Qt5::Core
        Qt5::Gui
        Qt5::Widgets
        Qt5::Concurrent
        Qt5::Xml
)

if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
    target_compile_options(spike_viewer_native PRIVATE -Wall -Wextra -Wpedantic)
endif()
