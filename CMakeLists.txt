cmake_minimum_required(VERSION 3.24)

project(bh61_sdr_framework VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Bh61Options)

set(BH61_HAVE_HACKRF OFF)
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
  src/core/session.cpp
  src/core/vcmp.cpp
  src/core/vmac.cpp
  src/core/version.cpp
)
target_include_directories(bh61_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(BH61_HAVE_HACKRF)
  target_compile_definitions(bh61_core PUBLIC BH61_HAVE_HACKRF=1)
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
)
target_include_directories(bh61_radio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(BH61_HAVE_HACKRF)
  target_sources(bh61_radio PRIVATE src/radio/libhackrf_transport.cpp)
  target_compile_definitions(bh61_radio PUBLIC BH61_HAVE_HACKRF=1)
  target_include_directories(bh61_radio PRIVATE "${BH61_HACKRF_INCLUDE_DIR}")
  target_link_libraries(bh61_radio PRIVATE "${BH61_HACKRF_LIBRARY}")
endif()
add_library(
  bh61_evidence STATIC
  src/evidence/writer.cpp
  src/evidence/passive_capture.cpp
)
target_include_directories(bh61_evidence PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(bh61_evidence PUBLIC bh61_core)

add_library(bh61_app STATIC src/apps/arguments.cpp)
target_include_directories(bh61_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(
  bh61_app
  PUBLIC bh61_core bh61_dsp bh61_radio bh61_evidence
)

add_executable(bh61-radio src/apps/main.cpp)
target_link_libraries(
  bh61-radio
  PRIVATE bh61_app bh61_core bh61_dsp bh61_radio bh61_evidence
)

enable_testing()
add_subdirectory(tests)
