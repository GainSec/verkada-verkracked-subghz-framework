#pragma once

#include "bh61/radio/hackrf_transport.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bh61::test {

class FakeHackrfTransport final : public radio::HackrfTransport {
 public:
  std::vector<radio::HackrfIdentity> attached;
  radio::HackrfRealizedConfiguration realized;
  bool fail_configuration{};
  std::size_t open_calls{};
  std::size_t configure_calls{};
  std::size_t start_rx_calls{};
  std::size_t stop_rx_calls{};
  std::size_t close_calls{};
  std::size_t transmit_calls{};
  std::vector<std::int8_t> transmitted_bytes;
  std::string selected_serial;

  auto enumerate() -> std::vector<radio::HackrfIdentity> override {
    return attached;
  }

  void open(std::string_view serial) override {
    ++open_calls;
    selected_serial = serial;
  }

  auto configure(const radio::HackrfConfiguration& configuration)
      -> radio::HackrfRealizedConfiguration override {
    ++configure_calls;
    if (fail_configuration) {
      throw std::runtime_error("injected configuration failure");
    }
    realized.sample_rate = configuration.sample_rate;
    realized.center_frequency_hz = configuration.center_frequency_hz;
    realized.baseband_filter_hz = configuration.baseband_filter_hz;
    realized.lna_gain_db = configuration.lna_gain_db;
    realized.vga_gain_db = configuration.vga_gain_db;
    return realized;
  }

  void start_rx(radio::HackrfRxCallback callback) override {
    ++start_rx_calls;
    callback_ = std::move(callback);
  }

  void stop_rx() override { ++stop_rx_calls; }

  void close() override { ++close_calls; }

  void transmit(std::span<const std::int8_t> bytes) override {
    ++transmit_calls;
    transmitted_bytes.assign(bytes.begin(), bytes.end());
  }

  void emit(std::span<const std::int8_t> bytes,
            std::uint64_t monotonic_time_ns) {
    callback_(radio::HackrfRxTransfer{bytes, monotonic_time_ns,
                                      radio::HackrfTransferStatus::Samples,
                                      {}});
  }

  void emit_status(radio::HackrfTransferStatus status,
                   std::string_view detail) {
    callback_(radio::HackrfRxTransfer{{}, 0U, status, detail});
  }

 private:
  radio::HackrfRxCallback callback_;
};

}  // namespace bh61::test
