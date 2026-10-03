include(GNUInstallDirs)
add_subdirectory(sdk)

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
    ${SERVICE_DAEMON_SOURCES}
)
target_include_directories(kira-service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/daemon
)
target_link_libraries(kira-service PRIVATE KiraService::Protocol Qt6::Network Qt6::WebSockets)

add_executable(kira-service-manager
    manager/resources.qrc
    manager/kira-service-manager-main.cpp
    manager/ModelOptionsWidget.h
    manager/ModelOptionsWidget.cpp
    manager/AddModelDialog.h
    manager/AddModelDialog.cpp
    manager/ManageModelsWindow.cpp
    manager/ManageModelsWindow.h
    manager/ManagerServiceClient.cpp
    manager/ManagerServiceClient.h
)
set_target_properties(kira-service-manager PROPERTIES AUTORCC ON)
target_include_directories(kira-service-manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/manager
)
target_link_libraries(kira-service-manager PRIVATE KiraService::Protocol KiraUi::Widgets Qt6::Network Qt6::WebSockets Qt6::Widgets)

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
