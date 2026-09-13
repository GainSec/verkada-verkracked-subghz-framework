#pragma once

#include "bh61/radio/hackrf_transport.hpp"

#include <libhackrf/hackrf.h>

#include <cstdint>
#include <condition_variable>
#include <functional>
#include <span>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bh61::radio {

class NativeHackrfError final : public std::runtime_error {
 public:
  NativeHackrfError(std::string operation, int native_code,
                    std::string native_name);
  auto operation() const noexcept -> std::string_view;
  auto native_code() const noexcept -> int;

 private:
  std::string operation_;
  int native_code_{};
};

struct LibhackrfFunctions {
  std::function<int()> initialize;
  std::function<int()> shutdown;
  std::function<hackrf_device_list_t*()> device_list;
  std::function<void(hackrf_device_list_t*)> device_list_free;
  std::function<int(const char*, hackrf_device**)> open_by_serial;
  std::function<int(hackrf_device*)> close;
  std::function<int(hackrf_device*, std::uint8_t*)> board_id_read;
  std::function<int(hackrf_device*, char*, std::uint8_t)> version_string_read;
  std::function<int(hackrf_device*, std::uint16_t*)> usb_api_version_read;
  std::function<int(hackrf_device*, std::uint8_t*)> board_rev_read;
  std::function<const char*(hackrf_board_id)> board_id_name;
  std::function<const char*(hackrf_board_rev)> board_rev_name;
  std::function<const char*(hackrf_error)> error_name;
  std::function<int(hackrf_device*, double)> set_sample_rate;
  std::function<std::uint32_t(std::uint32_t)> compute_baseband_filter;
  std::function<int(hackrf_device*, std::uint32_t)> set_baseband_filter;
  std::function<int(hackrf_device*, std::uint64_t)> set_frequency;
  std::function<int(hackrf_device*, std::uint32_t)> set_lna_gain;
  std::function<int(hackrf_device*, std::uint32_t)> set_vga_gain;
  std::function<int(hackrf_device*, std::uint32_t)> set_tx_vga_gain;
  std::function<int(hackrf_device*, std::uint8_t)> set_amplifier;
  std::function<int(hackrf_device*, std::uint8_t)> set_antenna_power;
  std::function<int(hackrf_device*, hackrf_sample_block_cb_fn, void*)> start_rx;
  std::function<int(hackrf_device*)> stop_rx;
  std::function<int(hackrf_device*, hackrf_flush_cb_fn, void*)>
      enable_tx_flush;
  std::function<int(hackrf_device*, hackrf_sample_block_cb_fn, void*)> start_tx;
  std::function<int(hackrf_device*)> stop_tx;

  static auto native() -> LibhackrfFunctions;
};

class LibhackrfTransport final : public HackrfTransport {
 public:
  explicit LibhackrfTransport(
      LibhackrfFunctions functions = LibhackrfFunctions::native());
  ~LibhackrfTransport() override;

  LibhackrfTransport(const LibhackrfTransport&) = delete;
  auto operator=(const LibhackrfTransport&) -> LibhackrfTransport& = delete;

  auto enumerate() -> std::vector<HackrfIdentity> override;
  void open(std::string_view serial) override;
  auto configure(const HackrfConfiguration& configuration)
      -> HackrfRealizedConfiguration override;
  void start_rx(HackrfRxCallback callback) override;
  void stop_rx() override;
  void close() override;
  void transmit(std::span<const std::int8_t> bytes) override;

 private:
  static auto rx_thunk(hackrf_transfer* transfer) -> int;
  static auto tx_thunk(hackrf_transfer* transfer) -> int;
  static void tx_flush_thunk(void* context, int result);
  auto on_rx(hackrf_transfer* transfer) noexcept -> int;
  auto on_tx(hackrf_transfer* transfer) noexcept -> int;
  void require_success(int result, std::string_view operation) const;
  auto read_identity(std::string serial, hackrf_device* device) const
      -> HackrfIdentity;

  LibhackrfFunctions functions_;
  hackrf_device* device_{};
  HackrfRxCallback receive_callback_;
  bool initialized_{};
  bool rx_started_{};
  std::mutex tx_mutex_;
  std::condition_variable tx_condition_;
  std::vector<std::int8_t> tx_bytes_;
  std::size_t tx_offset_{};
  int tx_flush_result_{};
  bool tx_flushed_{};
  bool tx_started_{};
};

}  // namespace bh61::radio
