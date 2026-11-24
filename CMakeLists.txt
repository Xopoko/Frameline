cmake_minimum_required(VERSION 3.21.1)

if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.4 CACHE STRING "Minimum macOS version")
endif()

project(Frameline VERSION 0.4.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_compile_definitions(FRAMELINE_VERSION="${PROJECT_VERSION}")

include(CTest)
include(GNUInstallDirs)
find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network Concurrent)

if(MSVC)
    add_compile_options(/utf-8)
endif()

add_library(frameline_core STATIC
    src/annotations.cpp
    src/annotations.h
    src/application.cpp
    src/application.h
    src/capture.cpp
    src/capture.h
    src/controls.cpp
    src/controls.h
    src/editor.cpp
    src/editor.h
    src/libraryclient.cpp
    src/libraryclient.h
    src/libraryprotocol.h
    src/libraryserver.cpp
    src/libraryserver.h
    src/librarystorage.cpp
    src/librarystorage.h
    src/librarywindow.cpp
    src/librarywindow.h
    src/pngexport.cpp
    src/pngexport.h
    src/regionselector.cpp
    src/regionselector.h
    src/settings.cpp
    src/settings.h
    src/settingswindow.cpp
    src/settingswindow.h
    src/singleinstance.cpp
    src/singleinstance.h
    src/tailscale.cpp
    src/tailscale.h
    src/tailrelay.cpp
    src/tailrelay.h
    src/platform/autostart.cpp
    src/platform/autostart.h
    src/shortcuts.cpp
    src/shortcuts.h
    src/theme.cpp
    src/theme.h
)
target_include_directories(frameline_core PUBLIC src)
target_link_libraries(frameline_core PUBLIC Qt6::Widgets PRIVATE Qt6::Network Qt6::Concurrent)

if(WIN32)
    target_sources(frameline_core PRIVATE src/platform/windows_shortcuts.cpp src/platform/windows_shortcuts.h)
    target_link_libraries(frameline_core PRIVATE user32 shell32 gdi32 dwmapi ws2_32 iphlpapi)
