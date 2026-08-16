include(GNUInstallDirs)

set(SERVICE_PROTOCOL_SOURCES
    protocol/GenerationProgress.h
    protocol/ServiceProtocol.cpp
    protocol/ServiceProtocol.h
)

set(SERVICE_DAEMON_SOURCES
    daemon/GenerationRequest.h
    daemon/KiraService.cpp
    daemon/KiraService.h
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

add_executable(kira-service
    daemon/kira-service-main.cpp
    ${SERVICE_PROTOCOL_SOURCES}
    ${SERVICE_DAEMON_SOURCES}
)
target_include_directories(kira-service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/daemon
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol
)
target_link_libraries(kira-service PRIVATE Qt6::Network Qt6::WebSockets)

add_executable(kira-service-manager
    manager/kira-service-manager-main.cpp
    manager/ManageModelsWindow.cpp
    manager/ManageModelsWindow.h
    manager/ManagerServiceClient.cpp
    manager/ManagerServiceClient.h
    ${SERVICE_PROTOCOL_SOURCES}
)
target_include_directories(kira-service-manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/manager
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol
)
target_link_libraries(kira-service-manager PRIVATE Qt6::Network Qt6::WebSockets Qt6::Widgets)

install(TARGETS kira-service
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/kira-service
)
install(TARGETS kira-service-manager
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

foreach(target kira-service kira-service-manager)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    endif()
endforeach()
