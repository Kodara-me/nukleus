if(NOT BUILD_TESTING)
    return()
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Network Test WebSockets)

add_executable(kira-service-test
    ServiceTest.cpp
    ../src/daemon/GenerationRequest.h
    ../src/sdk/include/kira/service/GenerationProgress.h
    ../src/daemon/KiraService.cpp
    ../src/daemon/KiraService.h
    ../src/daemon/ModelCatalog.cpp
    ../src/daemon/ModelCatalog.h
    ../src/daemon/SdCliRunner.cpp
    ../src/daemon/SdCliRunner.h
    ../src/daemon/SdCliProgressParser.cpp
    ../src/daemon/SdCliProgressParser.h
    ../src/daemon/ServiceJobResult.h
    ../src/daemon/ServicePaths.cpp
    ../src/daemon/ServicePaths.h
)
target_include_directories(kira-service-test PRIVATE
    ../src/daemon
)
target_link_libraries(kira-service-test PRIVATE KiraService::Protocol Qt6::Network Qt6::Test Qt6::WebSockets)
add_test(NAME kira-service-test COMMAND kira-service-test)

add_executable(kira-service-sdk-test ServiceSdkTest.cpp)
target_link_libraries(kira-service-sdk-test PRIVATE KiraService::SDK Qt6::Network Qt6::Test Qt6::WebSockets)
add_test(NAME kira-service-sdk-test COMMAND kira-service-sdk-test)

add_test(NAME kira-service-installer-test
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/KiraServiceInstallerTest.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:kira-service> $<TARGET_FILE:kira-service-manager>)

add_test(NAME kira-service-cmake-install-test
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/KiraServiceCMakeInstallTest.sh
            ${CMAKE_BINARY_DIR})

add_test(NAME kira-service-manager-help COMMAND $<TARGET_FILE:kira-service-manager> --help)
add_test(NAME kira-service-manager-rejects-companion-category
    COMMAND $<TARGET_FILE:kira-service-manager> add https://example.test/model.gguf
            --category upscaler --companion-lora https://example.test/companion.safetensors)
set_tests_properties(kira-service-manager-rejects-companion-category PROPERTIES WILL_FAIL TRUE)
add_test(NAME kira-service-manager-unavailable COMMAND $<TARGET_FILE:kira-service-manager> list)
set_tests_properties(kira-service-manager-unavailable PROPERTIES WILL_FAIL TRUE)