elseif(APPLE)
    enable_language(OBJCXX)
    set_target_properties(frameline_core PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
    target_sources(frameline_core PRIVATE src/platform/macos_capture.mm)
    set_source_files_properties(src/platform/macos_capture.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    find_library(CARBON_FRAMEWORK Carbon REQUIRED)
    find_library(APPLICATION_SERVICES_FRAMEWORK ApplicationServices REQUIRED)
    find_library(SCREEN_CAPTURE_KIT_FRAMEWORK ScreenCaptureKit REQUIRED)
    find_library(CORE_GRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    target_link_libraries(frameline_core PRIVATE
        ${CARBON_FRAMEWORK} ${APPLICATION_SERVICES_FRAMEWORK} ${SCREEN_CAPTURE_KIT_FRAMEWORK}
        ${CORE_GRAPHICS_FRAMEWORK} objc)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 6.4 REQUIRED COMPONENTS DBus)
    find_package(X11 REQUIRED COMPONENTS xcb)
    target_sources(frameline_core PRIVATE src/platform/portal_request.cpp src/platform/portal_request.h)
    target_link_libraries(frameline_core PRIVATE Qt6::DBus X11::xcb)
else()
    message(FATAL_ERROR "Frameline supports Windows, macOS and Linux.")
endif()

qt_add_executable(frameline MACOSX_BUNDLE src/main.cpp resources/resources.qrc)
target_link_libraries(frameline PRIVATE frameline_core)
if(APPLE)
    set_target_properties(frameline PROPERTIES
        OUTPUT_NAME Frameline
        MACOSX_BUNDLE_BUNDLE_NAME Frameline
        MACOSX_BUNDLE_GUI_IDENTIFIER io.github.xopoko.Frameline
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_ICON_FILE Frameline.icns)
    target_sources(frameline PRIVATE resources/Frameline.icns)
    set_source_files_properties(resources/Frameline.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

if(WIN32)
    qt_add_executable(FramelineGUI WIN32 src/main.cpp resources/resources.qrc resources/windows.rc)
    target_link_libraries(FramelineGUI PRIVATE frameline_core)
    install(TARGETS FramelineGUI RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(TARGETS frameline BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WIN32 OR APPLE)
    set(FRAMELINE_QT_NOTICES_DIR "" CACHE PATH "Directory containing notices for the bundled Qt SDK")
    if(WIN32)
        set(notice_destination .)
    else()
        set(notice_destination Frameline.app/Contents/Resources)
    endif()
    install(FILES LICENSE DESTINATION ${notice_destination})
    if(FRAMELINE_QT_NOTICES_DIR)
        install(DIRECTORY "${FRAMELINE_QT_NOTICES_DIR}/" DESTINATION ${notice_destination}/Qt-notices)
        install(FILES docs/release.md DESTINATION ${notice_destination} RENAME RELEASE.md)
    endif()
    if(WIN32)
        set(deploy_target FramelineGUI)
        if(Qt6_VERSION VERSION_GREATER_EQUAL 6.8)
            set(deploy_options
                --skip-plugin-types generic,networkinformation,tls
                --exclude-plugins qgif,qico
                --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw
                --no-compiler-runtime --no-translations)
        endif()
    else()
        set(deploy_target frameline)
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
    install(FILES resources/io.github.xopoko.Frameline.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES scripts/configure-gnome-shortcut.py DESTINATION ${CMAKE_INSTALL_DATADIR}/frameline)
    install(FILES resources/Frameline.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
        RENAME io.github.xopoko.Frameline.png)
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/frameline RENAME copyright)
    install(FILES docs/release.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/frameline RENAME RELEASE.md)
    set(CPACK_GENERATOR DEB)
    set(CPACK_PACKAGE_NAME frameline)
    set(CPACK_PACKAGE_CONTACT "Frameline")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_COMPRESSION_TYPE xz)
    set(CPACK_DEBIAN_PACKAGE_SECTION graphics)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins, qt6-wayland, xdg-desktop-portal, wl-clipboard")
    # Frameline was released as ScreenFrame. Replace that package and keep its command working for
    # scripts and desktop shortcuts that still call it.
    set(CPACK_DEBIAN_PACKAGE_REPLACES screenframe)
    set(CPACK_DEBIAN_PACKAGE_CONFLICTS screenframe)
    set(CPACK_DEBIAN_PACKAGE_PROVIDES screenframe)
    install(CODE "file(CREATE_LINK frameline \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/screenframe\" SYMBOLIC)")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Desktop screenshot utility and annotation editor")
    include(CPack)
endif()

if(BUILD_TESTING)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
    add_executable(frameline_tests tests/frameline_tests.cpp)
    target_link_libraries(frameline_tests PRIVATE frameline_core Qt6::Test)
    # Renders every window to PNG for visual review: frameline_shots <output-directory>
    add_executable(frameline_shots tools/shots.cpp)
    target_link_libraries(frameline_shots PRIVATE frameline_core Qt6::Test)
    add_test(NAME frameline_components COMMAND frameline_tests)
    set_tests_properties(frameline_components PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    foreach(component autostart librarysettings libraryserver librarystorage singleinstance tailscale tailrelay settingswindow)
        add_executable(frameline_${component}_tests tests/${component}_tests.cpp)
        target_link_libraries(frameline_${component}_tests PRIVATE frameline_core Qt6::Test Qt6::Network)
        add_test(NAME frameline_${component} COMMAND frameline_${component}_tests)
        set_tests_properties(frameline_${component} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
    add_test(NAME frameline_cli_help COMMAND frameline --help)
    add_test(NAME frameline_cli_rejects_invalid_options COMMAND frameline capture full --no-preview)
    set_tests_properties(frameline_cli_rejects_invalid_options PROPERTIES WILL_FAIL TRUE)
endif()
