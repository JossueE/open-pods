cmake_minimum_required(VERSION 3.16)

project(open_pods LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED dbus-1)

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
