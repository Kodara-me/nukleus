include(GNUInstallDirs)
add_subdirectory(sdk)

set(SERVICE_DAEMON_SOURCES
    daemon/GenerationRequest.h
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

install(TARGETS nukleusd
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/nukleus
)
install(TARGETS nukleus-manager
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

foreach(target nukleusd nukleus-manager)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    endif()
endforeach()
