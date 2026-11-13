cmake_minimum_required(VERSION 3.21.1)

if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.4 CACHE STRING "Minimum macOS version")
endif()

project(ScreenFrame VERSION 0.1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_compile_definitions(SCREENFRAME_VERSION="${PROJECT_VERSION}")

include(CTest)
include(GNUInstallDirs)
find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)

if(MSVC)
    add_compile_options(/utf-8)
endif()

add_library(screenframe_core STATIC
    src/annotations.cpp
    src/annotations.h
    src/application.cpp
    src/application.h
    src/capture.cpp
    src/capture.h
    src/editor.cpp
    src/editor.h
    src/pngexport.cpp
    src/pngexport.h
    src/regionselector.cpp
    src/regionselector.h
    src/shortcuts.cpp
    src/shortcuts.h
)
target_include_directories(screenframe_core PUBLIC src)
target_link_libraries(screenframe_core PUBLIC Qt6::Widgets)

if(WIN32)
    target_sources(screenframe_core PRIVATE src/platform/windows_shortcuts.cpp src/platform/windows_shortcuts.h)
    target_link_libraries(screenframe_core PRIVATE user32 shell32 gdi32 dwmapi)
elseif(APPLE)
    enable_language(OBJCXX)
    set_target_properties(screenframe_core PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
    target_sources(screenframe_core PRIVATE src/platform/macos_capture.mm)
    set_source_files_properties(src/platform/macos_capture.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    find_library(CARBON_FRAMEWORK Carbon REQUIRED)
    find_library(APPLICATION_SERVICES_FRAMEWORK ApplicationServices REQUIRED)
    find_library(SCREEN_CAPTURE_KIT_FRAMEWORK ScreenCaptureKit REQUIRED)
    find_library(CORE_GRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    target_link_libraries(screenframe_core PRIVATE
        ${CARBON_FRAMEWORK} ${APPLICATION_SERVICES_FRAMEWORK} ${SCREEN_CAPTURE_KIT_FRAMEWORK}
        ${CORE_GRAPHICS_FRAMEWORK} objc)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 6.4 REQUIRED COMPONENTS DBus)
    find_package(X11 REQUIRED COMPONENTS xcb)
    target_sources(screenframe_core PRIVATE src/platform/portal_request.cpp src/platform/portal_request.h)
    target_link_libraries(screenframe_core PRIVATE Qt6::DBus X11::xcb)
else()
    message(FATAL_ERROR "ScreenFrame supports Windows, macOS and Linux.")
endif()

qt_add_executable(screenframe MACOSX_BUNDLE src/main.cpp resources/resources.qrc)
target_link_libraries(screenframe PRIVATE screenframe_core)
if(APPLE)
    set_target_properties(screenframe PROPERTIES
        OUTPUT_NAME ScreenFrame
        MACOSX_BUNDLE_BUNDLE_NAME ScreenFrame
        MACOSX_BUNDLE_GUI_IDENTIFIER io.github.xopoko.ScreenFrame
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_ICON_FILE ScreenFrame.icns)
    target_sources(screenframe PRIVATE resources/ScreenFrame.icns)
    set_source_files_properties(resources/ScreenFrame.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

if(WIN32)
    qt_add_executable(ScreenFrameGUI WIN32 src/main.cpp resources/resources.qrc resources/windows.rc)
    target_link_libraries(ScreenFrameGUI PRIVATE screenframe_core)
    install(TARGETS ScreenFrameGUI RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(TARGETS screenframe BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WIN32 OR APPLE)
    if(WIN32)
        set(deploy_target ScreenFrameGUI)
        if(Qt6_VERSION VERSION_GREATER_EQUAL 6.8)
            set(deploy_options
                --skip-plugin-types generic,imageformats,networkinformation,tls
                --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw)
        endif()
    else()
        set(deploy_target screenframe)
    endif()
    if(deploy_options)
        qt_generate_deploy_app_script(TARGET ${deploy_target} OUTPUT_SCRIPT deploy_script
            DEPLOY_TOOL_OPTIONS ${deploy_options})
    else()
        qt_generate_deploy_app_script(TARGET ${deploy_target} OUTPUT_SCRIPT deploy_script)
    endif()
    install(SCRIPT ${deploy_script})
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(FILES resources/io.github.xopoko.ScreenFrame.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES scripts/configure-gnome-shortcut.py DESTINATION ${CMAKE_INSTALL_DATADIR}/screenframe)
    install(FILES resources/ScreenFrame.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
        RENAME io.github.xopoko.ScreenFrame.png)
    set(CPACK_GENERATOR DEB)
    set(CPACK_PACKAGE_NAME screenframe)
    set(CPACK_PACKAGE_CONTACT "ScreenFrame")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_COMPRESSION_TYPE xz)
    set(CPACK_DEBIAN_PACKAGE_SECTION graphics)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6widgets6 (>= 6.4.2), libqt6dbus6 (>= 6.4.2), libxcb1, qt6-qpa-plugins, qt6-wayland, xdg-desktop-portal")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Desktop screenshot utility and annotation editor")
    include(CPack)
endif()

if(BUILD_TESTING)
    add_executable(screenframe_tests tests/screenframe_tests.cpp)
    target_link_libraries(screenframe_tests PRIVATE screenframe_core)
    add_test(NAME screenframe_components COMMAND screenframe_tests)
    set_tests_properties(screenframe_components PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME screenframe_cli_help COMMAND screenframe --help)
    add_test(NAME screenframe_cli_rejects_invalid_options COMMAND screenframe capture full --no-preview)
    set_tests_properties(screenframe_cli_rejects_invalid_options PROPERTIES WILL_FAIL TRUE)
endif()
