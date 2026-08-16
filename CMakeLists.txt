cmake_minimum_required(VERSION 3.21)

project(KiraService VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Network WebSockets Widgets)

include(CTest)

add_subdirectory(src)
add_subdirectory(tests)
