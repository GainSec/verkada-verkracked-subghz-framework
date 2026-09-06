#pragma once

#include "bh61/radio/hackrf_types.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bh61::radio {

struct HackrfIdentity {
  std::string serial;
  std::string board_name;
  std::string firmware_version;
  std::string hardware_revision;
};

struct HackrfRealizedConfiguration {
  std::uint32_t sample_rate{};
  std::uint64_t center_frequency_hz{};
  std::uint32_t baseband_filter_hz{};
  std::uint32_t lna_gain_db{};
  std::uint32_t vga_gain_db{};
};

enum class HackrfTransferStatus {
  Samples,
  EndOfStream,
  DeviceRemoved,
  Error,
};

struct HackrfRxTransfer {
  std::span<const std::int8_t> bytes;
  std::uint64_t monotonic_time_ns{};
  HackrfTransferStatus status{HackrfTransferStatus::Samples};
  std::string_view detail;
};

using HackrfRxCallback = std::function<void(const HackrfRxTransfer&)>;

class HackrfTransport {
 public:
  virtual ~HackrfTransport() = default;
  virtual auto enumerate() -> std::vector<HackrfIdentity> = 0;
  virtual void open(std::string_view serial) = 0;
  virtual auto configure(const HackrfConfiguration& configuration)
      -> HackrfRealizedConfiguration = 0;
  virtual void start_rx(HackrfRxCallback callback) = 0;
  virtual void stop_rx() = 0;
  virtual void close() = 0;
  virtual void transmit(std::span<const std::int8_t> bytes) = 0;
};

}  // namespace bh61::radio
