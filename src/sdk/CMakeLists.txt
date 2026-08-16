include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(KIRA_SERVICE_SDK_INSTALL "Install and export the Kira Service SDK" ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Network WebSockets)

add_library(KiraServiceProtocol STATIC
    include/kira/service/GenerationProgress.h
    include/kira/service/ServiceProtocol.h
    src/ServiceProtocol.cpp
)
add_library(KiraService::Protocol ALIAS KiraServiceProtocol)
set_target_properties(KiraServiceProtocol PROPERTIES EXPORT_NAME Protocol)
target_include_directories(KiraServiceProtocol PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(KiraServiceProtocol PUBLIC Qt6::Core)

add_library(KiraServiceSdk STATIC
    include/kira/service/ImageTypes.h
    include/kira/service/ServiceClient.h
    src/ServiceClient.cpp
)
add_library(KiraService::SDK ALIAS KiraServiceSdk)
set_target_properties(KiraServiceSdk PROPERTIES EXPORT_NAME SDK AUTOMOC ON)
target_include_directories(KiraServiceSdk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(KiraServiceSdk PUBLIC KiraService::Protocol Qt6::Network Qt6::WebSockets)

foreach(target KiraServiceProtocol KiraServiceSdk)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    endif()
endforeach()

if(KIRA_SERVICE_SDK_INSTALL)
    install(TARGETS KiraServiceProtocol KiraServiceSdk
        EXPORT KiraServiceSdkTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/KiraServiceSdkConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/KiraServiceSdkConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KiraServiceSdk
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/KiraServiceSdkConfigVersion.cmake
        COMPATIBILITY SameMajorVersion
    )
    install(EXPORT KiraServiceSdkTargets
        FILE KiraServiceSdkTargets.cmake
        NAMESPACE KiraService::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KiraServiceSdk
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/KiraServiceSdkConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/KiraServiceSdkConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KiraServiceSdk
    )
endif()
