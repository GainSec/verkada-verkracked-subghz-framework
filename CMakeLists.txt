cmake_minimum_required(VERSION 3.24)

project(bh61_sdr_framework VERSION 0.1.0 LANGUAGES CXX)

find_package(OpenSSL REQUIRED COMPONENTS Crypto)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Bh61Options)

set(BH61_HAVE_HACKRF OFF)
set(BH61_HAVE_UHD OFF)
set(BH61_HAVE_RTLSDR OFF)
if(BH61_ENABLE_RTLSDR)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(BH61_RTLSDR QUIET IMPORTED_TARGET librtlsdr)
  endif()
  if(BH61_RTLSDR_FOUND)
    set(BH61_HAVE_RTLSDR ON)
    message(STATUS "Native RTL-SDR backend available (receive-only)")
  else()
    message(STATUS "Native RTL-SDR backend unavailable; building without it")
  endif()
endif()
if(BH61_ENABLE_UHD)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(BH61_UHD QUIET IMPORTED_TARGET uhd)
  endif()
  if(BH61_UHD_FOUND)
    set(BH61_HAVE_UHD ON)
    message(STATUS "Native UHD backend available")
  else()
    message(STATUS "Native UHD backend unavailable; building without it")
  endif()
endif()
if(BH61_ENABLE_HACKRF OR BH61_REQUIRE_HACKRF)
  find_path(BH61_HACKRF_INCLUDE_DIR NAMES libhackrf/hackrf.h)
  find_library(BH61_HACKRF_LIBRARY NAMES hackrf)
  if(BH61_HACKRF_INCLUDE_DIR AND BH61_HACKRF_LIBRARY)
    set(BH61_HAVE_HACKRF ON)
    message(STATUS "Native HackRF backend available")
  elseif(BH61_REQUIRE_HACKRF)
    message(FATAL_ERROR
      "BH61_REQUIRE_HACKRF=ON but libhackrf headers/library were not found")
  else()
    message(STATUS "Native HackRF backend unavailable; building without it")
  endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(
  bh61_core STATIC
  src/core/crc16.cpp
  src/core/frame.cpp
  src/core/messages.cpp
  src/core/p256.cpp
  src/core/session.cpp
  src/core/sha256.cpp
  src/core/sensor_model.cpp
  src/core/sensor_session.cpp
  src/core/vcmp.cpp
  src/core/vmac.cpp
  src/core/version.cpp
)
target_include_directories(bh61_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(bh61_core PRIVATE OpenSSL::Crypto)
if(BH61_HAVE_HACKRF)
  target_compile_definitions(bh61_core PUBLIC BH61_HAVE_HACKRF=1)
endif()
if(BH61_ENABLE_TX)
  target_compile_definitions(bh61_core PUBLIC BH61_ENABLE_TX=1)
endif()
if(BH61_HAVE_UHD)
  target_compile_definitions(bh61_core PUBLIC BH61_HAVE_UHD=1)
endif()
if(BH61_HAVE_RTLSDR)
  target_compile_definitions(bh61_core PUBLIC BH61_HAVE_RTLSDR=1)
endif()

add_library(
  bh61_dsp STATIC
  src/dsp/acquisition.cpp
  src/dsp/demodulator.cpp
  src/dsp/dsss.cpp
  src/dsp/library.cpp
  src/dsp/modulator.cpp
  src/dsp/profile.cpp
)
target_include_directories(bh61_dsp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(bh61_dsp PUBLIC bh61_core)

add_library(
  bh61_radio STATIC
  src/radio/device_factory.cpp
  src/radio/file_device.cpp
  src/radio/hackrf_device.cpp
  src/radio/hackrf_types.cpp
  src/radio/library.cpp
  src/radio/uhd_device.cpp
  src/radio/rtlsdr_device.cpp
)
target_include_directories(bh61_radio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(BH61_HAVE_HACKRF)
  target_sources(bh61_radio PRIVATE src/radio/libhackrf_transport.cpp)
  target_compile_definitions(bh61_radio PUBLIC BH61_HAVE_HACKRF=1)
  target_include_directories(bh61_radio PRIVATE "${BH61_HACKRF_INCLUDE_DIR}")
  target_link_libraries(bh61_radio PRIVATE "${BH61_HACKRF_LIBRARY}")
endif()
if(BH61_ENABLE_TX)
  target_compile_definitions(bh61_radio PUBLIC BH61_ENABLE_TX=1)
endif()
if(BH61_HAVE_UHD)
  target_sources(bh61_radio PRIVATE src/radio/libuhd_transport.cpp)
  target_compile_definitions(bh61_radio PUBLIC BH61_HAVE_UHD=1)
  target_link_libraries(bh61_radio PRIVATE PkgConfig::BH61_UHD)
endif()
if(BH61_HAVE_RTLSDR)
  target_sources(bh61_radio PRIVATE src/radio/librtlsdr_transport.cpp)
  target_compile_definitions(bh61_radio PUBLIC BH61_HAVE_RTLSDR=1)
  target_link_libraries(bh61_radio PRIVATE PkgConfig::BH61_RTLSDR)
endif()

add_library(
  bh61_evidence STATIC
  src/evidence/writer.cpp
  src/evidence/passive_capture.cpp
  src/evidence/lab_event.cpp
)
target_include_directories(bh61_evidence PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(bh61_evidence PUBLIC bh61_core)

add_library(
  bh61_app STATIC
  src/apps/arguments.cpp
  src/apps/sensor_session_runner.cpp
)
target_include_directories(bh61_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(
  bh61_app
  PUBLIC bh61_core bh61_dsp bh61_radio bh61_evidence
  PRIVATE OpenSSL::Crypto
)

add_executable(bh61-radio src/apps/main.cpp)
target_link_libraries(
  bh61-radio
  PRIVATE bh61_app bh61_core bh61_dsp bh61_radio bh61_evidence
)

enable_testing()
add_subdirectory(tests)
