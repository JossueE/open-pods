cmake_minimum_required(VERSION 3.16)

project(open_pods VERSION 0.1.0 LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED dbus-1)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB_RECURSE OPEN_PODS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(open-pods ${OPEN_PODS_SOURCES})

target_include_directories(open-pods
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        ${DBUS_INCLUDE_DIRS}
)

target_compile_options(open-pods
    PRIVATE
        ${DBUS_CFLAGS_OTHER}
        -Wall
        -Wextra
        -Wpedantic
)

target_link_libraries(open-pods
    PRIVATE
        bluetooth
        pulse
        ${DBUS_LIBRARIES}
)

install(TARGETS open-pods
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES packaging/open-pods.service
    DESTINATION lib/systemd/user
)

install(DIRECTORY extensions/gnome/open-pods@jossuee.dev
    DESTINATION share/gnome-shell/extensions
)

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "open-pods")
set(CPACK_PACKAGE_VENDOR "JossueE")
set(CPACK_PACKAGE_CONTACT "JossueE")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Terminal UI and daemon for managing AirPods on Linux")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/JossueE/open-pods")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "bluez, dbus, libbluetooth3, libdbus-1-3, libpulse0")

include(CPack)
