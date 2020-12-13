cmake_minimum_required (VERSION 3.17.0)
project(sbitx)
include(FeatureSummary)

find_library(wiringPi_LIB wiringPi)

find_package(ALSA)
set_package_properties(ALSA PROPERTIES
                       URL "http://www.alsa-project.org/"
                       DESCRIPTION "ALSA provides audio and MIDI functionality"
                       TYPE REQUIRED
                       PURPOSE "Pass-through audio via ALSA"
)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads)
set_package_properties(Threads PROPERTIES
                        URL "https://man7.org/linux/man-pages/man7/pthreads.7.html"
                        DESCRIPTION "The system thread library"
                        TYPE REQUIRED
)

find_package(SQLite3)
set_package_properties(SQLite3 PROPERTIES
                        URL "http://www.sqlite.org"
                        DESCRIPTION "SQLite is a Binary-Database"
                        TYPE REQUIRED
)

find_package(Curses)
set_package_properties(Curses PROPERTIES
                        URL "https://www.gnu.org/software/ncurses/"
                        DESCRIPTION "ncurses provides text-based UI capabilities"
                        TYPE REQUIRED
)

set(FFTW_DIR ${CMAKE_SOURCE_DIR}/CMake)
find_package(FFTW REQUIRED)
set_package_properties(FFTW PROPERTIES
                        URL "http://fftw.org/"
                        DESCRIPTION "FFTW is a C subroutine library for computing discrete Fourier transform."
                        TYPE REQUIRED
                        PURPOSE "Used to process incoming audio stream for signals."
)

find_package(PkgConfig)
set_package_properties(PkgConfig PROPERTIES
                        URL "https://www.freedesktop.org/wiki/Software/pkg-config/"
                        DESCRIPTION "pkg-config is a helper tool used when compiling applications and libraries."
                        TYPE REQUIRED
                        PURPOSE "Used to find cflags & libs for gtk3+."
)

add_executable (sbitx
    sbitx.c
    vfo.c
    si570.c
    sbitx_sound.c
    fft_filter.c
    sbitx_gtk.c
)

pkg_check_modules(GTK3+ REQUIRED gtk+-3.0)
target_link_libraries (sbitx PUBLIC
    ${wiringPi_LIB}
    ${ALSA_LIBRARIES}
    fftw3
    fftw3f
    m
    Threads::Threads
    SQLite::SQLite3
    ncurses
    ${GTK3+_LIBRARIES}
)
target_compile_options(sbitx PUBLIC ${GTK3+_CFLAGS})

install (TARGETS sbitx
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime
  BUNDLE DESTINATION . COMPONENT runtime
)

configure_file (
    "${CMAKE_CURRENT_SOURCE_DIR}/CMake/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    @ONLY
)
add_custom_target (uninstall
    "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
)
