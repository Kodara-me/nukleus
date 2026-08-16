if(NOT BUILD_TESTING)
    return()
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Network Test WebSockets)

add_executable(kira-service-test
    ServiceTest.cpp
    ../src/GenerationRequest.h
    ../src/GenerationProgress.h
    ../src/KiraService.cpp
    ../src/KiraService.h
    ../src/ModelCatalog.cpp
    ../src/ModelCatalog.h
    ../src/SdCliRunner.cpp
    ../src/SdCliRunner.h
    ../src/SdCliProgressParser.cpp
    ../src/SdCliProgressParser.h
    ../src/ServiceJobResult.h
    ../src/ServicePaths.cpp
    ../src/ServicePaths.h
    ../src/ServiceProtocol.cpp
    ../src/ServiceProtocol.h
)
target_include_directories(kira-service-test PRIVATE ../src)
target_link_libraries(kira-service-test PRIVATE Qt6::Network Qt6::Test Qt6::WebSockets)
add_test(NAME kira-service-test COMMAND kira-service-test)

add_test(NAME kira-service-installer-test
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/KiraServiceInstallerTest.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:kira-service> $<TARGET_FILE:kira-service-manager>)

add_test(NAME kira-service-manager-help COMMAND $<TARGET_FILE:kira-service-manager> --help)
add_test(NAME kira-service-manager-unavailable COMMAND $<TARGET_FILE:kira-service-manager> list)
set_tests_properties(kira-service-manager-unavailable PROPERTIES WILL_FAIL TRUE)
