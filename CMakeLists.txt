cmake_minimum_required(VERSION 3.21.1)

if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.4 CACHE STRING "Minimum macOS version")
endif()

project(FreezeCut VERSION 0.5.1 LANGUAGES CXX)

option(FREEZECUT_MAC_APP_STORE "Build FreezeCut for the Mac App Store" OFF)
option(FREEZECUT_WINDOWS_STORE "Build FreezeCut for the Microsoft Store" OFF)
option(FREEZECUT_LINUX_SNAP "Build FreezeCut for the Snap Store" OFF)
if(FREEZECUT_MAC_APP_STORE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "FREEZECUT_MAC_APP_STORE is only supported on macOS.")
endif()
if(FREEZECUT_WINDOWS_STORE AND NOT WIN32)
    message(FATAL_ERROR "FREEZECUT_WINDOWS_STORE is only supported on Windows.")
endif()
if(FREEZECUT_LINUX_SNAP AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "FREEZECUT_LINUX_SNAP is only supported on Linux.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_compile_definitions(FREEZECUT_VERSION="${PROJECT_VERSION}")
if(FREEZECUT_LINUX_SNAP)
    set(FREEZECUT_SNAP_BUILD_NUMBER "1" CACHE STRING "Snap Store build number for source distribution")
    if(NOT FREEZECUT_SNAP_BUILD_NUMBER MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "Set FREEZECUT_SNAP_BUILD_NUMBER to a positive integer.")
    endif()
    add_compile_definitions(FREEZECUT_LINUX_SNAP
        FREEZECUT_STORE_SOURCE_URL="https://github.com/Xopoko/FreezeCut/releases/tag/v${PROJECT_VERSION}-snap-store.${FREEZECUT_SNAP_BUILD_NUMBER}")
endif()

include(CTest)
include(GNUInstallDirs)
find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network Concurrent)

if(FREEZECUT_MAC_APP_STORE)
    set(FREEZECUT_MAC_BUILD_NUMBER "" CACHE STRING "Mac App Store build number")
    if(NOT FREEZECUT_MAC_BUILD_NUMBER MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "Set FREEZECUT_MAC_BUILD_NUMBER to a positive integer.")
    endif()
    get_target_property(qt_features Qt6::Core QT_ENABLED_PUBLIC_FEATURES)
    if(NOT "appstore_compliant" IN_LIST qt_features)
        message(FATAL_ERROR "Build Qt with -feature-appstore-compliant for the Mac App Store.")
    endif()
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

add_library(freezecut_core STATIC
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
    src/platform/reducedmotion.h
    src/shortcuts.cpp
    src/shortcuts.h
    src/theme.cpp
    src/theme.h
    src/welcomewindow.cpp
    src/welcomewindow.h
)
target_include_directories(freezecut_core PUBLIC src)
target_link_libraries(freezecut_core PUBLIC Qt6::Widgets PRIVATE Qt6::Network Qt6::Concurrent)

if(WIN32)
    target_sources(freezecut_core PRIVATE
        src/platform/reducedmotion.cpp src/platform/windows_shortcuts.cpp src/platform/windows_shortcuts.h)
    target_link_libraries(freezecut_core PRIVATE user32 shell32 gdi32 dwmapi ws2_32 iphlpapi)
    if(FREEZECUT_WINDOWS_STORE)
        set(FREEZECUT_WINDOWS_STORE_BUILD_NUMBER "" CACHE STRING "Microsoft Store build number for source distribution")
        if(NOT FREEZECUT_WINDOWS_STORE_BUILD_NUMBER MATCHES "^[1-9][0-9]*$")
            message(FATAL_ERROR "Set FREEZECUT_WINDOWS_STORE_BUILD_NUMBER to a positive integer.")
        endif()
        target_sources(freezecut_core PRIVATE src/platform/windows_store_autostart.cpp)
        target_compile_definitions(freezecut_core PUBLIC FREEZECUT_WINDOWS_STORE
            FREEZECUT_STORE_SOURCE_URL="https://github.com/Xopoko/FreezeCut/releases/tag/v${PROJECT_VERSION}-microsoft-store.${FREEZECUT_WINDOWS_STORE_BUILD_NUMBER}")
        target_link_libraries(freezecut_core PRIVATE windowsapp)
    endif()
