cmake_minimum_required(VERSION 3.7...4.0)

project(scream2diretta VERSION 0.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


add_executable(${PROJECT_NAME}
    src/scream.c src/network.c src/shmem.c src/raw.c
    src/pcm_dump.cpp src/pcm_startup.cpp src/receiver_tap.cpp)
target_compile_definitions(${PROJECT_NAME} PRIVATE SCREAM2DIRETTA_VERSION="${PROJECT_VERSION}")

# find pulseaudio
option(PULSEAUDIO_ENABLE "Enable PulseAudio" ON)
if (PULSEAUDIO_ENABLE)
  find_package(PkgConfig)
  pkg_check_modules(PULSEAUDIO libpulse-simple)
  if (PULSEAUDIO_FOUND)
    include_directories(${PULSEAUDIO_INCLUDE_DIRS})
    target_link_directories(${PROJECT_NAME} PUBLIC ${PULSEAUDIO_LIBRARY_DIRS})
    target_link_libraries(${PROJECT_NAME} ${PULSEAUDIO_LIBRARIES})
    target_sources(${PROJECT_NAME} PRIVATE src/pulseaudio.c)
  else ()
    set(PULSEAUDIO_ENABLE OFF)
  endif ()
endif ()

# find ALSA
option(ALSA_ENABLE "Enable ALSA" ON)
if (ALSA_ENABLE)
  find_package(PkgConfig)
  pkg_check_modules(PC_ALSA alsa)
  if (PC_ALSA_FOUND)
    include_directories(${PC_ALSA_INCLUDEDIR})
    target_link_directories(${PROJECT_NAME} PUBLIC ${PC_ALSA_LIBRARY_DIRS})
    target_link_libraries(${PROJECT_NAME} ${PC_ALSA_LIBRARIES})
    target_sources(${PROJECT_NAME} PRIVATE src/alsa.c)
  else ()
    set(ALSA_ENABLE OFF)
  endif ()
endif ()

# find JACK
option(JACK_ENABLE "Enable JACK" ON)
if (JACK_ENABLE)
  find_package(PkgConfig)
  pkg_check_modules(PC_JACK jack)
  pkg_check_modules(PC_LIBSOXR soxr)
  if(PC_JACK_FOUND AND PC_LIBSOXR_FOUND)
    include_directories(${PC_JACK_INCLUDEDIR} ${PC_LIBSOXR_INCLUDEDIR})
    target_link_directories(${PROJECT_NAME} PUBLIC ${PC_JACK_LIBRARY_DIRS})
    target_link_libraries(${PROJECT_NAME} ${PC_JACK_LIBRARIES} ${PC_LIBSOXR_LIBRARIES})
    target_sources(${PROJECT_NAME} PRIVATE src/jack.c)
  else ()
    set(JACK_ENABLE OFF)
  endif ()
endif ()


# find libpcap
option(PCAP_ENABLE "Enable PCAP" ON)
if (PCAP_ENABLE)
  find_package(PkgConfig)
  pkg_check_modules(PC_PCAP libpcap)
  if (PC_PCAP_FOUND)
    include_directories(${PC_PCAP_INCLUDEDIR})
    target_link_directories(${PROJECT_NAME} PUBLIC ${PC_PCAP_LIBRARY_DIRS})
    target_link_libraries(${PROJECT_NAME} ${PC_PCAP_LIBRARIES})
    target_sources(${PROJECT_NAME} PRIVATE src/pcap_input.c)
  else ()
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
    if (SNDIO_USABLE)
      include_directories(${PC_SNDIO_INCLUDEDIR})
      target_link_directories(${PROJECT_NAME} PUBLIC ${PC_SNDIO_LIBRARY_DIRS})
      target_link_libraries(${PROJECT_NAME} ${PC_SNDIO_LIBRARIES})
      target_sources(${PROJECT_NAME} PRIVATE src/sndio.c)
    else ()
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
  target_sources(${PROJECT_NAME} PRIVATE src/diretta.cpp src/diretta_sync.cpp)
  target_include_directories(${PROJECT_NAME} PRIVATE "${DIRETTA_SDK_ROOT}/Host")
  # The SDK archives cross-reference each other; --start-group/--end-group
  # is the most reliable way to satisfy that without ordering surprises.
  target_link_libraries(${PROJECT_NAME}
    -Wl,--start-group
    "${_diretta_libDiretta}"
    "${_diretta_libACQUA}"
    -Wl,--end-group
    pthread m stdc++)
endif ()

target_include_directories(${PROJECT_NAME} PRIVATE src)
configure_file(config.h.in config.h)
target_include_directories(${PROJECT_NAME} PUBLIC "${PROJECT_BINARY_DIR}")

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
