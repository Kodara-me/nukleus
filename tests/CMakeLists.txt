if(NOT BUILD_TESTING)
    return()
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Network Test WebSockets)

add_executable(nukleus-test
    ServiceTest.cpp
    ../src/daemon/GenerationRequest.h
    ../src/sdk/include/nukleus/GenerationProgress.h
    ../src/daemon/NukleusDaemon.cpp
    ../src/daemon/NukleusDaemon.h
    ../src/daemon/LlamaServerRunner.cpp
    ../src/daemon/LlamaServerRunner.h
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
set_target_properties(nukleus-test PROPERTIES AUTORCC ON)
target_include_directories(nukleus-test PRIVATE
    ../src/daemon
)
target_link_libraries(nukleus-test PRIVATE Nukleus::SDK Qt6::Network Qt6::Test Qt6::WebSockets)
# Stand-in for llama-server, so tests never start the real one installed on the host.
add_executable(nukleus-fake-llama-server FakeLlamaServer.cpp)
target_link_libraries(nukleus-fake-llama-server PRIVATE Qt6::Network)
add_dependencies(nukleus-test nukleus-fake-llama-server)
target_compile_definitions(nukleus-test PRIVATE
    NUKLEUS_FAKE_LLAMA_SERVER="$<TARGET_FILE:nukleus-fake-llama-server>")
add_test(NAME nukleus-test COMMAND nukleus-test)

add_executable(nukleus-sdk-test ServiceSdkTest.cpp)
target_link_libraries(nukleus-sdk-test PRIVATE Nukleus::SDK Qt6::Network Qt6::Test Qt6::WebSockets)
add_test(NAME nukleus-sdk-test COMMAND nukleus-sdk-test)

add_test(NAME nukleus-installer-test
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/NukleusInstallerTest.sh
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:nukleusd> $<TARGET_FILE:nukleus-manager>)

add_test(NAME nukleus-storage-migration-test
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/NukleusStorageMigrationTest.sh ${CMAKE_SOURCE_DIR})

add_test(NAME nukleus-cmake-install-test
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/NukleusCMakeInstallTest.sh
            ${CMAKE_BINARY_DIR})

add_test(NAME nukleus-manager-help COMMAND $<TARGET_FILE:nukleus-manager> --help)
add_test(NAME nukleus-manager-rejects-companion-category
    COMMAND $<TARGET_FILE:nukleus-manager> add https://example.test/model.gguf
            --category upscaler --companion-lora https://example.test/companion.safetensors)
set_tests_properties(nukleus-manager-rejects-companion-category PROPERTIES WILL_FAIL TRUE)
add_test(NAME nukleus-manager-unavailable COMMAND ${CMAKE_COMMAND}
    -DMANAGER=$<TARGET_FILE:nukleus-manager> -P ${CMAKE_CURRENT_SOURCE_DIR}/ManagerUnavailableTest.cmake)
set_tests_properties(nukleus-manager-unavailable PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: local service is running")

add_executable(nukleus-manager-ui-test
    ManagerUiTest.cpp
    ../src/manager/resources.qrc
    ../src/manager/ModelOptionsWidget.h
    ../src/manager/ModelOptionsWidget.cpp
    ../src/manager/AddModelDialog.h
    ../src/manager/AddModelDialog.cpp
    ../src/manager/ManageModelsWindow.cpp
    ../src/manager/ManageModelsWindow.h
    ../src/manager/ModelRoutingDialog.cpp
    ../src/manager/ModelRoutingDialog.h
    ../src/manager/ServiceConnectionDialog.cpp
    ../src/manager/ServiceConnectionDialog.h
    ../src/manager/ManagerServiceClient.cpp
    ../src/manager/ManagerServiceClient.h
    ../src/manager/ManagerSettings.cpp
    ../src/manager/ManagerSettings.h
    ../src/manager/ManagerCli.cpp
    ../src/manager/ManagerCli.h
)
set_target_properties(nukleus-manager-ui-test PROPERTIES AUTORCC ON)
target_link_libraries(nukleus-manager-ui-test PRIVATE Nukleus::Protocol Ardoise::Widgets Qt6::Widgets Qt6::Test Qt6::WebSockets)
add_test(NAME nukleus-manager-ui-test COMMAND nukleus-manager-ui-test)
set_tests_properties(nukleus-manager-ui-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME nukleus-updater-test
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/NukleusUpdaterTest.sh ${CMAKE_SOURCE_DIR})
    add_test(NAME nukleus-get-test
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/NukleusGetTest.sh ${CMAKE_SOURCE_DIR}/scripts/get-nukleus.sh)
endif()
