include(GNUInstallDirs)
add_subdirectory(sdk)

set(SERVICE_DAEMON_SOURCES
    daemon/ChildProcessJob.cpp
    daemon/ChildProcessJob.h
    daemon/FfmpegRunner.cpp
    daemon/FfmpegRunner.h
    daemon/GenerationRequest.h
    daemon/HardLink.cpp
    daemon/HardLink.h
    daemon/NukleusDaemon.cpp
    daemon/NukleusDaemon.h
    daemon/LlamaServerRunner.cpp
    daemon/LlamaServerRunner.h
    daemon/ModelCatalog.cpp
    daemon/ModelCatalog.h
    daemon/SdCliProgressParser.cpp
    daemon/SdCliProgressParser.h
    daemon/SdCliRunner.cpp
    daemon/SdCliRunner.h
    daemon/ServiceJobResult.h
    daemon/ServicePaths.cpp
    daemon/ServicePaths.h
)

add_executable(nukleusd
    daemon/nukleusd-main.cpp
    ${SERVICE_DAEMON_SOURCES}
)
set_target_properties(nukleusd PROPERTIES AUTORCC ON)
target_include_directories(nukleusd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/daemon
)
target_link_libraries(nukleusd PRIVATE Nukleus::Protocol Qt6::Network Qt6::WebSockets)

add_executable(nukleus-manager
    manager/resources.qrc
    manager/nukleus-manager-main.cpp
    manager/ManagerCli.h
    manager/ManagerCli.cpp
    manager/ModelOptionsWidget.h
    manager/ModelOptionsWidget.cpp
    manager/AddModelDialog.h
    manager/AddModelDialog.cpp
    manager/ManageModelsWindow.cpp
    manager/ManageModelsWindow.h
    manager/ModelRoutingDialog.cpp
    manager/ModelRoutingDialog.h
    manager/ServiceConnectionDialog.cpp
    manager/ServiceConnectionDialog.h
    manager/ManagerServiceClient.cpp
    manager/ManagerServiceClient.h
    manager/ManagerSettings.cpp
    manager/ManagerSettings.h
)
set_target_properties(nukleus-manager PROPERTIES AUTORCC ON)
target_include_directories(nukleus-manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/manager
)
target_link_libraries(nukleus-manager PRIVATE Nukleus::Protocol Ardoise::Widgets Qt6::Network Qt6::WebSockets Qt6::Widgets)

# The web service's code, shared by nukleus-web and its test.
add_library(nukleus-web-core OBJECT
    web/resources.qrc
    web/BackendLink.cpp
    web/BackendLink.h
    web/HostRules.cpp
    web/HostRules.h
    web/HttpServer.cpp
    web/HttpServer.h
    web/ImageUpload.cpp
    web/ImageUpload.h
    web/JobStore.cpp
    web/JobStore.h
    web/Multipart.cpp
    web/Multipart.h
    web/WebConfig.cpp
    web/WebConfig.h
    web/WebService.cpp
    web/WebService.h
)
set_target_properties(nukleus-web-core PROPERTIES AUTORCC ON)
target_include_directories(nukleus-web-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/web)
target_link_libraries(nukleus-web-core PUBLIC Nukleus::SDK Qt6::Gui Qt6::Network Qt6::WebSockets
    $<$<PLATFORM_ID:Windows>:ws2_32>)

add_executable(nukleus-web web/nukleus-web-main.cpp)
target_link_libraries(nukleus-web PRIVATE nukleus-web-core)

install(TARGETS nukleusd nukleus-web
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/nukleus
)
install(TARGETS nukleus-manager
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

foreach(target nukleusd nukleus-manager nukleus-web nukleus-web-core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    endif()
endforeach()
