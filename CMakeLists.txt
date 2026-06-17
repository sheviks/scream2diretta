cmake_minimum_required(VERSION 3.7...4.0)

project(scream2diretta VERSION 0.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# We build two binaries from the same source tree:
#
#   scream2diretta        production build, -DSCREAM2DIRETTA_NO_DIAGNOSTICS
#                         All diagnostic taps (--dump-*, --startup-analyze-ms,
#                         --compare-receiver-tap-ms) are compile-time-disabled.
#                         The static inline receiver_tap_any_armed() in
#                         receiver_tap.h becomes a constant 0, so every
#                         per-packet `if (receiver_tap_any_armed())` block in
#                         scream.c / raw.c / diretta.cpp gets DCE'd by the
#                         compiler. Zero per-packet diagnostic overhead.
#
#   scream2diretta-debug  full build with all diagnostic facilities armed
#                         at runtime via CLI flags. Same source, same Diretta
#                         path, single extra runtime branch per packet when
#                         nothing is armed.
#
# All option() flags below (DIRETTA_ENABLE / PULSEAUDIO_ENABLE / ALSA_ENABLE /
# JACK_ENABLE / PCAP_ENABLE / SNDIO_ENABLE / DIRETTA_NOLOG / DIRETTA_SDK_ROOT /
# DIRETTA_ARCH_SUFFIX) are honoured identically by both targets.
option(SCREAM2DIRETTA_BUILD_DEBUG_VARIANT
       "Also build scream2diretta-debug (full diagnostics)" ON)

set(SCREAM2DIRETTA_SOURCES
    src/scream.c src/network.c src/shmem.c src/raw.c
    src/pcm_dump.cpp src/pcm_startup.cpp src/receiver_tap.cpp)

# find pulseaudio
option(PULSEAUDIO_ENABLE "Enable PulseAudio" ON)
if (PULSEAUDIO_ENABLE)
  find_package(PkgConfig)
  pkg_check_modules(PULSEAUDIO libpulse-simple)
  if (NOT PULSEAUDIO_FOUND)
    set(PULSEAUDIO_ENABLE OFF)
  endif ()
endif ()

# find ALSA
option(ALSA_ENABLE "Enable ALSA" ON)
if (ALSA_ENABLE)
  find_package(PkgConfig)
  pkg_check_modules(PC_ALSA alsa)
  if (NOT PC_ALSA_FOUND)
    set(ALSA_ENABLE OFF)
  endif ()
endif ()

# find JACK
option(JACK_ENABLE "Enable JACK" ON)
if (JACK_ENABLE)
  find_package(PkgConfig)
  pkg_check_modules(PC_JACK jack)
  pkg_check_modules(PC_LIBSOXR soxr)
  if (NOT (PC_JACK_FOUND AND PC_LIBSOXR_FOUND))
    set(JACK_ENABLE OFF)
  endif ()
endif ()

# find libpcap
option(PCAP_ENABLE "Enable PCAP" ON)
if (PCAP_ENABLE)
  find_package(PkgConfig)
  pkg_check_modules(PC_PCAP libpcap)
  if (NOT PC_PCAP_FOUND)
    set(PCAP_ENABLE OFF)
  endif ()
endif ()

option(SNDIO_ENABLE "Enable sndio" ON)
if (SNDIO_ENABLE)
  find_package(PkgConfig)
  pkg_check_modules(PC_SNDIO sndio)
  if (PC_SNDIO_FOUND)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_INCLUDES ${PC_SNDIO_INCLUDEDIR})
    check_c_source_compiles("
      #include <sndio.h>
      int main(void) {
        struct sio_par p;
        struct sio_hdl *h = sio_open(SIO_DEVANY, SIO_PLAY, 0);
        (void)h; (void)p;
        return 0;
      }
    " SNDIO_USABLE)
    if (NOT SNDIO_USABLE)
      message(WARNING "sndio headers found but unusable (incomplete); disabling sndio support")
      set(SNDIO_ENABLE OFF)
    endif ()
  else ()
    set(SNDIO_ENABLE OFF)
  endif ()
endif ()

# Diretta backend.
#
# Requires the Diretta Host SDK (proprietary). Point DIRETTA_SDK_ROOT at the
# extracted SDK directory that contains Host/ and lib/. The arch suffix selects
# which static archive variant to link; default is x64-linux-15v3.
#
# Example:
#   cmake -DDIRETTA_ENABLE=ON \
#         -DDIRETTA_SDK_ROOT=/path/to/DirettaHostSDK_149 \
#         -DDIRETTA_ARCH_SUFFIX=x64-linux-15v3 ..
option(DIRETTA_ENABLE "Enable Diretta output" OFF)
set(DIRETTA_SDK_ROOT "" CACHE PATH "Path to extracted DirettaHostSDK_149 (must contain Host/ and lib/)")
set(DIRETTA_ARCH_SUFFIX "x64-linux-15v3" CACHE STRING "Diretta arch suffix used for libACQUA_<suffix>.a / libDirettaHost_<suffix>.a")
option(DIRETTA_NOLOG "Link the -nolog SDK variants" OFF)

if (DIRETTA_ENABLE)
  if (NOT DIRETTA_SDK_ROOT OR NOT EXISTS "${DIRETTA_SDK_ROOT}/Host" OR NOT EXISTS "${DIRETTA_SDK_ROOT}/lib")
    message(FATAL_ERROR
      "DIRETTA_ENABLE=ON requires DIRETTA_SDK_ROOT to point at a directory "
      "containing Host/ and lib/ (the extracted DirettaHostSDK_149).")
  endif ()
  if (DIRETTA_NOLOG)
    set(_diretta_suffix "${DIRETTA_ARCH_SUFFIX}-nolog")
  else ()
    set(_diretta_suffix "${DIRETTA_ARCH_SUFFIX}")
  endif ()
  set(_diretta_libDiretta "${DIRETTA_SDK_ROOT}/lib/libDirettaHost_${_diretta_suffix}.a")
  set(_diretta_libACQUA "${DIRETTA_SDK_ROOT}/lib/libACQUA_${_diretta_suffix}.a")
  if (NOT EXISTS "${_diretta_libDiretta}")
    message(FATAL_ERROR "Diretta SDK library not found: ${_diretta_libDiretta}")
  endif ()
  if (NOT EXISTS "${_diretta_libACQUA}")
    message(FATAL_ERROR "ACQUA SDK library not found: ${_diretta_libACQUA}")
  endif ()
endif ()

# Helper: define one scream2diretta target with the shared option matrix.
#
#   target_name : output executable name
#   no_diag     : if ON, define SCREAM2DIRETTA_NO_DIAGNOSTICS
function(scream2diretta_define_target target_name no_diag)
  add_executable(${target_name} ${SCREAM2DIRETTA_SOURCES})
  target_compile_definitions(${target_name} PRIVATE
      SCREAM2DIRETTA_VERSION="${PROJECT_VERSION}")
  if (no_diag)
    target_compile_definitions(${target_name} PRIVATE SCREAM2DIRETTA_NO_DIAGNOSTICS)
  endif ()
  target_include_directories(${target_name} PRIVATE src)
  target_include_directories(${target_name} PUBLIC "${PROJECT_BINARY_DIR}")

  if (PULSEAUDIO_ENABLE)
    target_include_directories(${target_name} PRIVATE ${PULSEAUDIO_INCLUDE_DIRS})
    target_link_directories(${target_name} PUBLIC ${PULSEAUDIO_LIBRARY_DIRS})
    target_link_libraries(${target_name} ${PULSEAUDIO_LIBRARIES})
    target_sources(${target_name} PRIVATE src/pulseaudio.c)
  endif ()
  if (ALSA_ENABLE)
    target_include_directories(${target_name} PRIVATE ${PC_ALSA_INCLUDEDIR})
    target_link_directories(${target_name} PUBLIC ${PC_ALSA_LIBRARY_DIRS})
    target_link_libraries(${target_name} ${PC_ALSA_LIBRARIES})
    target_sources(${target_name} PRIVATE src/alsa.c)
  endif ()
  if (JACK_ENABLE)
    target_include_directories(${target_name} PRIVATE ${PC_JACK_INCLUDEDIR} ${PC_LIBSOXR_INCLUDEDIR})
    target_link_directories(${target_name} PUBLIC ${PC_JACK_LIBRARY_DIRS})
    target_link_libraries(${target_name} ${PC_JACK_LIBRARIES} ${PC_LIBSOXR_LIBRARIES})
    target_sources(${target_name} PRIVATE src/jack.c)
  endif ()
  if (PCAP_ENABLE)
    target_include_directories(${target_name} PRIVATE ${PC_PCAP_INCLUDEDIR})
    target_link_directories(${target_name} PUBLIC ${PC_PCAP_LIBRARY_DIRS})
    target_link_libraries(${target_name} ${PC_PCAP_LIBRARIES})
    target_sources(${target_name} PRIVATE src/pcap_input.c)
  endif ()
  if (SNDIO_ENABLE)
    target_include_directories(${target_name} PRIVATE ${PC_SNDIO_INCLUDEDIR})
    target_link_directories(${target_name} PUBLIC ${PC_SNDIO_LIBRARY_DIRS})
    target_link_libraries(${target_name} ${PC_SNDIO_LIBRARIES})
    target_sources(${target_name} PRIVATE src/sndio.c)
  endif ()
  if (DIRETTA_ENABLE)
    target_sources(${target_name} PRIVATE src/diretta.cpp src/diretta_sync.cpp)
    target_include_directories(${target_name} PRIVATE "${DIRETTA_SDK_ROOT}/Host")
    target_link_libraries(${target_name}
      -Wl,--start-group
      "${_diretta_libDiretta}"
      "${_diretta_libACQUA}"
      -Wl,--end-group
      pthread m stdc++)
  endif ()
endfunction()

# Production binary: no diagnostics. Installed by default.
scream2diretta_define_target(scream2diretta ON)

# Debug binary: same code, full diagnostics armable at runtime.
if (SCREAM2DIRETTA_BUILD_DEBUG_VARIANT)
  scream2diretta_define_target(scream2diretta-debug OFF)
endif ()

configure_file(config.h.in config.h)

include(GNUInstallDirs)
install(TARGETS scream2diretta RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if (SCREAM2DIRETTA_BUILD_DEBUG_VARIANT)
  install(TARGETS scream2diretta-debug RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif ()
