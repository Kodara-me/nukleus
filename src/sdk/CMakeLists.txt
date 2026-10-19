include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(NUKLEUS_SDK_INSTALL "Install and export the Nukleus SDK" ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Network WebSockets)
find_package(LibArchive REQUIRED)

add_library(NukleusProtocol STATIC
    include/nukleus/ModelRouting.h
    src/ModelRouting.cpp
    include/nukleus/PromptStyle.h
    src/PromptStyle.cpp
    include/nukleus/GenerationProgress.h
    include/nukleus/ServiceProtocol.h
    include/nukleus/ModelOptions.h
    src/ModelOptions.cpp
    src/ServiceProtocol.cpp
)
add_library(Nukleus::Protocol ALIAS NukleusProtocol)
set_target_properties(NukleusProtocol PROPERTIES EXPORT_NAME Protocol)
target_include_directories(NukleusProtocol PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(NukleusProtocol PUBLIC Qt6::Core)

add_library(NukleusSdk STATIC
    include/nukleus/Identity.h
    src/Identity.cpp
    include/nukleus/ImageTypes.h
    include/nukleus/ServiceClient.h
    src/ServiceClient.cpp
)
add_library(Nukleus::SDK ALIAS NukleusSdk)
set_target_properties(NukleusSdk PROPERTIES EXPORT_NAME SDK AUTOMOC ON)
target_include_directories(NukleusSdk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(NukleusSdk PUBLIC Nukleus::Protocol Qt6::Network Qt6::WebSockets LibArchive::LibArchive)

foreach(target NukleusProtocol NukleusSdk)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    endif()
endforeach()

# Routing labels are UTF-8; propagate the source encoding to Windows consumers.
target_compile_options(NukleusProtocol PUBLIC "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

if(NUKLEUS_SDK_INSTALL)
    install(TARGETS NukleusProtocol NukleusSdk
        EXPORT NukleusSdkTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/NukleusSdkConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/NukleusSdkConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NukleusSdk
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/NukleusSdkConfigVersion.cmake
        COMPATIBILITY SameMajorVersion
    )
    install(EXPORT NukleusSdkTargets
        FILE NukleusSdkTargets.cmake
        NAMESPACE Nukleus::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NukleusSdk
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/NukleusSdkConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/NukleusSdkConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NukleusSdk
    )
endif()
