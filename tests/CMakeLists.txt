if(NOT BUILD_TESTING)
    return()
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Network Test WebSockets)

add_executable(kira-service-test
    ../src/daemon/resources.qrc
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
set_target_properties(kira-service-test PROPERTIES AUTORCC ON)
target_include_directories(kira-service-test PRIVATE
    ../src/daemon
)
target_link_libraries(kira-service-test PRIVATE KiraService::SDK Qt6::Network Qt6::Test Qt6::WebSockets)
add_test(NAME kira-service-test COMMAND kira-service-test)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME kira-service-face-parsing-test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/FaceParsingTest.py)
    set_tests_properties(kira-service-face-parsing-test PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP:")
endif()

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
add_test(NAME kira-service-manager-unavailable COMMAND ${CMAKE_COMMAND}
    -DMANAGER=$<TARGET_FILE:kira-service-manager> -P ${CMAKE_CURRENT_SOURCE_DIR}/ManagerUnavailableTest.cmake)
set_tests_properties(kira-service-manager-unavailable PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: local service is running")

add_executable(kira-service-manager-ui-test
    ManagerUiTest.cpp
    ../src/manager/resources.qrc
    ../src/manager/ModelOptionsWidget.h
    ../src/manager/ModelOptionsWidget.cpp
    ../src/manager/AddModelDialog.h
    ../src/manager/AddModelDialog.cpp
    ../src/manager/ManageModelsWindow.cpp
    ../src/manager/ManageModelsWindow.h
    ../src/manager/ModelStylesDialog.cpp
    ../src/manager/ModelStylesDialog.h
    ../src/manager/ServiceConnectionDialog.cpp
    ../src/manager/ServiceConnectionDialog.h
    ../src/manager/ManagerServiceClient.cpp
    ../src/manager/ManagerServiceClient.h
)
set_target_properties(kira-service-manager-ui-test PROPERTIES AUTORCC ON)
target_link_libraries(kira-service-manager-ui-test PRIVATE KiraService::Protocol KiraUi::Widgets Qt6::Widgets Qt6::Test Qt6::WebSockets)
add_test(NAME kira-service-manager-ui-test COMMAND kira-service-manager-ui-test)
set_tests_properties(kira-service-manager-ui-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME kira-service-updater-test
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/KiraServiceUpdaterTest.sh ${CMAKE_SOURCE_DIR})
endif()
