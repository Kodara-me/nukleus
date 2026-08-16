set(SERVICE_RUNTIME_SOURCES
    GenerationProgress.h
    GenerationRequest.h
    KiraService.cpp
    KiraService.h
    ModelCatalog.cpp
    ModelCatalog.h
    SdCliProgressParser.cpp
    SdCliProgressParser.h
    SdCliRunner.cpp
    SdCliRunner.h
    ServiceJobResult.h
    ServicePaths.cpp
    ServicePaths.h
    ServiceProtocol.cpp
    ServiceProtocol.h
)

add_executable(kira-service
    kira-service-main.cpp
    ${SERVICE_RUNTIME_SOURCES}
)
target_link_libraries(kira-service PRIVATE Qt6::Network Qt6::WebSockets)

add_executable(kira-service-manager
    kira-service-manager-main.cpp
    ManageModelsWindow.cpp
    ManageModelsWindow.h
    ManagerServiceClient.cpp
    ManagerServiceClient.h
    GenerationProgress.h
    ServiceProtocol.cpp
    ServiceProtocol.h
)
target_link_libraries(kira-service-manager PRIVATE Qt6::Network Qt6::WebSockets Qt6::Widgets)

foreach(target kira-service kira-service-manager)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    endif()
endforeach()
