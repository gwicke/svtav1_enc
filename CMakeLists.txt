cmake_minimum_required(VERSION 3.16)
project(svtav1_wrap C)

set(CMAKE_C_STANDARD 11)

# --- Find SvtAv1Enc via pkg-config ----------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(SVT_AV1 REQUIRED SvtAv1Enc)

# --- Shared library --------------------------------------------------------
add_library(svtav1_wrap SHARED svtav1_wrap.c)

target_include_directories(svtav1_wrap
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${SVT_AV1_INCLUDE_DIRS}
)

target_link_libraries(svtav1_wrap
    PRIVATE ${SVT_AV1_LIBRARIES}
)

target_compile_options(svtav1_wrap PRIVATE ${SVT_AV1_CFLAGS_OTHER} -O2 -Wall -Wextra)

set_target_properties(svtav1_wrap PROPERTIES
    VERSION   1.0.0
    SOVERSION 1
    PUBLIC_HEADER svtav1_wrap.h
)

# --- Static library (optional) ---------------------------------------------
add_library(svtav1_wrap_static STATIC svtav1_wrap.c)

target_include_directories(svtav1_wrap_static
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${SVT_AV1_INCLUDE_DIRS}
)

target_link_libraries(svtav1_wrap_static
    PRIVATE ${SVT_AV1_LIBRARIES}
)

target_compile_options(svtav1_wrap_static PRIVATE ${SVT_AV1_CFLAGS_OTHER} -O2 -Wall -Wextra)

set_target_properties(svtav1_wrap_static PROPERTIES
    OUTPUT_NAME svtav1_wrap
)

# --- Install ---------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS svtav1_wrap svtav1_wrap_static
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# --- pkg-config file -------------------------------------------------------
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/svtav1_wrap.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/svtav1_wrap.pc
    @ONLY
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/svtav1_wrap.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
