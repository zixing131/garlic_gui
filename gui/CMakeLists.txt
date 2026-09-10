find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network Concurrent)
set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

qt_add_library(garlic-gui-core STATIC
    mainwindow.cpp mainwindow.h backend.cpp backend.h codeeditor.cpp codeeditor.h
    settings.cpp settings.h project.cpp project.h classview.cpp classview.h
    referencesdialog.cpp referencesdialog.h projectbrowser.cpp resources.cpp resources.h ${CMAKE_CURRENT_SOURCE_DIR}/../src/libs/zip/zip.c
    nodeicons.cpp nodeicons.h preferences.cpp mcpserver.cpp mcpserver.h search.cpp search.h searchdialog.cpp searchdialog.h)
qt_add_resources(NODE_ICON_RESOURCES icons/jadx/jadx_icons.qrc)
target_sources(garlic-gui-core PRIVATE ${NODE_ICON_RESOURCES})
set_property(TARGET garlic-gui-core PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(garlic-gui-core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/libs/zip")
target_link_libraries(garlic-gui-core PUBLIC Qt6::Widgets Qt6::Network Qt6::Concurrent)
qt_add_executable(garlic-gui MACOSX_BUNDLE WIN32 main.cpp)
set_property(TARGET garlic-gui PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(garlic-gui PRIVATE garlic-gui-core)
set_target_properties(garlic-gui PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Garlic"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.garlic.gui"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.4.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.4.0")
# Refresh the adjacent engine even when only C sources have changed.
add_custom_target(garlic-gui-engine
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:garlic-gui>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:garlic> $<TARGET_FILE_DIR:garlic-gui>/garlic${CMAKE_EXECUTABLE_SUFFIX}
    DEPENDS garlic)
add_dependencies(garlic-gui garlic-gui-engine)

option(GARLIC_GUI_TESTS "Build GUI integration tests" OFF)
if(GARLIC_GUI_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    enable_testing()
    qt_add_executable(garlic-gui-fake-engine tests/fake_engine.cpp)
    set_property(TARGET garlic-gui-fake-engine PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(garlic-gui-fake-engine PRIVATE Qt6::Core)
    qt_add_executable(garlic-gui-tests tests/backend_test.cpp)
    set_property(TARGET garlic-gui-tests PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(garlic-gui-tests PRIVATE garlic-gui-core Qt6::Test)
    qt_add_executable(garlic-gui-window-tests tests/window_test.cpp)
    set_property(TARGET garlic-gui-window-tests PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(garlic-gui-window-tests PRIVATE garlic-gui-core Qt6::Test)
    qt_add_executable(garlic-gui-project-tests tests/project_test.cpp)
    set_property(TARGET garlic-gui-project-tests PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(garlic-gui-project-tests PRIVATE garlic-gui-core Qt6::Test)
    qt_add_executable(garlic-gui-interaction-tests tests/interaction_test.cpp)
    set_property(TARGET garlic-gui-interaction-tests PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(garlic-gui-interaction-tests PRIVATE garlic-gui-core Qt6::Test)
    add_test(NAME garlic-gui-interaction COMMAND garlic-gui-interaction-tests)
    set_tests_properties(garlic-gui-interaction PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;GARLIC_TEST_ENGINE=$<TARGET_FILE:garlic>;GARLIC_TEST_FIXTURES=${CMAKE_BINARY_DIR}/fixtures")
    add_test(NAME garlic-gui-project COMMAND garlic-gui-project-tests)
    set_tests_properties(garlic-gui-project PROPERTIES ENVIRONMENT "GARLIC_TEST_ENGINE=$<TARGET_FILE:garlic>;GARLIC_TEST_FIXTURES=${CMAKE_BINARY_DIR}/fixtures")
    qt_add_executable(garlic-gui-mcp-tests tests/mcp_test.cpp)
    set_property(TARGET garlic-gui-mcp-tests PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(garlic-gui-mcp-tests PRIVATE garlic-gui-core Qt6::Test)
    add_test(NAME garlic-gui-mcp COMMAND garlic-gui-mcp-tests)
    set_tests_properties(garlic-gui-mcp PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;GARLIC_TEST_ENGINE=$<TARGET_FILE:garlic>;GARLIC_TEST_FIXTURES=${CMAKE_BINARY_DIR}/fixtures;GARLIC_TEST_GUI=$<TARGET_FILE:garlic-gui>")
    # Preserve QtTest stdout/stderr on Windows so CTest reports failing assertions.
    set_target_properties(garlic-gui-tests garlic-gui-window-tests garlic-gui-project-tests
        garlic-gui-interaction-tests garlic-gui-mcp-tests garlic-gui-fake-engine
        PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME garlic-gui-window COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_qttest.py $<TARGET_FILE:garlic-gui-window-tests>)
    set_tests_properties(garlic-gui-window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;GARLIC_TEST_ENGINE=$<TARGET_FILE:garlic>;GARLIC_TEST_FIXTURES=${CMAKE_BINARY_DIR}/fixtures")
    add_test(NAME garlic-gui-backend COMMAND garlic-gui-tests)
    add_test(NAME garlic-gui-cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.py $<TARGET_FILE:garlic> ${CMAKE_BINARY_DIR}/fixtures)
    set_tests_properties(garlic-gui-backend PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;GARLIC_TEST_ENGINE=$<TARGET_FILE:garlic>;GARLIC_TEST_FIXTURES=${CMAKE_BINARY_DIR}/fixtures;GARLIC_TEST_FAKE_ENGINE=$<TARGET_FILE:garlic-gui-fake-engine>")
endif()
