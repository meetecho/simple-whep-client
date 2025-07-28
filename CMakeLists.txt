cmake_minimum_required(VERSION 3.22)

project(simple-whep-client VERSION 1.0 LANGUAGES C)

find_package(PkgConfig REQUIRED)

pkg_check_modules(
    GSTREAMER
    REQUIRED
    IMPORTED_TARGET
    gstreamer-1.0>=1.16
    gstreamer-webrtc-1.0>=1.16
    gstreamer-sdp-1.0>=1.16
    gstreamer-video-1.0
)

pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)

add_executable(whep-client src/whep-client.c)

target_compile_features(whep-client PRIVATE c_std_99)
target_compile_options(
    whep-client
    PRIVATE
        -Wall
        -Wstrict-prototypes
        -Wmissing-prototypes
        -Wmissing-declarations
        -Wunused
)

target_link_libraries(
    whep-client
    PRIVATE PkgConfig::GSTREAMER PkgConfig::GLIB PkgConfig::SOUP
)

if(WIN32)
    target_compile_definitions(whep-client PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(whep-client PRIVATE ws2_32)
endif()

include(GNUInstallDirs)
install(TARGETS whep-client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
