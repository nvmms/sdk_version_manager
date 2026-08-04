cmake_minimum_required(VERSION 3.16)

project(sdk_version_manager VERSION 0.1 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick Network Concurrent)

qt_standard_project_setup(REQUIRES 6.10)

# Content of downloaded design from Figma to Qt or similar
if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/importedcontent/CMakeLists.txt")
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/importedcontent)
endif()

qt_add_executable(appsdk_version_manager
    main.cpp
    src/eventbus.cpp
    src/eventbus.h
    src/networkproxyconfig.cpp
    src/networkproxyconfig.h
    src/providercontroller.cpp
    src/providercontroller.h
)

qt_add_qml_module(appsdk_version_manager
    URI sdk_version_manager
    QML_FILES
        Main.qml
        ProviderCatalog.js
)

qt_add_executable(svm
    cli/main.cpp
    src/eventbus.cpp
    src/eventbus.h
    src/networkproxyconfig.cpp
    src/networkproxyconfig.h
    src/providercontroller.cpp
    src/providercontroller.h
)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
set_target_properties(appsdk_version_manager PROPERTIES
#    MACOSX_BUNDLE_GUI_IDENTIFIER com.example.appsdk_version_manager
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(appsdk_version_manager
    PRIVATE Qt6::Quick Qt6::Network Qt6::Concurrent
)

target_link_libraries(svm
    PRIVATE Qt6::Core Qt6::Network Qt6::Concurrent
)

if (WIN32)
    target_link_libraries(appsdk_version_manager PRIVATE wintrust)
    target_link_libraries(svm PRIVATE wintrust)
endif()

install(TARGETS appsdk_version_manager svm
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

qt_generate_deploy_qml_app_script(
    TARGET appsdk_version_manager
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
