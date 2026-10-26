cmake_minimum_required(VERSION 3.24)

project(Nukleus VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Network WebSockets Widgets)

include(FetchContent)
FetchContent_Declare(Ardoise
    GIT_REPOSITORY https://github.com/jstmichel/ardoise.git
    GIT_TAG v0.1.0
    FIND_PACKAGE_ARGS 0.1 CONFIG)
FetchContent_MakeAvailable(Ardoise)

include(CTest)

add_subdirectory(src)
add_subdirectory(tests)
