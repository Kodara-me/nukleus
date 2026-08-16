if(NOT BUILD_TESTING)
    return()
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Network Test)

add_executable(kira-service-test
    ServiceTest.cpp
    ../src/GenerationProgress.h
    ../src/ModelCatalog.cpp
    ../src/ModelCatalog.h
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
            ${CMAKE_SOURCE_DIR} $<TARGET_FILE:kira-service>)
