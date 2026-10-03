cmake_minimum_required(VERSION 3.21)

project(KiraService VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Network WebSockets Widgets)

set(KIRA_UI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../kira-ui-framework" CACHE PATH "Path to a Kira UI Framework checkout")
if(EXISTS "${KIRA_UI_SOURCE_DIR}/CMakeLists.txt" AND EXISTS "${KIRA_UI_SOURCE_DIR}/include/KiraUi/Version.h")
    add_subdirectory("${KIRA_UI_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/kira-ui-framework" EXCLUDE_FROM_ALL)
else()
    find_package(KiraUi 0.2 CONFIG REQUIRED)
endif()

include(CTest)

add_subdirectory(src)
add_subdirectory(tests)
