#pragma once

#include "bh61/radio/device.hpp"
#include "bh61/radio/hackrf_transport.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace bh61::radio {

class HackrfDevice final : public Device {
 public:
  HackrfDevice(HackrfTransport& transport, HackrfConfiguration configuration,
               std::chrono::milliseconds receive_timeout);
  ~HackrfDevice() override;

  HackrfDevice(const HackrfDevice&) = delete;
  auto operator=(const HackrfDevice&) -> HackrfDevice& = delete;

  void open();
  void start_receive();
  void stop();
  void close();

  auto identity() const -> const HackrfIdentity&;
  auto realized_configuration() const
      -> const HackrfRealizedConfiguration&;

  auto capabilities() const -> DeviceCapabilities override;
  auto receive(std::size_t maximum_samples) -> SampleBlock override;
  void transmit(std::span<const std::complex<float>> samples,
                std::uint64_t requested_time_ns) override;

 private:
  struct RawBlock {
    std::vector<std::int8_t> bytes;
    std::uint64_t first_sample_index{};
    std::uint64_t monotonic_time_ns{};
    std::size_t consumed_samples{};
    bool discontinuity{};
    std::string discontinuity_reason;
  };

  void handle_transfer(const HackrfRxTransfer& transfer);
  auto terminal_block() const -> SampleBlock;

  HackrfTransport& transport_;
  HackrfConfiguration configuration_;
  std::chrono::milliseconds receive_timeout_;
  HackrfIdentity identity_;
  HackrfRealizedConfiguration realized_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<RawBlock> queue_;
  std::uint64_t next_sample_index_{};
  SampleBlockStatus terminal_status_{SampleBlockStatus::EndOfStream};
  std::string terminal_reason_;
  bool terminal_pending_{};
  bool opened_{};
  bool receiving_{};
  bool cancelled_{};
};

}  // namespace bh61::radio
