find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

qt_add_executable(garlic-gui MACOSX_BUNDLE WIN32
    main.cpp mainwindow.cpp mainwindow.h
    backend.cpp backend.h codeeditor.cpp codeeditor.h)
target_link_libraries(garlic-gui PRIVATE Qt6::Widgets)
# The engine's global C include paths contain names such as string.h; keep the
# C++ GUI isolated from them.
set_property(TARGET garlic-gui PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(garlic-gui PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Garlic"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.garlic.gui"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0")
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
    qt_add_executable(garlic-gui-tests tests/backend_test.cpp backend.cpp backend.h)
    set_property(TARGET garlic-gui-tests PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(garlic-gui-tests PRIVATE Qt6::Widgets Qt6::Test)
    qt_add_executable(garlic-gui-window-tests tests/window_test.cpp
        mainwindow.cpp mainwindow.h backend.cpp backend.h codeeditor.cpp codeeditor.h)
    set_property(TARGET garlic-gui-window-tests PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(garlic-gui-window-tests PRIVATE Qt6::Widgets Qt6::Test)
    add_test(NAME garlic-gui-window COMMAND garlic-gui-window-tests)
    set_tests_properties(garlic-gui-window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;GARLIC_TEST_ENGINE=$<TARGET_FILE:garlic>;GARLIC_TEST_FIXTURES=${CMAKE_BINARY_DIR}/fixtures")
    add_test(NAME garlic-gui-backend COMMAND garlic-gui-tests)
    add_test(NAME garlic-gui-cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.py $<TARGET_FILE:garlic> ${CMAKE_BINARY_DIR}/fixtures)
    set_tests_properties(garlic-gui-backend PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;GARLIC_TEST_ENGINE=$<TARGET_FILE:garlic>;GARLIC_TEST_FIXTURES=${CMAKE_BINARY_DIR}/fixtures;GARLIC_TEST_FAKE_ENGINE=$<TARGET_FILE:garlic-gui-fake-engine>")
endif()