elseif(APPLE)
    enable_language(OBJCXX)
    set_target_properties(freezecut_core PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
    target_sources(freezecut_core PRIVATE src/platform/macos_capture.mm src/platform/macos_reducedmotion.mm)
    set_source_files_properties(src/platform/macos_capture.mm src/platform/macos_reducedmotion.mm
        PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(CARBON_FRAMEWORK Carbon REQUIRED)
    find_library(APPLICATION_SERVICES_FRAMEWORK ApplicationServices REQUIRED)
    find_library(SCREEN_CAPTURE_KIT_FRAMEWORK ScreenCaptureKit REQUIRED)
    find_library(CORE_GRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    target_link_libraries(freezecut_core PRIVATE
        ${CARBON_FRAMEWORK} ${APPLICATION_SERVICES_FRAMEWORK} ${SCREEN_CAPTURE_KIT_FRAMEWORK}
        ${CORE_GRAPHICS_FRAMEWORK} ${APPKIT_FRAMEWORK} objc)
    if(FREEZECUT_MAC_APP_STORE)
        find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
        find_library(SERVICE_MANAGEMENT_FRAMEWORK ServiceManagement REQUIRED)
        target_compile_definitions(freezecut_core PUBLIC FREEZECUT_MAC_APP_STORE
            FREEZECUT_STORE_SOURCE_URL="https://github.com/Xopoko/FreezeCut/releases/tag/v${PROJECT_VERSION}-mac-app-store.${FREEZECUT_MAC_BUILD_NUMBER}")
        target_sources(freezecut_core PRIVATE src/platform/macos_autostart.mm src/platform/macos_paths.mm)
        set_source_files_properties(src/platform/macos_autostart.mm src/platform/macos_paths.mm
            PROPERTIES COMPILE_OPTIONS -fobjc-arc)
        target_link_libraries(freezecut_core PRIVATE ${FOUNDATION_FRAMEWORK} ${SERVICE_MANAGEMENT_FRAMEWORK})
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 6.4 REQUIRED COMPONENTS DBus)
    find_package(X11 REQUIRED COMPONENTS xcb)
    target_sources(freezecut_core PRIVATE
        src/platform/reducedmotion.cpp src/platform/portal_request.cpp src/platform/portal_request.h)
    target_link_libraries(freezecut_core PRIVATE Qt6::DBus X11::xcb)
else()
    message(FATAL_ERROR "FreezeCut supports Windows, macOS and Linux.")
endif()

qt_add_executable(freezecut MACOSX_BUNDLE src/main.cpp resources/resources.qrc)
target_link_libraries(freezecut PRIVATE freezecut_core)
if(APPLE)
    set_target_properties(freezecut PROPERTIES
        OUTPUT_NAME FreezeCut
        MACOSX_BUNDLE_BUNDLE_NAME FreezeCut
        MACOSX_BUNDLE_GUI_IDENTIFIER io.github.xopoko.Frameline
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_ICON_FILE FreezeCut.icns)
    target_sources(freezecut PRIVATE resources/FreezeCut.icns)
    set_source_files_properties(resources/FreezeCut.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    if(FREEZECUT_MAC_APP_STORE)
        set_target_properties(freezecut PROPERTIES
            MACOSX_BUNDLE_BUNDLE_VERSION "${FREEZECUT_MAC_BUILD_NUMBER}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/FreezeCutStoreInfo.plist.in")
    endif()
endif()

if(WIN32)
    set_source_files_properties(resources/windows.rc PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/FreezeCut.ico")
    qt_add_executable(FreezeCutGUI WIN32 src/main.cpp resources/resources.qrc resources/windows.rc)
    target_link_libraries(FreezeCutGUI PRIVATE freezecut_core)
    install(TARGETS FreezeCutGUI RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    # Existing Frameline shortcuts and scripts can keep their executable paths.
    install(FILES $<TARGET_FILE:FreezeCutGUI> DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME FramelineGUI.exe)
    install(FILES $<TARGET_FILE:freezecut> DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME frameline.exe)
endif()

install(TARGETS freezecut BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WIN32 OR APPLE)
    set(FREEZECUT_QT_NOTICES_DIR "" CACHE PATH "Directory containing notices for the bundled Qt SDK")
    if(NOT FREEZECUT_QT_NOTICES_DIR AND FRAMELINE_QT_NOTICES_DIR)
        set(FREEZECUT_QT_NOTICES_DIR "${FRAMELINE_QT_NOTICES_DIR}")
    endif()
    if((FREEZECUT_MAC_APP_STORE OR FREEZECUT_WINDOWS_STORE)
            AND NOT EXISTS "${FREEZECUT_QT_NOTICES_DIR}/README.txt")
        message(FATAL_ERROR "Set FREEZECUT_QT_NOTICES_DIR to the notices for the bundled Qt SDK.")
    endif()
    if(WIN32)
        set(notice_destination .)
    else()
        set(notice_destination FreezeCut.app/Contents/Resources)
    endif()
    install(FILES LICENSE DESTINATION ${notice_destination})
    if(FREEZECUT_QT_NOTICES_DIR)
        install(DIRECTORY "${FREEZECUT_QT_NOTICES_DIR}/" DESTINATION ${notice_destination}/Qt-notices)
        install(FILES docs/release.md DESTINATION ${notice_destination} RENAME RELEASE.md)
    endif()
    if(WIN32)
        set(deploy_target FreezeCutGUI)
        if(Qt6_VERSION VERSION_GREATER_EQUAL 6.8)
            set(deploy_options
                --skip-plugin-types generic,networkinformation,tls
                --exclude-plugins qgif,qico
                --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw
                --no-compiler-runtime --no-translations)
        endif()
    else()
        set(deploy_target freezecut)
        if(FREEZECUT_MAC_APP_STORE)
            set(deploy_options -appstore-compliant -no-codesign)
        endif()
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
    install(FILES scripts/configure-gnome-shortcut.py DESTINATION ${CMAKE_INSTALL_DATADIR}/freezecut)
    install(FILES scripts/configure-gnome-shortcut.py DESTINATION ${CMAKE_INSTALL_DATADIR}/frameline)
    install(FILES resources/FreezeCut.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
        RENAME io.github.xopoko.Frameline.png)
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/freezecut RENAME copyright)
    install(FILES docs/release.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/freezecut RENAME RELEASE.md)
    set(CPACK_GENERATOR DEB)
    set(CPACK_PACKAGE_NAME freezecut)
    set(CPACK_PACKAGE_CONTACT "FreezeCut")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_COMPRESSION_TYPE xz)
    set(CPACK_DEBIAN_PACKAGE_SECTION graphics)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins, qt6-wayland, xdg-desktop-portal, wl-clipboard")
    # Replace older packages while keeping their commands available to scripts and desktop shortcuts.
    set(CPACK_DEBIAN_PACKAGE_REPLACES "frameline, screenframe")
    set(CPACK_DEBIAN_PACKAGE_CONFLICTS "frameline, screenframe")
    set(CPACK_DEBIAN_PACKAGE_PROVIDES "frameline, screenframe")
    foreach(previous_name frameline screenframe)
        install(CODE "file(CREATE_LINK freezecut \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/${previous_name}\" SYMBOLIC)")
    endforeach()
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Desktop screenshot utility and annotation editor")
    include(CPack)
endif()

if(BUILD_TESTING)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
    add_executable(freezecut_tests tests/freezecut_tests.cpp)
    target_link_libraries(freezecut_tests PRIVATE freezecut_core Qt6::Test)
    # Renders every window to PNG for visual review: freezecut_shots <output-directory>
    add_executable(freezecut_shots tools/shots.cpp resources/resources.qrc)
    target_link_libraries(freezecut_shots PRIVATE freezecut_core Qt6::Test)
    add_test(NAME freezecut_components COMMAND freezecut_tests)
    set_tests_properties(freezecut_components PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    foreach(component autostart librarysettings libraryserver librarystorage singleinstance tailscale tailrelay settingswindow)
        add_executable(freezecut_${component}_tests tests/${component}_tests.cpp)
        target_link_libraries(freezecut_${component}_tests PRIVATE freezecut_core Qt6::Test Qt6::Network)
        add_test(NAME freezecut_${component} COMMAND freezecut_${component}_tests)
        set_tests_properties(freezecut_${component} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
    add_test(NAME freezecut_cli_help COMMAND freezecut --help)
    add_test(NAME freezecut_cli_rejects_invalid_options COMMAND freezecut capture full --no-preview)
    set_tests_properties(freezecut_cli_rejects_invalid_options PROPERTIES WILL_FAIL TRUE)
endif()
