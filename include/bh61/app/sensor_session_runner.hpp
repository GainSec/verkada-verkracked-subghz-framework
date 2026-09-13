#pragma once

#include "bh61/core/messages.hpp"
#include "bh61/core/sensor_session.hpp"
#include "bh61/radio/device.hpp"

#include <array>
#include <chrono>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bh61::app {

struct RunnerOracleStatus {
  bool healthy{};
  std::string detail;
};

class SensorFrameRadio {
 public:
  using MacAckPreparedCallback = std::function<void(
      std::span<const std::uint8_t>,
      std::span<const std::complex<float>>, std::uint64_t)>;

  virtual ~SensorFrameRadio() = default;
  virtual void transmit_frame(
      std::span<const std::uint8_t> frame,
      std::span<const std::complex<float>> waveform) = 0;
  virtual auto receive_frames(std::size_t maximum_frames,
                              std::chrono::milliseconds timeout)
      -> std::vector<std::vector<std::uint8_t>> = 0;
  virtual auto transact_frame(
      std::span<const std::uint8_t> frame,
      std::span<const std::complex<float>> waveform,
      std::size_t maximum_frames, std::chrono::milliseconds timeout)
      -> std::vector<std::vector<std::uint8_t>> {
    transmit_frame(frame, waveform);
    return receive_frames(maximum_frames, timeout);
  }
  virtual auto transact_frame_with_mac_ack(
      std::span<const std::uint8_t> frame,
      std::span<const std::complex<float>> waveform,
      std::size_t maximum_frames, std::chrono::milliseconds timeout,
      std::uint32_t sample_rate, std::uint16_t expected_pan,
      std::chrono::microseconds turnaround,
      const MacAckPreparedCallback& before_ack_transmit,
      std::size_t maximum_request_attempts = 1U,
      std::chrono::milliseconds request_retry_interval =
          std::chrono::milliseconds(0))
      -> std::vector<std::vector<std::uint8_t>>;
};

class DeviceSensorFrameRadio final : public SensorFrameRadio {
 public:
  DeviceSensorFrameRadio(radio::Device& device, std::uint32_t sample_rate,
                         std::size_t samples_per_receive,
                         double carrier_offset_hz = 0.0,
                         std::optional<std::filesystem::path> raw_iq_path =
                             std::nullopt,
                         std::optional<std::array<std::uint8_t, 8>>
                             target_eui = std::nullopt,
                         std::size_t target_frame_count = 0U);

  void transmit_frame(
      std::span<const std::uint8_t> frame,
      std::span<const std::complex<float>> waveform) override;
  auto receive_frames(std::size_t maximum_frames,
                      std::chrono::milliseconds timeout)
      -> std::vector<std::vector<std::uint8_t>> override;
  auto transact_frame(
      std::span<const std::uint8_t> frame,
      std::span<const std::complex<float>> waveform,
      std::size_t maximum_frames, std::chrono::milliseconds timeout)
      -> std::vector<std::vector<std::uint8_t>> override;
  auto transact_frame_with_mac_ack(
      std::span<const std::uint8_t> frame,
      std::span<const std::complex<float>> waveform,
      std::size_t maximum_frames, std::chrono::milliseconds timeout,
      std::uint32_t sample_rate, std::uint16_t expected_pan,
      std::chrono::microseconds turnaround,
      const MacAckPreparedCallback& before_ack_transmit,
      std::size_t maximum_request_attempts = 1U,
      std::chrono::milliseconds request_retry_interval =
          std::chrono::milliseconds(0))
      -> std::vector<std::vector<std::uint8_t>> override;

 private:
  auto receive_frames_impl(std::size_t maximum_frames,
                           std::chrono::milliseconds timeout,
                           const std::function<void()>& before_stream_stop)
      -> std::vector<std::vector<std::uint8_t>>;
  auto next_raw_iq_path() -> std::optional<std::filesystem::path>;

  radio::Device& device_;
  std::uint32_t sample_rate_{};
  std::size_t samples_per_receive_{};
  double carrier_offset_hz_{};
  std::optional<std::filesystem::path> raw_iq_path_;
  std::size_t raw_capture_sequence_{};
  std::optional<std::array<std::uint8_t, 8>> target_eui_;
  std::size_t target_frame_count_{};
  bool continuous_receive_active_{};
};

struct SensorRunnerConfig {
  std::uint16_t pan{0x01ff};
  std::uint16_t hub_short_address{1};
  std::uint32_t sample_rate{4'000'000};
  std::chrono::milliseconds receive_timeout{2'000};
  std::size_t maximum_receive_frames{8};
  std::filesystem::path evidence_directory;
  std::function<RunnerOracleStatus()> oracle;
  std::optional<std::array<std::uint8_t, 16>> fixed_event_iv;
  bool request_uplink_mac_ack{};
  bool dry_run{};
};

enum class CoordinatorReadOperation {
  Echo,
  ImageState,
  RangeStatus,
  Config,
  Statistics,
  Peer,
  Mempool,
};

struct CoordinatorReadRequest {
  CoordinatorReadOperation operation{};
  std::uint8_t request_id{};
  std::uint8_t response_id{};
  std::uint8_t transaction{};
  std::vector<std::uint8_t> payload;
};

struct CoordinatorReadResult {
  CoordinatorReadOperation operation{};
  std::uint8_t response_id{};
  std::uint8_t transaction{};
  std::uint16_t counter{};
  std::vector<std::uint8_t> payload;
};

auto coordinator_read_operation(std::string_view name)
    -> std::optional<CoordinatorReadOperation>;
auto coordinator_read_operation_name(CoordinatorReadOperation operation)
    -> std::string_view;
auto make_coordinator_read_request(
    CoordinatorReadOperation operation,
    std::span<const std::uint8_t> echo_payload = {},
    std::optional<std::uint16_t> selector = std::nullopt,
    std::optional<std::uint8_t> index = std::nullopt,
    std::uint8_t transaction = 0x45U) -> CoordinatorReadRequest;

class SensorSessionRunner {
 public:
  SensorSessionRunner(SensorFrameRadio& radio, SensorRunnerConfig config);

  void join(core::SensorSession& session);
  void send_event(core::SensorSession& session,
                  core::EventSemantic semantic, std::uint32_t event_id,
                  std::string_view evidence_stem);
  void send_heartbeat(core::SensorSession& session, std::uint32_t event_id);
  auto read_coordinator(core::SensorSession& session,
                        const CoordinatorReadRequest& request,
                        std::string_view evidence_stem)
      -> std::optional<CoordinatorReadResult>;
 private:
  void require_healthy(std::string_view stage) const;
  void preserve_received(std::string_view prefix,
                         const std::vector<std::vector<std::uint8_t>>& frames);

  SensorFrameRadio& radio_;
  SensorRunnerConfig config_;
};

}  // namespace bh61::app
