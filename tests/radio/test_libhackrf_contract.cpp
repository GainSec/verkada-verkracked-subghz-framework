#include "bh61/radio/libhackrf_transport.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct NativeState {
  int initialize_result{HACKRF_SUCCESS};
  int initialize_calls{};
  int shutdown_calls{};
  int open_calls{};
  int close_calls{};
  int list_free_calls{};
  int set_frequency_result{HACKRF_SUCCESS};
  std::vector<std::string> operations;
  hackrf_sample_block_cb_fn rx_callback{};
  void* rx_context{};
  hackrf_device* handle{reinterpret_cast<hackrf_device*>(0x1234)};
  std::array<char*, 1> serials{};
  std::array<hackrf_usb_board_id, 1> board_ids{USB_BOARD_ID_HACKRF_ONE};
  std::array<int, 1> usb_indices{0};
  hackrf_device_list_t list{};
};

auto functions(NativeState& state) -> bh61::radio::LibhackrfFunctions {
  state.serials[0] = const_cast<char*>("target");
  state.list.serial_numbers = state.serials.data();
  state.list.usb_board_ids = state.board_ids.data();
  state.list.usb_device_index = state.usb_indices.data();
  state.list.devicecount = 1;
  return {
      [&state] {
        ++state.initialize_calls;
        return state.initialize_result;
      },
      [&state] {
        ++state.shutdown_calls;
        return HACKRF_SUCCESS;
      },
      [&state] { return &state.list; },
      [&state](hackrf_device_list_t*) { ++state.list_free_calls; },
      [&state](const char*, hackrf_device** output) {
        ++state.open_calls;
        *output = state.handle;
        return HACKRF_SUCCESS;
      },
      [&state](hackrf_device*) {
        ++state.close_calls;
        return HACKRF_SUCCESS;
      },
      [](hackrf_device*, std::uint8_t* value) {
        *value = 5U;
        return HACKRF_SUCCESS;
      },
      [](hackrf_device*, char* output, std::uint8_t) {
        std::strcpy(output, "git-test");
        return HACKRF_SUCCESS;
      },
      [](hackrf_device*, std::uint16_t* value) {
        *value = 0x0109U;
        return HACKRF_SUCCESS;
      },
      [](hackrf_device*, std::uint8_t* value) {
        *value = 12U;
        return HACKRF_SUCCESS;
      },
      [](hackrf_board_id) { return "HackRF Pro"; },
      [](hackrf_board_rev) { return "r1.2"; },
      [](hackrf_error) { return "injected_native_error"; },
      [&state](hackrf_device*, double) {
        state.operations.emplace_back("sample_rate");
        return HACKRF_SUCCESS;
      },
      [](std::uint32_t value) { return value; },
      [&state](hackrf_device*, std::uint32_t) {
        state.operations.emplace_back("filter");
        return HACKRF_SUCCESS;
      },
      [&state](hackrf_device*, std::uint64_t) {
        state.operations.emplace_back("frequency");
        return state.set_frequency_result;
      },
      [&state](hackrf_device*, std::uint32_t) {
        state.operations.emplace_back("lna");
        return HACKRF_SUCCESS;
      },
      [&state](hackrf_device*, std::uint32_t) {
        state.operations.emplace_back("vga");
        return HACKRF_SUCCESS;
      },
      [&state](hackrf_device*, std::uint8_t) {
        state.operations.emplace_back("amp");
        return HACKRF_SUCCESS;
      },
      [&state](hackrf_device*, std::uint8_t) {
        state.operations.emplace_back("antenna");
        return HACKRF_SUCCESS;
      },
      [&state](hackrf_device*, hackrf_sample_block_cb_fn callback,
               void* context) {
        state.rx_callback = callback;
        state.rx_context = context;
        return HACKRF_SUCCESS;
      },
      [](hackrf_device*) { return HACKRF_SUCCESS; }};
}

}  // namespace

BH61_TEST("libhackrf initialization failure preserves native error evidence") {
  NativeState state;
  state.initialize_result = HACKRF_ERROR_LIBUSB;
  bool failed = false;
  try {
    bh61::radio::LibhackrfTransport transport(functions(state));
  } catch (const bh61::radio::NativeHackrfError& error) {
    failed = true;
    BH61_REQUIRE(error.native_code() == HACKRF_ERROR_LIBUSB);
    BH61_REQUIRE(error.operation() == "hackrf_init");
    BH61_REQUIRE(std::string(error.what()).find("injected_native_error") !=
                 std::string::npos);
  }
  BH61_REQUIRE(failed);
  BH61_REQUIRE(state.initialize_calls == 1);
  BH61_REQUIRE(state.shutdown_calls == 0);
}

BH61_TEST("libhackrf transport enumerates identity configures in safe order") {
  NativeState state;
  {
    bh61::radio::LibhackrfTransport transport(functions(state));
    const auto attached = transport.enumerate();
    BH61_REQUIRE(attached.size() == 1U);
    BH61_REQUIRE(attached[0].serial == "target");
    BH61_REQUIRE(attached[0].board_name == "HackRF Pro");
    BH61_REQUIRE(attached[0].firmware_version == "git-test (API:1.09)");
    BH61_REQUIRE(attached[0].hardware_revision == "r1.2");
    BH61_REQUIRE(state.list_free_calls == 1);
    BH61_REQUIRE(state.close_calls == 1);

    transport.open("target");
    bh61::radio::HackrfConfiguration configuration;
    configuration.serial = "target";
    const auto realized = transport.configure(configuration);
    BH61_REQUIRE(realized.sample_rate == 4'000'000U);
    const std::vector<std::string> expected{
        "sample_rate", "filter", "frequency", "lna", "vga", "amp",
        "antenna"};
    BH61_REQUIRE(state.operations == expected);
    transport.close();
  }
  BH61_REQUIRE(state.shutdown_calls == 1);
}

BH61_TEST("libhackrf callback forwards only valid signed IQ bytes") {
  NativeState state;
  bh61::radio::LibhackrfTransport transport(functions(state));
  transport.open("target");
  std::vector<std::int8_t> observed;
  transport.start_rx([&observed](const bh61::radio::HackrfRxTransfer& transfer) {
    observed.assign(transfer.bytes.begin(), transfer.bytes.end());
  });
  std::array<std::uint8_t, 6> raw{0x80U, 0x7fU, 1U, 2U, 3U, 4U};
  hackrf_transfer transfer{};
  transfer.buffer = raw.data();
  transfer.buffer_length = 6;
  transfer.valid_length = 4;
  transfer.rx_ctx = state.rx_context;
  BH61_REQUIRE(state.rx_callback(&transfer) == 0);
  constexpr std::array<std::int8_t, 4> expected{-128, 127, 1, 2};
  BH61_REQUIRE(observed.size() == expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    BH61_REQUIRE(observed[index] == expected[index]);
  }
  transport.stop_rx();
  transport.close();
}

BH61_TEST("libhackrf configuration error identifies the exact failing call") {
  NativeState state;
  state.set_frequency_result = HACKRF_ERROR_LIBUSB;
  bh61::radio::LibhackrfTransport transport(functions(state));
  transport.open("target");
  bh61::radio::HackrfConfiguration configuration;
  configuration.serial = "target";
  bool failed = false;
  try {
    static_cast<void>(transport.configure(configuration));
  } catch (const bh61::radio::NativeHackrfError& error) {
    failed = true;
    BH61_REQUIRE(error.operation() == "hackrf_set_freq");
    BH61_REQUIRE(error.native_code() == HACKRF_ERROR_LIBUSB);
  }
  BH61_REQUIRE(failed);
  transport.close();
}
