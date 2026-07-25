# =============================================================================
# 墨弈项目构建配置
# 配置范围：项目总体结构、Qt 模块依赖、源文件组织、资源与测试目标。
# =============================================================================
cmake_minimum_required(VERSION 3.16)

project(moyi VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        boardwidget.cpp
        boardwidget.h
        gamemodel.cpp
        gamemodel.h
        gametypes.h
        aiprovider.cpp
        aiprovider.h
        builtinai.cpp
        builtinai.h
        httpaiprovider.cpp
        httpaiprovider.h
        resources.qrc
)

if(WIN32)
    list(APPEND PROJECT_SOURCES appicon.rc)
endif()

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(moyi
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET moyi APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(moyi SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(moyi
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_link_libraries(moyi PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
)

# 每次构建都同步本地 AI 适配器，避免只修改 Python 时 EXE 旁仍是旧脚本。
add_custom_target(moyi_ai_adapter ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/ai_server.py
            $<TARGET_FILE_DIR:moyi>/ai_server.py
    COMMENT "Copying local AI adapter next to Moyi"
)
add_dependencies(moyi moyi_ai_adapter)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.moyi)
endif()
set_target_properties(moyi PROPERTIES
    OUTPUT_NAME "Moyi"
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS moyi
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES examples/ai_server.py DESTINATION ${CMAKE_INSTALL_BINDIR})

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(moyi)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(superwuziqi_tests
        tests/gamemodel_test.cpp
        gamemodel.cpp
        gamemodel.h
        gametypes.h
        aiprovider.cpp
        aiprovider.h
        builtinai.cpp
        builtinai.h
    )
    target_include_directories(superwuziqi_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(superwuziqi_tests PRIVATE Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME superwuziqi_tests COMMAND superwuziqi_tests)

    add_executable(superwuziqi_http_test
        tests/httpaiprovider_test.cpp
        aiprovider.cpp
        aiprovider.h
        httpaiprovider.cpp
        httpaiprovider.h
        gametypes.h
    )
    target_include_directories(superwuziqi_http_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(superwuziqi_http_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Network
    )
    add_test(NAME superwuziqi_http_test COMMAND superwuziqi_http_test)
endif()
