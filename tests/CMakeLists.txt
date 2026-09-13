function(bh61_add_test test_name)
  add_executable(${test_name} runner.cpp ${ARGN})
  target_include_directories(
    ${test_name}
    PRIVATE
      "${PROJECT_SOURCE_DIR}/include"
      "${CMAKE_CURRENT_SOURCE_DIR}"
  )
  target_link_libraries(${test_name} PRIVATE bh61_core)
  add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

bh61_add_test(bh61-build-smoke-tests test_main.cpp)
bh61_add_test(bh61-wireshark-tool-tests research/test_wireshark_tool.cpp)
target_compile_definitions(
  bh61-wireshark-tool-tests PRIVATE BH61_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
bh61_add_test(bh61-crc-tests core/test_crc16.cpp)
bh61_add_test(bh61-sensor-model-tests core/test_sensor_model.cpp)
bh61_add_test(
  bh61-frame-tests
  core/test_frame.cpp
  core/test_vmac.cpp
)
bh61_add_test(
  bh61-vcmp-tests
  core/test_vcmp.cpp
  core/test_messages.cpp
  core/test_session.cpp
)
bh61_add_test(bh61-p256-tests core/test_p256.cpp)
bh61_add_test(bh61-sensor-session-tests core/test_sensor_session.cpp)
bh61_add_test(bh61-dsss-tests dsp/test_dsss.cpp)
target_link_libraries(bh61-dsss-tests PRIVATE bh61_dsp)
bh61_add_test(
  bh61-modulator-tests
  dsp/test_profile.cpp
  dsp/test_modulator.cpp
)
target_link_libraries(bh61-modulator-tests PRIVATE bh61_dsp bh61_evidence)
target_compile_definitions(
  bh61-modulator-tests PRIVATE BH61_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
bh61_add_test(
  bh61-demodulator-tests
  dsp/test_roundtrip.cpp
  dsp/test_impairments.cpp
)
target_link_libraries(bh61-demodulator-tests PRIVATE bh61_dsp)
bh61_add_test(
  bh61-file-evidence-tests
  radio/test_file_device.cpp
  evidence/test_writer.cpp
  evidence/test_passive_capture.cpp
  evidence/test_lab_event.cpp
)
target_link_libraries(
  bh61-file-evidence-tests
  PRIVATE bh61_radio bh61_evidence
)
bh61_add_test(bh61-hackrf-types-tests radio/test_hackrf_types.cpp)
target_link_libraries(bh61-hackrf-types-tests PRIVATE bh61_radio)
bh61_add_test(bh61-hackrf-device-tests radio/test_hackrf_device.cpp)
target_link_libraries(bh61-hackrf-device-tests PRIVATE bh61_radio)
bh61_add_test(bh61-uhd-device-tests radio/test_uhd_device.cpp)
target_link_libraries(bh61-uhd-device-tests PRIVATE bh61_radio)
bh61_add_test(bh61-rtlsdr-device-tests radio/test_rtlsdr_device.cpp)
target_link_libraries(bh61-rtlsdr-device-tests PRIVATE bh61_radio)
bh61_add_test(bh61-device-factory-tests radio/test_device_factory.cpp)
target_link_libraries(bh61-device-factory-tests PRIVATE bh61_radio)
if(BH61_HAVE_HACKRF)
  bh61_add_test(
    bh61-libhackrf-contract-tests
    radio/test_libhackrf_contract.cpp
  )
  target_link_libraries(bh61-libhackrf-contract-tests PRIVATE bh61_radio)
  target_include_directories(
    bh61-libhackrf-contract-tests PRIVATE "${BH61_HACKRF_INCLUDE_DIR}"
  )
endif()
bh61_add_test(bh61-cli-tests apps/test_cli.cpp)
target_link_libraries(
  bh61-cli-tests
  PRIVATE bh61_app bh61_dsp bh61_radio bh61_evidence
)
bh61_add_test(
  bh61-sensor-session-runner-tests
  apps/test_sensor_session_runner.cpp
)
target_link_libraries(
  bh61-sensor-session-runner-tests PRIVATE bh61_app bh61_dsp bh61_radio
)
