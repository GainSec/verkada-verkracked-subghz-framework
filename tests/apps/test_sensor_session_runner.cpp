#include "bh61/app/sensor_session_runner.hpp"
#include "bh61/core/frame.hpp"
#include "bh61/core/messages.hpp"
#include "bh61/core/session.hpp"
#include "bh61/core/vcmp.hpp"
#include "bh61/core/vmac.hpp"
#include "bh61/dsp/modulator.hpp"
#include "test_harness.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numbers>
#include <thread>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

auto temp_path(const char* label) -> std::filesystem::path {
  static std::uint64_t sequence{};
  auto path = std::filesystem::temp_directory_path() /
      (std::string("bh61-runner-") + label + "-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
       "-" + std::to_string(++sequence));
  std::filesystem::remove_all(path);
  return path;
}

struct Fixture {
  bh61::core::P256KeyPair sensor_keys{bh61::core::generate_p256_keypair()};
  bh61::core::P256KeyPair hub_keys{bh61::core::generate_p256_keypair()};
  bh61::core::SensorIdentity identity{
      "SYN-DOOR-0001",
      {1, 2, 3, 4, 5, 6, 7, 8},
      {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18},
      {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28},
      sensor_keys.public_xy};
  std::array<std::uint8_t, 8> hub_eui{0xa1, 0xa2, 0xa3, 0xa4,
                                      0xa5, 0xa6, 0xa7, 0xa8};
  std::uint16_t pan{0x01ff};
};

auto radio_frame(const bh61::core::VmacFrame& vmac)
    -> std::vector<std::uint8_t> {
  const auto psdu = bh61::core::encode_vmac(vmac);
  return bh61::core::encode_radio_frame({0, psdu, 0, 0});
}

auto plaintext_from_hub(const Fixture& fixture, std::uint8_t type,
                        std::span<const std::uint8_t> body,
                        std::array<std::uint8_t, 8> destination = {})
    -> std::vector<std::uint8_t> {
  if (destination == std::array<std::uint8_t, 8>{}) destination = fixture.identity.eui64;
  const auto vcmp = bh61::core::encode_vcmp(
      {type, 0, 0, std::nullopt, bh61::core::VcmpPlaintext{
          std::vector<std::uint8_t>(body.begin(), body.end())}});
  return radio_frame({0xcc41, 1, fixture.pan, destination, fixture.hub_eui, vcmp});
}

auto join_frames(const Fixture& fixture) -> std::vector<std::vector<std::uint8_t>> {
  bh61::core::JoinResponse response{};
  response.format = 5;
  response.local_public_xy = fixture.hub_keys.public_xy;
  response.remote_public_hash = bh61::core::public_key_hash4(fixture.identity.public_xy);
  response.local_counter = 0x1234;
  response.remote_counter = 0x5678;
  const auto response_body = bh61::core::encode_join_response(response);
  const auto signature_body = bh61::core::encode_join_signature({{0x30, 0x01, 0x00}});
  return {plaintext_from_hub(fixture, 9, response_body),
          plaintext_from_hub(fixture, 10, signature_body)};
}

auto acknowledgement_frame(const Fixture& fixture, std::uint32_t event_id)
    -> std::vector<std::uint8_t> {
  const auto shared = bh61::core::p256_shared_x(fixture.hub_keys.private_scalar,
                                                fixture.identity.public_xy);
  const auto key = bh61::core::derive_peer_key(shared);
  const auto ack = bh61::core::encode_sense_ack_request({event_id});
  const auto rpc = bh61::core::encode_rpc(
      {0x1f, 0x44, static_cast<std::uint8_t>(ack.size()), 0, ack, {}});
  const std::array<std::uint8_t, 16> iv{
      0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  const auto vcmp = bh61::core::encode_vcmp(
      bh61::core::seal_vcmp(1, 1, 0x5678, rpc, key, iv));
  return radio_frame({0xcc41, 2, fixture.pan, fixture.identity.eui64,
                      fixture.hub_eui, vcmp});
}

auto protected_rpc_from_hub(const Fixture& fixture, std::uint8_t message_id,
                            std::uint8_t transaction,
                            std::span<const std::uint8_t> payload,
                            std::uint16_t counter, std::uint8_t sequence,
                            std::uint8_t rpc_flags = 0U,
                            bool request_mac_ack = true)
    -> std::vector<std::uint8_t> {
  const auto shared = bh61::core::p256_shared_x(
      fixture.hub_keys.private_scalar, fixture.identity.public_xy);
  const auto key = bh61::core::derive_peer_key(shared);
  const auto rpc = bh61::core::encode_rpc(
      {message_id, transaction, static_cast<std::uint8_t>(payload.size()),
       rpc_flags,
       std::vector<std::uint8_t>(payload.begin(), payload.end()), {}});
  const std::array<std::uint8_t, 16> iv{
      sequence, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  const auto vcmp = bh61::core::encode_vcmp(
      bh61::core::seal_vcmp(1, 1, counter, rpc, key, iv));
  return radio_frame({static_cast<std::uint16_t>(
                          request_mac_ack ? 0xcc61U : 0xcc41U),
                      sequence, fixture.pan, fixture.identity.eui64,
                      fixture.hub_eui, vcmp});
}

auto decode_protected_rpc_to_hub(const Fixture& fixture,
                                 std::span<const std::uint8_t> frame)
    -> bh61::core::RpcMessage {
  const auto radio = std::get<bh61::core::RadioFrame>(
      bh61::core::parse_radio_frame(frame));
  const auto mac = std::get<bh61::core::VmacFrame>(
      bh61::core::parse_vmac(radio.psdu));
  const auto management = std::get<bh61::core::VcmpFrame>(
      bh61::core::parse_vcmp(mac.payload));
  const auto shared = bh61::core::p256_shared_x(
      fixture.hub_keys.private_scalar, fixture.identity.public_xy);
  const auto key = bh61::core::derive_peer_key(shared);
  const auto opened = std::get<bh61::core::OpenedVcmp>(
      bh61::core::open_vcmp(management, key));
  return std::get<bh61::core::RpcMessage>(
      bh61::core::parse_rpc(opened.payload));
}

class FakeFrameRadio final : public bh61::app::SensorFrameRadio {
 public:
  std::vector<std::vector<std::vector<std::uint8_t>>> receive_batches;
  std::vector<std::vector<std::uint8_t>> transmitted_frames;
  std::vector<std::size_t> transmitted_sample_counts;
  std::vector<std::size_t> receive_limits;
  std::vector<std::chrono::milliseconds> receive_timeouts;
  std::size_t transaction_calls{};
  std::optional<std::filesystem::path> required_file_before_transaction;

  void transmit_frame(std::span<const std::uint8_t> frame,
                      std::span<const std::complex<float>> waveform) override {
    transmitted_frames.emplace_back(frame.begin(), frame.end());
    transmitted_sample_counts.push_back(waveform.size());
  }
  auto receive_frames(std::size_t maximum_frames,
                      std::chrono::milliseconds timeout) -> std::vector<std::vector<std::uint8_t>> override {
    receive_limits.push_back(maximum_frames);
    receive_timeouts.push_back(timeout);
    if (receive_batches.empty()) return {};
    auto result = std::move(receive_batches.front());
    receive_batches.erase(receive_batches.begin());
    if (result.size() > maximum_frames) result.resize(maximum_frames);
    return result;
  }
  auto transact_frame(
      std::span<const std::uint8_t> frame,
      std::span<const std::complex<float>> waveform,
      std::size_t maximum_frames, std::chrono::milliseconds timeout)
      -> std::vector<std::vector<std::uint8_t>> override {
    ++transaction_calls;
    if (required_file_before_transaction) {
      BH61_REQUIRE(std::filesystem::exists(*required_file_before_transaction));
    }
    return bh61::app::SensorFrameRadio::transact_frame(
        frame, waveform, maximum_frames, timeout);
  }
};

class AckGatedJoinRadio final : public bh61::app::SensorFrameRadio {
 public:
  explicit AckGatedJoinRadio(const Fixture& fixture)
      : type9_(join_frames(fixture).front()),
        type10_(join_frames(fixture).back()),
        expected_ack_(bh61::core::encode_mac_ack_frame(1U)) {
    const auto parsed_radio = std::get<bh61::core::RadioFrame>(
        bh61::core::parse_radio_frame(type9_));
    auto parsed_vmac = std::get<bh61::core::VmacFrame>(
        bh61::core::parse_vmac(parsed_radio.psdu));
    parsed_vmac.frame_control |= 0x0020U;
    type9_ = radio_frame(parsed_vmac);
  }

  void transmit_frame(
      std::span<const std::uint8_t> frame,
      std::span<const std::complex<float>> waveform) override {
    transmitted_frames.emplace_back(frame.begin(), frame.end());
    BH61_REQUIRE(!waveform.empty());
    if (transmitted_frames.size() > 1U &&
        transmitted_frames.back() == expected_ack_) {
      ack_received_ = true;
    }
  }

  auto receive_frames(std::size_t maximum_frames,
                      std::chrono::milliseconds timeout)
      -> std::vector<std::vector<std::uint8_t>> override {
    BH61_REQUIRE(maximum_frames > 0U);
    BH61_REQUIRE(timeout.count() > 0);
    if (!type9_delivered_) {
      type9_delivered_ = true;
      return {type9_};
    }
    if (ack_received_ && !type10_delivered_) {
      type10_delivered_ = true;
      return {type10_};
    }
    return {};
  }

  std::vector<std::vector<std::uint8_t>> transmitted_frames;

 private:
  std::vector<std::uint8_t> type9_;
  std::vector<std::uint8_t> type10_;
  std::vector<std::uint8_t> expected_ack_;
  bool type9_delivered_{};
  bool type10_delivered_{};
  bool ack_received_{};
};

class AckGatedHeartbeatRadio final : public bh61::app::SensorFrameRadio {
 public:
  explicit AckGatedHeartbeatRadio(const Fixture& fixture)
      : join_(join_frames(fixture)),
        immediate_(protected_rpc_from_hub(
            fixture, 0x00U, 0x44U,
            std::array<std::uint8_t, 4>{0, 0, 0, 0}, 0x5678U, 0xf2U)),
        config_(protected_rpc_from_hub(
            fixture, 0x13U, 0xc6U,
            std::array<std::uint8_t, 4>{0x00, 0x04, 0x96, 0x00}, 0x5679U,
            0xf3U)),
        sense_ack_(protected_rpc_from_hub(
            fixture, 0x1fU, 0x44U,
            std::array<std::uint8_t, 4>{0x00, 0x00, 0x00, 0x01}, 0x567aU,
            0xf4U)) {}

  void transmit_frame(
      std::span<const std::uint8_t> frame,
      std::span<const std::complex<float>> waveform) override {
    BH61_REQUIRE(!waveform.empty());
    transmitted_frames.emplace_back(frame.begin(), frame.end());
    if (transmitted_frames.back() == bh61::core::encode_mac_ack_frame(0xf2U)) {
      stage_ = 1U;
    } else if (transmitted_frames.back() ==
               bh61::core::encode_mac_ack_frame(0xf3U)) {
      stage_ = 2U;
    } else if (transmitted_frames.back() ==
               bh61::core::encode_mac_ack_frame(0xf4U)) {
      stage_ = 3U;
    }
  }

  auto receive_frames(std::size_t maximum_frames,
                      std::chrono::milliseconds timeout)
      -> std::vector<std::vector<std::uint8_t>> override {
    BH61_REQUIRE(maximum_frames > 0U);
    BH61_REQUIRE(timeout.count() > 0);
    if (!join_delivered_) {
      join_delivered_ = true;
      return join_;
    }
    if (!immediate_delivered_) {
      immediate_delivered_ = true;
      return {immediate_};
    }
    if (stage_ == 1U && !config_delivered_) {
      config_delivered_ = true;
      return {config_};
    }
    if (stage_ == 2U && !sense_ack_delivered_) {
      sense_ack_delivered_ = true;
      return {sense_ack_};
    }
    return {};
  }

  std::vector<std::vector<std::uint8_t>> transmitted_frames;

 private:
  std::vector<std::vector<std::uint8_t>> join_;
  std::vector<std::uint8_t> immediate_;
  std::vector<std::uint8_t> config_;
  std::vector<std::uint8_t> sense_ack_;
  std::size_t stage_{};
  bool join_delivered_{};
  bool immediate_delivered_{};
  bool config_delivered_{};
  bool sense_ack_delivered_{};
};

class OneBlockDevice final : public bh61::radio::Device {
 public:
  explicit OneBlockDevice(std::vector<std::complex<float>> samples)
      : samples_(std::move(samples)) {}

  auto capabilities() const -> bh61::radio::DeviceCapabilities override {
    return {true, true, true, false, false,
            bh61::radio::SampleFormat::ComplexFloat32, 4'000'000U,
            20'000'000U};
  }

  auto receive(std::size_t) -> bh61::radio::SampleBlock override {
    if (delivered_) {
      return {{}, samples_.size(), 0U, false, {},
              bh61::radio::SampleBlockStatus::EndOfStream};
    }
    delivered_ = true;
    return {samples_, 0U, 0U, false, {},
            bh61::radio::SampleBlockStatus::Data};
  }

  void transmit(std::span<const std::complex<float>>, std::uint64_t) override {}

 private:
  std::vector<std::complex<float>> samples_;
  bool delivered_{};
};

class RestartableOneBlockDevice final : public bh61::radio::Device {
 public:
  explicit RestartableOneBlockDevice(
      std::vector<std::complex<float>> samples)
      : samples_(std::move(samples)) {}

  auto capabilities() const -> bh61::radio::DeviceCapabilities override {
    return {true, true, true, true, true,
            bh61::radio::SampleFormat::ComplexFloat32, 4'000'000U,
            20'000'000U};
  }
  void start_receive_stream() override { delivered_ = false; }
  void stop_receive_stream() override {}
  auto receive(std::size_t) -> bh61::radio::SampleBlock override {
    if (delivered_) {
      return {{}, samples_.size(), 0U, false, {},
              bh61::radio::SampleBlockStatus::EndOfStream};
    }
    delivered_ = true;
    return {samples_, 0U, 0U, false, {},
            bh61::radio::SampleBlockStatus::Data};
  }
  void transmit(std::span<const std::complex<float>>,
                std::uint64_t) override {}

 private:
  std::vector<std::complex<float>> samples_;
  bool delivered_{};
};

class QueuedBlockDevice final : public bh61::radio::Device {
 public:
  explicit QueuedBlockDevice(
      std::vector<std::vector<std::complex<float>>> blocks)
      : blocks_(std::move(blocks)) {}

  auto capabilities() const -> bh61::radio::DeviceCapabilities override {
    return {true, true, true, false, false,
            bh61::radio::SampleFormat::ComplexFloat32, 4'000'000U,
            20'000'000U};
  }

  auto receive(std::size_t) -> bh61::radio::SampleBlock override {
    ++receive_count_;
    if (next_ == blocks_.size()) {
      return {{}, 0U, 0U, false, {},
              bh61::radio::SampleBlockStatus::EndOfStream};
    }
    return {blocks_[next_++], 0U, 0U, false, {},
            bh61::radio::SampleBlockStatus::Data};
  }

  void transmit(std::span<const std::complex<float>>, std::uint64_t) override {}

  auto receive_count() const -> std::size_t { return receive_count_; }

 private:
  std::vector<std::vector<std::complex<float>>> blocks_;
  std::size_t next_{};
  std::size_t receive_count_{};
};

class StreamingBlockDevice final : public bh61::radio::Device {
 public:
  explicit StreamingBlockDevice(std::vector<std::complex<float>> samples)
      : samples_(std::move(samples)) {}

  auto capabilities() const -> bh61::radio::DeviceCapabilities override {
    return {true, true, true, true, true,
            bh61::radio::SampleFormat::ComplexFloat32, 4'000'000U,
            20'000'000U};
  }

  void start_receive_stream() override {
    BH61_REQUIRE(!streaming_.exchange(true));
    record("start-rx");
  }

  void stop_receive_stream() override {
    BH61_REQUIRE(streaming_.exchange(false));
    record("stop-rx");
  }

  auto receive(std::size_t) -> bh61::radio::SampleBlock override {
    BH61_REQUIRE(streaming_.load());
    record("receive");
    if (delivered_) {
      end_of_stream_observed_.store(true);
      return {{}, samples_.size(), 0U, false, {},
              bh61::radio::SampleBlockStatus::EndOfStream};
    }
    delivered_ = true;
    return {samples_, 0U, 0U, false, {},
            bh61::radio::SampleBlockStatus::Data};
  }

  void transmit(std::span<const std::complex<float>>,
                std::uint64_t) override {
    for (std::size_t attempt = 0; attempt < 100U; ++attempt) {
      if (end_of_stream_observed_.load()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    BH61_REQUIRE(end_of_stream_observed_.load());
    BH61_REQUIRE(streaming_.load());
    record("transmit");
  }

  std::vector<std::string> operations;

 private:
  void record(std::string operation) {
    const std::lock_guard<std::mutex> lock(operations_mutex_);
    operations.push_back(std::move(operation));
  }

  std::vector<std::complex<float>> samples_;
  std::mutex operations_mutex_;
  std::atomic<bool> end_of_stream_observed_{};
  std::atomic<bool> streaming_{};
  bool delivered_{};
};

class ConcurrentDrainDevice final : public bh61::radio::Device {
 public:
  explicit ConcurrentDrainDevice(std::vector<std::complex<float>> samples)
      : samples_(std::move(samples)) {}

  auto capabilities() const -> bh61::radio::DeviceCapabilities override {
    return {true, true, true, true, true,
            bh61::radio::SampleFormat::ComplexFloat32, 4'000'000U,
            20'000'000U};
  }
  void start_receive_stream() override { streaming_ = true; }
  void stop_receive_stream() override { streaming_ = false; }
  auto receive(std::size_t) -> bh61::radio::SampleBlock override {
    BH61_REQUIRE(streaming_);
    receive_entered_.store(true);
    if (delivered_) {
      return {{}, samples_.size(), 0U, false, {},
              bh61::radio::SampleBlockStatus::EndOfStream};
    }
    delivered_ = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return {samples_, 0U, 0U, false, {},
            bh61::radio::SampleBlockStatus::Data};
  }
  void transmit(std::span<const std::complex<float>>,
                std::uint64_t) override {
    for (std::size_t attempt = 0; attempt < 100U; ++attempt) {
      if (receive_entered_.load()) {
        transmitted_.store(true);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("RX was not drained while TX was active");
  }
  auto transmitted() const -> bool { return transmitted_.load(); }

 private:
  std::vector<std::complex<float>> samples_;
  std::atomic<bool> receive_entered_{};
  std::atomic<bool> transmitted_{};
  bool delivered_{};
  bool streaming_{};
};

class ReactiveAckDevice final : public bh61::radio::Device {
 public:
  ReactiveAckDevice(const Fixture& fixture, std::uint32_t sample_rate,
                    std::chrono::microseconds turnaround,
                    std::size_t requests_before_response = 1U,
                    bool live_capture_impairment = false,
                    std::chrono::milliseconds response_delay =
                        std::chrono::milliseconds(100),
                    bool force_first_ack_stale = true,
                    double impairment_carrier_hz = -75.0)
      : sample_rate_(sample_rate),
        requests_before_response_(requests_before_response),
        force_first_ack_stale_(force_first_ack_stale) {
    auto frames = join_frames(fixture);
    {
      const auto parsed_radio = std::get<bh61::core::RadioFrame>(
          bh61::core::parse_radio_frame(frames.front()));
      auto parsed_vmac = std::get<bh61::core::VmacFrame>(
          bh61::core::parse_vmac(parsed_radio.psdu));
      parsed_vmac.frame_control |= 0x0020U;
      frames.front() = radio_frame(parsed_vmac);
    }
    type9_frame_ = frames.front();
    type10_frame_ = frames.back();
    const auto type9 = bh61::dsp::modulate_efr32_custom_oqpsk(
        type9_frame_, sample_rate_);
    const auto type10 = bh61::dsp::modulate_efr32_custom_oqpsk(
        type10_frame_, sample_rate_);
    const auto leakage_start = sample_rate_ / 50U;
    const auto leakage_samples = sample_rate_ / 200U;
    const auto silence_before =
        static_cast<std::size_t>(response_delay.count()) * sample_rate_ /
        1'000U;
    const auto silence_between = sample_rate_ * 300U / 1'000U;
    type9_start_sample_ = silence_before;
    type9_retry_start_sample_ =
        type9_start_sample_ + type9.samples.size() + silence_between;
    type10_start_sample_ =
        type9_retry_start_sample_ + type9.samples.size() + silence_between;
    if (live_capture_impairment) {
      constexpr std::size_t target_block_offset = 310U;
      type10_start_sample_ +=
          (target_block_offset + 4'000U - type10_start_sample_ % 4'000U) %
          4'000U;
    }
    stream_.resize(type10_start_sample_ + type10.samples.size() +
                   sample_rate_ / 200U);
    std::fill(stream_.begin() + static_cast<std::ptrdiff_t>(leakage_start),
              stream_.begin() + static_cast<std::ptrdiff_t>(leakage_start +
                                                            leakage_samples),
              std::complex<float>{1.0F, 0.0F});
    std::copy(type9.samples.begin(), type9.samples.end(),
              stream_.begin() + static_cast<std::ptrdiff_t>(type9_start_sample_));
    std::copy(type9.samples.begin(), type9.samples.end(),
              stream_.begin() +
                  static_cast<std::ptrdiff_t>(type9_retry_start_sample_));
    std::copy(type10.samples.begin(), type10.samples.end(),
              stream_.begin() + static_cast<std::ptrdiff_t>(type10_start_sample_));
    if (live_capture_impairment) {
      std::uint32_t noise_state = 0x514e17a9U;
      for (std::size_t index = 0U; index < stream_.size(); ++index) {
        const auto angle = 2.0 * std::numbers::pi * impairment_carrier_hz *
                           static_cast<double>(index) /
                           static_cast<double>(sample_rate_);
        stream_[index] *= std::complex<float>{
            static_cast<float>(std::cos(angle)),
            static_cast<float>(std::sin(angle))};
        noise_state = noise_state * 1664525U + 1013904223U;
        const auto real =
            static_cast<float>((noise_state >> 8U) & 0xffffU) / 32768.0F -
            1.0F;
        noise_state = noise_state * 1664525U + 1013904223U;
        const auto imag =
            static_cast<float>((noise_state >> 8U) & 0xffffU) / 32768.0F -
            1.0F;
        stream_[index] += 0.02F * std::complex<float>{real, imag};
      }
    }
    const auto type9_duration_ns = static_cast<std::uint64_t>(
        type9.samples.size()) * 1'000'000'000ULL / sample_rate_;
    expected_ack_time_ns_ =
        base_time_ns_ +
        static_cast<std::uint64_t>(force_first_ack_stale_
                                       ? type9_retry_start_sample_
                                       : type9_start_sample_) *
            1'000'000'000ULL / sample_rate_ +
        type9_duration_ns +
        static_cast<std::uint64_t>(turnaround.count()) * 1'000ULL;
    first_ack_deadline_ns_ =
        base_time_ns_ +
        static_cast<std::uint64_t>(type9_start_sample_) * 1'000'000'000ULL /
            sample_rate_ +
        type9_duration_ns +
        static_cast<std::uint64_t>(turnaround.count()) * 1'000ULL;
  }

  auto capabilities() const -> bh61::radio::DeviceCapabilities override {
    return {true, true, true, true, true,
            bh61::radio::SampleFormat::ComplexFloat32, 4'000'000U,
            20'000'000U};
  }

  void start_receive_stream() override { streaming_ = true; }
  void stop_receive_stream() override { streaming_ = false; }

  auto receive(std::size_t maximum_samples) -> bh61::radio::SampleBlock override {
    BH61_REQUIRE(streaming_);
    for (std::size_t attempt = 0U;
         attempt < 2'000U &&
         request_transmit_count_.load() < requests_before_response_;
         ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    BH61_REQUIRE(request_transmit_count_.load() >= requests_before_response_);
    std::this_thread::sleep_for(force_first_ack_stale_
                                   ? std::chrono::milliseconds(1)
                                   : std::chrono::milliseconds(50));
    if (next_sample_ >= type10_start_sample_ && !ack_scheduled_) {
      return {{}, next_sample_, sample_time(next_sample_), false, {},
              bh61::radio::SampleBlockStatus::Timeout};
    }
    if (next_sample_ >= stream_.size()) {
      return {{}, next_sample_, sample_time(next_sample_), false, {},
              bh61::radio::SampleBlockStatus::EndOfStream};
    }
    const auto count =
        std::min(maximum_samples, stream_.size() - next_sample_);
    std::vector<std::complex<float>> block(
        stream_.begin() + static_cast<std::ptrdiff_t>(next_sample_),
        stream_.begin() + static_cast<std::ptrdiff_t>(next_sample_ + count));
    const auto first = next_sample_;
    next_sample_ += count;
    return {std::move(block), first, sample_time(first), false, {},
            bh61::radio::SampleBlockStatus::Data};
  }

  void transmit(std::span<const std::complex<float>> samples,
                std::uint64_t requested_time_ns) override {
    BH61_REQUIRE(!samples.empty());
    if (requested_time_ns == 0U) {
      BH61_REQUIRE(requested_time_ns == 0U);
      request_transmit_count_.fetch_add(1U);
      return;
    }
    ack_transmit_count_++;
    scheduled_ack_time_ns_ = requested_time_ns;
    scheduled_ack_times_ns_.push_back(requested_time_ns);
    const auto timing_error_ns =
        requested_time_ns > expected_ack_time_ns_
            ? requested_time_ns - expected_ack_time_ns_
            : expected_ack_time_ns_ - requested_time_ns;
    ack_scheduled_ = ack_scheduled_ || timing_error_ns <= 2'000U;
  }

  auto current_time_ns() const -> std::uint64_t override {
    const auto call = current_time_call_count_++;
    if (force_first_ack_stale_ && call == 1U) {
      return first_ack_deadline_ns_ + 1'000'000ULL;
    }
    return sample_time(next_sample_);
  }

  auto expected_ack_time_ns() const -> std::uint64_t {
    return expected_ack_time_ns_;
  }
  auto first_ack_time_ns() const -> std::uint64_t {
    return first_ack_deadline_ns_;
  }
  auto scheduled_ack_time_ns() const -> std::uint64_t {
    return scheduled_ack_time_ns_;
  }
  auto ack_transmit_count() const -> std::size_t { return ack_transmit_count_; }
  auto scheduled_ack_times_ns() const -> const std::vector<std::uint64_t>& {
    return scheduled_ack_times_ns_;
  }
  auto request_transmit_count() const -> std::size_t {
    return request_transmit_count_.load();
  }
  auto expected_frames() const -> std::vector<std::vector<std::uint8_t>> {
    return {type9_frame_, type10_frame_};
  }

 private:
  auto sample_time(std::size_t sample) const -> std::uint64_t {
    return base_time_ns_ + static_cast<std::uint64_t>(sample) *
                               1'000'000'000ULL / sample_rate_;
  }

  static constexpr std::uint64_t base_time_ns_{5'000'000'000ULL};
  std::uint32_t sample_rate_{};
  std::size_t requests_before_response_{};
  std::vector<std::complex<float>> stream_;
  std::vector<std::uint8_t> type9_frame_;
  std::vector<std::uint8_t> type10_frame_;
  std::size_t type9_start_sample_{};
  std::size_t type9_retry_start_sample_{};
  std::size_t type10_start_sample_{};
  std::size_t next_sample_{};
  std::uint64_t expected_ack_time_ns_{};
  std::uint64_t first_ack_deadline_ns_{};
  std::uint64_t scheduled_ack_time_ns_{};
  std::vector<std::uint64_t> scheduled_ack_times_ns_;
  std::size_t ack_transmit_count_{};
  std::atomic<std::size_t> request_transmit_count_{};
  bool streaming_{};
  bool ack_scheduled_{};
  bool force_first_ack_stale_{};
  mutable std::size_t current_time_call_count_{};
};

class ReactiveAckChainDevice final : public bh61::radio::Device {
 public:
  ReactiveAckChainDevice(const Fixture& fixture, std::uint32_t sample_rate,
                         std::chrono::microseconds turnaround,
                         std::size_t copies_per_frame = 2U)
      : sample_rate_(sample_rate) {
    BH61_REQUIRE(copies_per_frame >= 2U);
    BH61_REQUIRE(copies_per_frame % 2U == 0U);
    const std::array<std::array<std::uint8_t, 4>, 3> payloads{{
        {0x00, 0x00, 0x00, 0x00},
        {0x00, 0x04, 0x96, 0x00},
        {0x00, 0x00, 0x00, 0x01},
    }};
    const std::array<std::uint8_t, 3> message_ids{0x00U, 0x13U, 0x1fU};
    const std::array<std::uint8_t, 3> sequences{0xf2U, 0xf3U, 0xf4U};
    const auto silence = sample_rate_ * 300U / 1'000U;
    std::size_t cursor = sample_rate_ / 10U;
    for (std::size_t index = 0U; index < sequences.size(); ++index) {
      frames_.push_back(protected_rpc_from_hub(
          fixture, message_ids[index], 0x44U, payloads[index],
          static_cast<std::uint16_t>(0x5678U + index), sequences[index]));
      const auto waveform = bh61::dsp::modulate_efr32_custom_oqpsk(
          frames_.back(), sample_rate_);
      const auto required_size =
          cursor + copies_per_frame * (waveform.samples.size() + silence);
      if (stream_.size() < required_size) stream_.resize(required_size);
      const auto duration_ns =
          static_cast<std::uint64_t>(waveform.samples.size()) *
          1'000'000'000ULL / sample_rate_;
      for (std::size_t copy = 0U; copy < copies_per_frame; ++copy) {
        const auto start =
            cursor + copy * (waveform.samples.size() + silence);
        std::copy(waveform.samples.begin(), waveform.samples.end(),
                  stream_.begin() + static_cast<std::ptrdiff_t>(start));
        if (copy % 2U == 1U) {
          expected_ack_times_.push_back(
              sample_time(start) + duration_ns +
              static_cast<std::uint64_t>(turnaround.count()) * 1'000ULL);
        }
      }
      cursor = required_size;
    }
  }

  auto capabilities() const -> bh61::radio::DeviceCapabilities override {
    return {true, true, true, true, true,
            bh61::radio::SampleFormat::ComplexFloat32, 4'000'000U,
            20'000'000U};
  }
  void start_receive_stream() override { streaming_ = true; }
  void stop_receive_stream() override { streaming_ = false; }
  auto receive(std::size_t maximum_samples)
      -> bh61::radio::SampleBlock override {
    BH61_REQUIRE(streaming_);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (next_sample_ >= stream_.size()) {
      return {{}, next_sample_, sample_time(next_sample_), false, {},
              bh61::radio::SampleBlockStatus::EndOfStream};
    }
    const auto count =
        std::min(maximum_samples, stream_.size() - next_sample_);
    std::vector<std::complex<float>> block(
        stream_.begin() + static_cast<std::ptrdiff_t>(next_sample_),
        stream_.begin() + static_cast<std::ptrdiff_t>(next_sample_ + count));
    const auto first = next_sample_;
    next_sample_ += count;
    return {std::move(block), first, sample_time(first), false, {},
            bh61::radio::SampleBlockStatus::Data};
  }
  void transmit(std::span<const std::complex<float>> samples,
                std::uint64_t requested_time_ns) override {
    BH61_REQUIRE(!samples.empty());
    if (requested_time_ns == 0U) return;
    scheduled_ack_times_.push_back(requested_time_ns);
  }
  auto current_time_ns() const -> std::uint64_t override {
    return sample_time(next_sample_);
  }
  auto expected_ack_times() const -> const std::vector<std::uint64_t>& {
    return expected_ack_times_;
  }
  auto scheduled_ack_times() const -> const std::vector<std::uint64_t>& {
    return scheduled_ack_times_;
  }
  auto frames() const -> const std::vector<std::vector<std::uint8_t>>& {
    return frames_;
  }

 private:
  auto sample_time(std::size_t sample) const -> std::uint64_t {
    return base_time_ns_ + static_cast<std::uint64_t>(sample) *
                               1'000'000'000ULL / sample_rate_;
  }

  static constexpr std::uint64_t base_time_ns_{9'000'000'000ULL};
  std::uint32_t sample_rate_{};
  std::vector<std::complex<float>> stream_;
  std::vector<std::vector<std::uint8_t>> frames_;
  std::vector<std::uint64_t> expected_ack_times_;
  std::vector<std::uint64_t> scheduled_ack_times_;
  std::size_t next_sample_{};
  bool streaming_{};
};

class ReactiveEvidenceStreamingDevice final : public bh61::radio::Device {
 public:
  explicit ReactiveEvidenceStreamingDevice(std::filesystem::path evidence_path)
      : evidence_path_(std::move(evidence_path)) {}

  auto capabilities() const -> bh61::radio::DeviceCapabilities override {
    return {true, true, true, true, true,
            bh61::radio::SampleFormat::ComplexFloat32, 4'000'000U,
            20'000'000U};
  }

  void start_receive_stream() override { streaming_ = true; }
  void stop_receive_stream() override { streaming_ = false; }

  auto receive(std::size_t maximum_samples) -> bh61::radio::SampleBlock override {
    BH61_REQUIRE(streaming_);
    for (std::size_t attempt = 0U; attempt < 100U && !join_transmitted_;
         ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    BH61_REQUIRE(join_transmitted_);
    if (delivered_) {
      BH61_REQUIRE(std::filesystem::file_size(evidence_path_) ==
                   block_.size() * sizeof(std::complex<float>));
      return {{}, block_.size(), 1'001'000'000ULL, false, {},
              bh61::radio::SampleBlockStatus::EndOfStream};
    }
    delivered_ = true;
    block_.assign(maximum_samples, std::complex<float>{1.0e-5F, 0.0F});
    return {block_, 0U, 1'000'000'000ULL, false, {},
            bh61::radio::SampleBlockStatus::Data};
  }

  void transmit(std::span<const std::complex<float>> samples,
                std::uint64_t requested_time_ns) override {
    BH61_REQUIRE(!samples.empty());
    BH61_REQUIRE(requested_time_ns == 0U);
    join_transmitted_ = true;
  }

  auto current_time_ns() const -> std::uint64_t override {
    return 1'000'000'000ULL;
  }

 private:
  std::filesystem::path evidence_path_;
  std::vector<std::complex<float>> block_;
  std::atomic<bool> join_transmitted_{};
  bool streaming_{};
  bool delivered_{};
};

class EvidenceOrderingDevice final : public bh61::radio::Device {
 public:
  EvidenceOrderingDevice(std::vector<std::complex<float>> samples,
                         std::filesystem::path evidence_path)
      : samples_(std::move(samples)), evidence_path_(std::move(evidence_path)) {}

  auto capabilities() const -> bh61::radio::DeviceCapabilities override {
    return {true, true, false, false, false,
            bh61::radio::SampleFormat::ComplexFloat32, 4'000'000U,
            20'000'000U};
  }
  auto receive(std::size_t) -> bh61::radio::SampleBlock override {
    if (delivered_) {
      BH61_REQUIRE(std::filesystem::file_size(evidence_path_) == 0U);
      return {{}, samples_.size(), 0U, false, {},
              bh61::radio::SampleBlockStatus::EndOfStream};
    }
    delivered_ = true;
    return {samples_, 0U, 0U, false, {},
            bh61::radio::SampleBlockStatus::Data};
  }
  void transmit(std::span<const std::complex<float>>,
                std::uint64_t) override {}

 private:
  std::vector<std::complex<float>> samples_;
  std::filesystem::path evidence_path_;
  bool delivered_{};
};

class DataThenErrorDevice final : public bh61::radio::Device {
 public:
  explicit DataThenErrorDevice(std::vector<std::complex<float>> samples)
      : samples_(std::move(samples)) {}

  auto capabilities() const -> bh61::radio::DeviceCapabilities override {
    return {true, true, false, false, false,
            bh61::radio::SampleFormat::ComplexFloat32, 4'000'000U,
            20'000'000U};
  }
  auto receive(std::size_t) -> bh61::radio::SampleBlock override {
    if (!delivered_) {
      delivered_ = true;
      return {samples_, 0U, 0U, false, {},
              bh61::radio::SampleBlockStatus::Data};
    }
    return {{}, samples_.size(), 0U, true, "synthetic overflow",
            bh61::radio::SampleBlockStatus::Error};
  }
  void transmit(std::span<const std::complex<float>>,
                std::uint64_t) override {}

 private:
  std::vector<std::complex<float>> samples_;
  bool delivered_{};
};

auto runner_config(const Fixture& fixture, const std::filesystem::path& evidence)
    -> bh61::app::SensorRunnerConfig {
  bh61::app::SensorRunnerConfig config;
  config.pan = fixture.pan;
  config.hub_short_address = 1;
  config.sample_rate = 4'000'000;
  config.receive_timeout = std::chrono::milliseconds(100);
  config.maximum_receive_frames = 8;
  config.evidence_directory = evidence;
  config.oracle = [] { return bh61::app::RunnerOracleStatus{true, "healthy"}; };
  config.fixed_event_iv = std::array<std::uint8_t, 16>{
      0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  return config;
}

auto decode_transmitted_event(const Fixture& fixture,
                              std::span<const std::uint8_t> frame)
    -> bh61::core::EventRequest {
  const auto radio = std::get<bh61::core::RadioFrame>(
      bh61::core::parse_radio_frame(frame));
  const auto vmac = std::get<bh61::core::VmacFrame>(
      bh61::core::parse_vmac(radio.psdu));
  const auto vcmp = std::get<bh61::core::VcmpFrame>(
      bh61::core::parse_vcmp(vmac.payload));
  const auto key = bh61::core::derive_peer_key(bh61::core::p256_shared_x(
      fixture.hub_keys.private_scalar, fixture.identity.public_xy));
  const auto opened = std::get<bh61::core::OpenedVcmp>(
      bh61::core::open_vcmp(vcmp, key));
  const auto rpc = std::get<bh61::core::RpcMessage>(
      bh61::core::parse_rpc(opened.payload));
  BH61_REQUIRE(rpc.message_id == 0x03U);
  return std::get<bh61::core::EventRequest>(
      bh61::core::parse_event_request(rpc.payload));
}

}  // namespace

BH61_TEST("generic transaction sends one exact MAC ACK") {
  Fixture fixture;
  FakeFrameRadio radio;
  radio.receive_batches = {{protected_rpc_from_hub(
      fixture, 0x00U, 0x44U, std::array<std::uint8_t, 4>{0, 0, 0, 0},
      0x5678U, 0xfeU, 0x01U)}};
  const std::array<std::uint8_t, 1> request{0x01};
  const std::array<std::complex<float>, 1> waveform{
      std::complex<float>{1.0F, 0.0F}};
  std::size_t callbacks{};
  static_cast<void>(radio.transact_frame_with_mac_ack(
      request, waveform, 8U, std::chrono::milliseconds(20), 4'000'000U,
      fixture.pan, std::chrono::microseconds(514),
      [&](std::span<const std::uint8_t>,
          std::span<const std::complex<float>>, std::uint64_t) { ++callbacks; },
      1U, std::chrono::milliseconds(0)));
  BH61_REQUIRE(callbacks == 1U);
  BH61_REQUIRE(radio.transmitted_frames.size() == 2U);
  BH61_REQUIRE(radio.transmitted_frames.back() ==
               bh61::core::encode_mac_ack_frame(0xfeU));
}

BH61_TEST("device sensor radio decodes every valid burst in one sample block") {
  Fixture fixture;
  const auto expected = join_frames(fixture);
  BH61_REQUIRE(expected.size() == 2U);
  auto first = bh61::dsp::modulate_oqpsk(
      expected[0], 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  auto second = bh61::dsp::modulate_oqpsk(
      expected[1], 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  std::vector<std::complex<float>> samples(4'000U);
  samples.insert(samples.end(), first.samples.begin(), first.samples.end());
  samples.resize(samples.size() + 8'000U);
  samples.insert(samples.end(), second.samples.begin(), second.samples.end());
  samples.resize(samples.size() + 4'000U);

  OneBlockDevice device(std::move(samples));
  bh61::app::DeviceSensorFrameRadio radio(device, 4'000'000U, 1'000'000U);
  const auto received = radio.receive_frames(8U, std::chrono::seconds(5));
  BH61_REQUIRE(received == expected);
}

BH61_TEST("device sensor radio captures bounded blocks before decoding") {
  Fixture fixture;
  const auto expected = join_frames(fixture);
  std::vector<std::vector<std::complex<float>>> blocks;
  for (const auto& frame : expected) {
    auto waveform = bh61::dsp::modulate_oqpsk(
        frame, 4'000'000U, bh61::dsp::OqpskOrientation::EvenChipsOnI,
        bh61::dsp::ChipPolarity::ZeroIsPositive);
    waveform.samples.resize(waveform.samples.size() + 4'000U);
    blocks.push_back(std::move(waveform.samples));
  }
  QueuedBlockDevice device(std::move(blocks));
  bh61::app::DeviceSensorFrameRadio radio(device, 4'000'000U, 1'000'000U);
  const auto received = radio.receive_frames(8U, std::chrono::milliseconds(10));
  BH61_REQUIRE(received == expected);
}

BH61_TEST("device sensor radio derives its block limit from receive timeout") {
  Fixture fixture;
  const auto expected = join_frames(fixture).front();
  std::vector<std::vector<std::complex<float>>> blocks(17U);
  auto waveform = bh61::dsp::modulate_oqpsk(
      expected, 4'000'000U, bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  blocks.push_back(std::move(waveform.samples));
  QueuedBlockDevice device(std::move(blocks));
  bh61::app::DeviceSensorFrameRadio radio(device, 4'000'000U, 1'000'000U);
  const auto received = radio.receive_frames(8U, std::chrono::seconds(5));
  BH61_REQUIRE(received == std::vector<std::vector<std::uint8_t>>{expected});
}

BH61_TEST("device sensor radio applies the measured carrier correction") {
  Fixture fixture;
  const auto expected = join_frames(fixture).front();
  auto waveform = bh61::dsp::modulate_oqpsk(
      expected, 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  waveform.samples.resize(waveform.samples.size() + 4'000U);
  for (std::size_t index = 0; index < waveform.samples.size(); ++index) {
    const auto angle = 2.0 * std::numbers::pi * 100.0 *
                       static_cast<double>(index) / 4'000'000.0;
    waveform.samples[index] *=
        std::complex<float>{static_cast<float>(std::cos(angle)),
                            static_cast<float>(std::sin(angle))};
  }

  OneBlockDevice device(std::move(waveform.samples));
  bh61::app::DeviceSensorFrameRadio radio(device, 4'000'000U, 1'000'000U,
                                           -100.0);
  const auto received = radio.receive_frames(8U, std::chrono::seconds(5));
  BH61_REQUIRE(received == std::vector<std::vector<std::uint8_t>>{expected});
}

BH61_TEST("device sensor radio searches around an imperfect carrier estimate") {
  Fixture fixture;
  const auto expected = join_frames(fixture).front();
  auto waveform = bh61::dsp::modulate_oqpsk(
      expected, 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  waveform.samples.resize(waveform.samples.size() + 4'000U);
  for (std::size_t index = 0; index < waveform.samples.size(); ++index) {
    const auto angle = -2.0 * std::numbers::pi * 50.0 *
                       static_cast<double>(index) / 4'000'000.0;
    waveform.samples[index] *=
        std::complex<float>{static_cast<float>(std::cos(angle)),
                            static_cast<float>(std::sin(angle))};
  }

  OneBlockDevice device(std::move(waveform.samples));
  bh61::app::DeviceSensorFrameRadio radio(device, 4'000'000U, 1'000'000U,
                                           -100.0);
  const auto received = radio.receive_frames(8U, std::chrono::seconds(5));
  BH61_REQUIRE(received == std::vector<std::vector<std::uint8_t>>{expected});
}

BH61_TEST("device sensor radio preserves exact bounded receive IQ") {
  Fixture fixture;
  const auto expected = join_frames(fixture).front();
  auto waveform = bh61::dsp::modulate_oqpsk(
      expected, 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  waveform.samples.resize(waveform.samples.size() + 4'000U);
  const auto expected_bytes = waveform.samples.size() *
                              sizeof(std::complex<float>);
  const auto evidence_path = temp_path("raw-iq.cf32");

  OneBlockDevice device(std::move(waveform.samples));
  bh61::app::DeviceSensorFrameRadio radio(
      device, 4'000'000U, 1'000'000U, 0.0, evidence_path);
  const auto received = radio.receive_frames(8U, std::chrono::seconds(5));
  BH61_REQUIRE(received == std::vector<std::vector<std::uint8_t>>{expected});
  BH61_REQUIRE(std::filesystem::exists(evidence_path));
  BH61_REQUIRE(std::filesystem::file_size(evidence_path) == expected_bytes);

  std::error_code cleanup_error;
  std::filesystem::remove(evidence_path, cleanup_error);
}

BH61_TEST("device sensor radio uses distinct raw IQ files for repeated transactions") {
  Fixture fixture;
  const auto expected = join_frames(fixture).front();
  auto waveform = bh61::dsp::modulate_oqpsk(
      expected, 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  waveform.samples.resize(waveform.samples.size() + 4'000U);
  const auto evidence_path = temp_path("repeated-raw-iq.cf32");
  auto second_path = evidence_path;
  second_path.replace_filename(evidence_path.stem().string() + "-1" +
                               evidence_path.extension().string());
  RestartableOneBlockDevice device(waveform.samples);
  bh61::app::DeviceSensorFrameRadio radio(
      device, 4'000'000U, 1'000'000U, 0.0, evidence_path);
  const std::array<std::uint8_t, 1> request{0x01U};
  const std::array<std::complex<float>, 1> request_waveform{
      std::complex<float>{1.0F, 0.0F}};

  const auto first = radio.transact_frame(
      request, request_waveform, 8U, std::chrono::seconds(5));
  const auto second = radio.transact_frame(
      request, request_waveform, 8U, std::chrono::seconds(5));

  BH61_REQUIRE(first == std::vector<std::vector<std::uint8_t>>{expected});
  BH61_REQUIRE(second == first);
  BH61_REQUIRE(std::filesystem::file_size(evidence_path) > 0U);
  BH61_REQUIRE(std::filesystem::file_size(second_path) > 0U);
  std::filesystem::remove(evidence_path);
  std::filesystem::remove(second_path);
}

BH61_TEST("device sensor radio defers IQ persistence until capture stops") {
  Fixture fixture;
  const auto expected = join_frames(fixture).front();
  auto waveform = bh61::dsp::modulate_oqpsk(
      expected, 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  waveform.samples.resize(waveform.samples.size() + 4'000U);
  const auto evidence_path = temp_path("deferred-raw-iq.cf32");
  EvidenceOrderingDevice device(std::move(waveform.samples), evidence_path);
  bh61::app::DeviceSensorFrameRadio radio(
      device, 4'000'000U, 1'000'000U, 0.0, evidence_path);
  const auto received = radio.receive_frames(8U, std::chrono::seconds(5));
  BH61_REQUIRE(received == std::vector<std::vector<std::uint8_t>>{expected});
  BH61_REQUIRE(std::filesystem::file_size(evidence_path) > 0U);
  std::error_code cleanup_error;
  std::filesystem::remove(evidence_path, cleanup_error);
}

BH61_TEST("device sensor radio preserves captured IQ after receive failure") {
  std::vector<std::complex<float>> samples(4'096U, {0.25F, -0.25F});
  const auto expected_bytes = samples.size() * sizeof(std::complex<float>);
  const auto evidence_path = temp_path("failed-receive.cf32");
  DataThenErrorDevice device(std::move(samples));
  bh61::app::DeviceSensorFrameRadio radio(
      device, 4'000'000U, 40'000U, 0.0, evidence_path);
  bool failed = false;
  try {
    static_cast<void>(
        radio.receive_frames(8U, std::chrono::milliseconds(100)));
  } catch (const std::runtime_error&) {
    failed = true;
  }
  BH61_REQUIRE(failed);
  BH61_REQUIRE(std::filesystem::file_size(evidence_path) == expected_bytes);
  std::error_code cleanup_error;
  std::filesystem::remove(evidence_path, cleanup_error);
}

BH61_TEST("device sensor radio captures the bounded stream before target decode") {
  Fixture fixture;
  const auto expected = join_frames(fixture);
  auto unrelated = expected.front();
  {
    const auto parsed_radio = std::get<bh61::core::RadioFrame>(
        bh61::core::parse_radio_frame(unrelated));
    auto parsed_vmac = std::get<bh61::core::VmacFrame>(
        bh61::core::parse_vmac(parsed_radio.psdu));
    parsed_vmac.destination = std::array<std::uint8_t, 8>{
        0xde, 0xad, 0xbe, 0xef, 0, 0, 0, 1};
    unrelated = radio_frame(parsed_vmac);
  }
  std::vector<std::vector<std::complex<float>>> blocks;
  for (const auto& frame :
       std::vector<std::vector<std::uint8_t>>{unrelated, expected[0],
                                              expected[1], unrelated}) {
    auto waveform = bh61::dsp::modulate_oqpsk(
        frame, 4'000'000U, bh61::dsp::OqpskOrientation::EvenChipsOnI,
        bh61::dsp::ChipPolarity::ZeroIsPositive);
    for (std::size_t index = 0; index < waveform.samples.size(); ++index) {
      const auto angle = 2.0 * std::numbers::pi * 100.0 *
                         static_cast<double>(index) / 4'000'000.0;
      waveform.samples[index] *=
          std::complex<float>{static_cast<float>(std::cos(angle)),
                              static_cast<float>(std::sin(angle))};
    }
    std::vector<std::complex<float>> block(300'000U);
    block.insert(block.end(), waveform.samples.begin(), waveform.samples.end());
    block.resize(1'000'000U);
    blocks.push_back(std::move(block));
  }

  QueuedBlockDevice device(std::move(blocks));
  bh61::app::DeviceSensorFrameRadio radio(
      device, 4'000'000U, 1'000'000U, -100.0, std::nullopt,
      fixture.identity.eui64, 2U);
  const auto received = radio.receive_frames(8U, std::chrono::seconds(5));
  BH61_REQUIRE(received == expected);
  BH61_REQUIRE(device.receive_count() == 5U);
}

BH61_TEST("device sensor transaction arms continuous RX before transmit") {
  Fixture fixture;
  const auto expected = join_frames(fixture).front();
  auto response = bh61::dsp::modulate_oqpsk(
      expected, 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  response.samples.resize(response.samples.size() + 4'000U);
  StreamingBlockDevice device(std::move(response.samples));
  bh61::app::DeviceSensorFrameRadio radio(
      device, 4'000'000U, 1'000'000U, 0.0, std::nullopt,
      fixture.identity.eui64, 1U);
  const std::array<std::uint8_t, 1> request{0x01};
  const std::array<std::complex<float>, 1> waveform{
      std::complex<float>{1.0F, 0.0F}};
  const auto received = radio.transact_frame(
      request, waveform, 8U, std::chrono::milliseconds(100));
  BH61_REQUIRE(received == std::vector<std::vector<std::uint8_t>>{expected});
  BH61_REQUIRE(device.operations.front() == "start-rx");
  BH61_REQUIRE(std::find(device.operations.begin(), device.operations.end(),
                         "transmit") != device.operations.end());
  BH61_REQUIRE(std::find(device.operations.begin(), device.operations.end(),
                         "receive") != device.operations.end());
  BH61_REQUIRE(device.operations.back() == "stop-rx");
}

BH61_TEST("device sensor transaction drains RX concurrently with TX") {
  Fixture fixture;
  const auto expected = join_frames(fixture).front();
  auto response = bh61::dsp::modulate_oqpsk(
      expected, 4'000'000U,
      bh61::dsp::OqpskOrientation::EvenChipsOnI,
      bh61::dsp::ChipPolarity::ZeroIsPositive);
  response.samples.resize(response.samples.size() + 4'000U);
  ConcurrentDrainDevice device(std::move(response.samples));
  bh61::app::DeviceSensorFrameRadio radio(
      device, 4'000'000U, 1'000'000U, 0.0, std::nullopt,
      fixture.identity.eui64, 1U);
  const std::array<std::uint8_t, 1> request{0x01};
  const std::array<std::complex<float>, 1> waveform{
      std::complex<float>{1.0F, 0.0F}};
  const auto received = radio.transact_frame(
      request, waveform, 8U, std::chrono::milliseconds(250));
  BH61_REQUIRE(device.transmitted());
  BH61_REQUIRE(received == std::vector<std::vector<std::uint8_t>>{expected});
}

BH61_TEST("device sensor transaction defers an unschedulable MAC ACK to the next retransmission") {
  Fixture fixture;
  constexpr std::uint32_t sample_rate = 4'000'000U;
  constexpr auto turnaround = std::chrono::microseconds(600);
  ReactiveAckDevice device(fixture, sample_rate, turnaround);
  const auto raw_iq_path = temp_path("reactive-raw-iq.cf32");
  bh61::app::DeviceSensorFrameRadio radio(
      device, sample_rate, sample_rate / 100U, 0.0, raw_iq_path,
      fixture.identity.eui64, 2U);
  const std::array<std::uint8_t, 1> request{0x01};
  const std::array<std::complex<float>, 1> request_waveform{
      std::complex<float>{1.0F, 0.0F}};
  std::vector<std::uint8_t> prepared_ack;
  std::uint64_t prepared_time_ns{};

  const auto received = radio.transact_frame_with_mac_ack(
      request, request_waveform, 8U, std::chrono::milliseconds(3'000),
      sample_rate, fixture.pan, turnaround,
      [&](std::span<const std::uint8_t> frame,
          std::span<const std::complex<float>> waveform,
          std::uint64_t requested_time_ns) {
        prepared_ack.assign(frame.begin(), frame.end());
        BH61_REQUIRE(!waveform.empty());
        prepared_time_ns = requested_time_ns;
      });

  BH61_REQUIRE(prepared_ack == bh61::core::encode_mac_ack_frame(1U));
  const auto timing_error_ns =
      prepared_time_ns > device.expected_ack_time_ns()
          ? prepared_time_ns - device.expected_ack_time_ns()
          : device.expected_ack_time_ns() - prepared_time_ns;
  BH61_REQUIRE(timing_error_ns <= 2'000U);
  BH61_REQUIRE(device.scheduled_ack_time_ns() == prepared_time_ns);
  BH61_REQUIRE(device.ack_transmit_count() == 1U);
  BH61_REQUIRE(!received.empty());
  BH61_REQUIRE(received.at(0) == device.expected_frames().at(0));
  BH61_REQUIRE(received.size() == 2U);
  BH61_REQUIRE(received.at(1) == device.expected_frames().at(1));
  BH61_REQUIRE(std::filesystem::file_size(raw_iq_path) > 0U);
  std::filesystem::remove(raw_iq_path);
}

BH61_TEST("reactive join retries stop after the matching response") {
  Fixture fixture;
  constexpr std::uint32_t sample_rate = 4'000'000U;
  constexpr auto turnaround = std::chrono::microseconds(600);
  constexpr std::size_t requests_before_response = 3U;
  ReactiveAckDevice device(fixture, sample_rate, turnaround,
                           requests_before_response);
  const auto raw_iq_path = temp_path("reactive-retry-raw-iq.cf32");
  bh61::app::DeviceSensorFrameRadio radio(
      device, sample_rate, sample_rate / 100U, 0.0, raw_iq_path,
      fixture.identity.eui64, 2U);
  const std::array<std::uint8_t, 1> request{0x01};
  const std::array<std::complex<float>, 1> request_waveform{
      std::complex<float>{1.0F, 0.0F}};

  const auto received = radio.transact_frame_with_mac_ack(
      request, request_waveform, 8U, std::chrono::milliseconds(3'000),
      sample_rate, fixture.pan, turnaround,
      [](std::span<const std::uint8_t>,
         std::span<const std::complex<float>>, std::uint64_t) {},
      5U, std::chrono::milliseconds(500));

  BH61_REQUIRE(device.request_transmit_count() == requests_before_response);
  BH61_REQUIRE(device.ack_transmit_count() == 1U);
  BH61_REQUIRE(received == device.expected_frames());
  BH61_REQUIRE(std::filesystem::file_size(raw_iq_path) > 0U);
  std::filesystem::remove(raw_iq_path);
}

BH61_TEST("reactive join matches a response inside fifty milliseconds") {
  Fixture fixture;
  constexpr std::uint32_t sample_rate = 4'000'000U;
  constexpr auto turnaround = std::chrono::microseconds(600);
  ReactiveAckDevice device(fixture, sample_rate, turnaround, 1U, false,
                           std::chrono::milliseconds(20));
  bh61::app::DeviceSensorFrameRadio radio(
      device, sample_rate, sample_rate / 100U, 0.0, std::nullopt,
      fixture.identity.eui64, 2U);
  const std::array<std::uint8_t, 1> request{0x01};
  const std::array<std::complex<float>, 1> request_waveform{
      std::complex<float>{1.0F, 0.0F}};

  const auto received = radio.transact_frame_with_mac_ack(
      request, request_waveform, 8U, std::chrono::milliseconds(3'000),
      sample_rate, fixture.pan, turnaround,
      [](std::span<const std::uint8_t>,
         std::span<const std::complex<float>>, std::uint64_t) {});

  BH61_REQUIRE(device.ack_transmit_count() == 1U);
  BH61_REQUIRE(device.scheduled_ack_time_ns() == device.expected_ack_time_ns());
  BH61_REQUIRE(received == device.expected_frames());
}

BH61_TEST("reactive join decodes a live-like type-10 acquisition") {
  Fixture fixture;
  constexpr std::uint32_t sample_rate = 4'000'000U;
  constexpr auto turnaround = std::chrono::microseconds(600);
  ReactiveAckDevice device(fixture, sample_rate, turnaround, 1U, true);
  bh61::app::DeviceSensorFrameRadio radio(
      device, sample_rate, sample_rate / 100U, 0.0, std::nullopt,
      fixture.identity.eui64, 2U);
  const std::array<std::uint8_t, 1> request{0x01};
  const std::array<std::complex<float>, 1> request_waveform{
      std::complex<float>{1.0F, 0.0F}};

  const auto received = radio.transact_frame_with_mac_ack(
      request, request_waveform, 8U, std::chrono::milliseconds(3'000),
      sample_rate, fixture.pan, turnaround,
      [](std::span<const std::uint8_t>,
         std::span<const std::complex<float>>, std::uint64_t) {});

  BH61_REQUIRE(received == device.expected_frames());
}

BH61_TEST("reactive join matches the measured positive carrier offset") {
  Fixture fixture;
  constexpr std::uint32_t sample_rate = 4'000'000U;
  constexpr auto turnaround = std::chrono::microseconds(600);
  ReactiveAckDevice device(fixture, sample_rate, turnaround, 1U, true,
                           std::chrono::milliseconds(100), true, 250.0);
  bh61::app::DeviceSensorFrameRadio radio(
      device, sample_rate, sample_rate / 100U, 0.0, std::nullopt,
      fixture.identity.eui64, 2U);
  const std::array<std::uint8_t, 1> request{0x01};
  const std::array<std::complex<float>, 1> request_waveform{
      std::complex<float>{1.0F, 0.0F}};

  const auto received = radio.transact_frame_with_mac_ack(
      request, request_waveform, 8U, std::chrono::milliseconds(3'000),
      sample_rate, fixture.pan, turnaround,
      [](std::span<const std::uint8_t>,
         std::span<const std::complex<float>>, std::uint64_t) {});

  BH61_REQUIRE(device.ack_transmit_count() == 1U);
  BH61_REQUIRE(!received.empty());
}

BH61_TEST("reactive transaction acknowledges every queued downlink sequence") {
  Fixture fixture;
  constexpr std::uint32_t sample_rate = 4'000'000U;
  constexpr auto turnaround = std::chrono::microseconds(600);
  ReactiveAckChainDevice device(fixture, sample_rate, turnaround);
  bh61::app::DeviceSensorFrameRadio radio(
      device, sample_rate, sample_rate / 100U, 0.0, std::nullopt,
      fixture.identity.eui64, 0U);
  const std::array<std::uint8_t, 1> request{0x01};
  const std::array<std::complex<float>, 1> request_waveform{
      std::complex<float>{1.0F, 0.0F}};
  std::vector<std::vector<std::uint8_t>> prepared_acks;

  const auto received = radio.transact_frame_with_mac_ack(
      request, request_waveform, 8U, std::chrono::milliseconds(4'000),
      sample_rate, fixture.pan, turnaround,
      [&](std::span<const std::uint8_t> frame,
          std::span<const std::complex<float>> waveform, std::uint64_t) {
        BH61_REQUIRE(!waveform.empty());
        prepared_acks.emplace_back(frame.begin(), frame.end());
      });

  BH61_REQUIRE(received == device.frames());
  BH61_REQUIRE(prepared_acks ==
               (std::vector<std::vector<std::uint8_t>>{
                   bh61::core::encode_mac_ack_frame(0xf2U),
                   bh61::core::encode_mac_ack_frame(0xf3U),
                   bh61::core::encode_mac_ack_frame(0xf4U)}));
  BH61_REQUIRE(device.scheduled_ack_times().size() ==
               device.expected_ack_times().size());
  for (std::size_t index = 0U; index < device.expected_ack_times().size();
       ++index) {
    const auto actual = device.scheduled_ack_times()[index];
    const auto expected = device.expected_ack_times()[index];
    const auto error = actual > expected ? actual - expected : expected - actual;
    BH61_REQUIRE(error <= 2'000U);
  }
}

BH61_TEST("reactive transaction re-acknowledges a repeated downlink sequence") {
  Fixture fixture;
  constexpr std::uint32_t sample_rate = 4'000'000U;
  constexpr auto turnaround = std::chrono::microseconds(600);
  ReactiveAckChainDevice device(fixture, sample_rate, turnaround, 4U);
  bh61::app::DeviceSensorFrameRadio radio(
      device, sample_rate, sample_rate / 100U, 0.0, std::nullopt,
      fixture.identity.eui64, 0U);
  const std::array<std::uint8_t, 1> request{0x01};
  const std::array<std::complex<float>, 1> request_waveform{
      std::complex<float>{1.0F, 0.0F}};
  std::vector<std::vector<std::uint8_t>> prepared_acks;

  const auto received = radio.transact_frame_with_mac_ack(
      request, request_waveform, 8U, std::chrono::milliseconds(8'000),
      sample_rate, fixture.pan, turnaround,
      [&](std::span<const std::uint8_t> frame,
          std::span<const std::complex<float>> waveform, std::uint64_t) {
        BH61_REQUIRE(!waveform.empty());
        prepared_acks.emplace_back(frame.begin(), frame.end());
      });

  BH61_REQUIRE(!received.empty());
  BH61_REQUIRE(prepared_acks ==
               (std::vector<std::vector<std::uint8_t>>{
                   bh61::core::encode_mac_ack_frame(0xf2U),
                   bh61::core::encode_mac_ack_frame(0xf2U),
                   bh61::core::encode_mac_ack_frame(0xf3U),
                   bh61::core::encode_mac_ack_frame(0xf3U),
                   bh61::core::encode_mac_ack_frame(0xf4U),
                   bh61::core::encode_mac_ack_frame(0xf4U)}));
  BH61_REQUIRE(device.scheduled_ack_times() ==
               device.expected_ack_times());
}

BH61_TEST("reactive transaction streams raw IQ evidence while receiving") {
  Fixture fixture;
  constexpr std::uint32_t sample_rate = 4'000'000U;
  const auto raw_iq_path = temp_path("reactive-streamed-raw-iq.cf32");
  ReactiveEvidenceStreamingDevice device(raw_iq_path);
  bh61::app::DeviceSensorFrameRadio radio(
      device, sample_rate, sample_rate / 100U, 0.0, raw_iq_path,
      fixture.identity.eui64, 2U);
  const std::array<std::uint8_t, 1> request{0x01};
  const std::array<std::complex<float>, 1> request_waveform{
      std::complex<float>{1.0F, 0.0F}};

  const auto received = radio.transact_frame_with_mac_ack(
      request, request_waveform, 8U, std::chrono::milliseconds(250),
      sample_rate, fixture.pan, std::chrono::microseconds(600),
      [](std::span<const std::uint8_t>,
         std::span<const std::complex<float>>, std::uint64_t) {});

  BH61_REQUIRE(received.empty());
  BH61_REQUIRE(std::filesystem::file_size(raw_iq_path) > 0U);
  std::filesystem::remove(raw_iq_path);
}

BH61_TEST("sensor runner reassembles join derives key and sends a named event") {
  Fixture fixture;
  const auto session_path = temp_path("session");
  const auto evidence_path = temp_path("evidence");
  auto session = bh61::core::SensorSession::create(
      session_path, fixture.identity, fixture.sensor_keys.private_scalar);
  FakeFrameRadio radio;
  auto responses = join_frames(fixture);
  responses.insert(responses.begin(), responses.front());
  auto wrong_pan = responses.front();
  {
    const auto parsed_radio = std::get<bh61::core::RadioFrame>(
        bh61::core::parse_radio_frame(wrong_pan));
    auto parsed_vmac = std::get<bh61::core::VmacFrame>(
        bh61::core::parse_vmac(parsed_radio.psdu));
    parsed_vmac.destination_pan = 0x0200;
    wrong_pan = radio_frame(parsed_vmac);
  }
  responses.insert(responses.begin(), wrong_pan);
  auto wrong_eui = responses.front();
  {
    const auto parsed_radio = std::get<bh61::core::RadioFrame>(
        bh61::core::parse_radio_frame(wrong_eui));
    auto parsed_vmac = std::get<bh61::core::VmacFrame>(
        bh61::core::parse_vmac(parsed_radio.psdu));
    parsed_vmac.destination = std::array<std::uint8_t, 8>{
        0xde, 0xad, 0xbe, 0xef, 0, 0, 0, 1};
    wrong_eui = radio_frame(parsed_vmac);
  }
  responses.insert(responses.begin(), wrong_eui);
  auto self = join_frames(fixture).front();
  const auto parsed_radio = std::get<bh61::core::RadioFrame>(bh61::core::parse_radio_frame(self));
  auto parsed_vmac = std::get<bh61::core::VmacFrame>(bh61::core::parse_vmac(parsed_radio.psdu));
  parsed_vmac.source_eui = fixture.identity.eui64;
  self = radio_frame(parsed_vmac);
  responses.insert(responses.begin(), self);
  radio.receive_batches = {responses, {acknowledgement_frame(fixture, 1)}};

  bh61::app::SensorSessionRunner runner(radio, runner_config(fixture, evidence_path));
  runner.join(session);
  BH61_REQUIRE(session.state() == bh61::core::SensorSessionState::Joined);
  BH61_REQUIRE(radio.transaction_calls == 1U);
  BH61_REQUIRE(session.send_counter() == 0x1234);
  BH61_REQUIRE(session.receive_counter() == 0x5678);
  const auto expected_key = bh61::core::derive_peer_key(
      bh61::core::p256_shared_x(fixture.sensor_keys.private_scalar,
                                fixture.hub_keys.public_xy));
  BH61_REQUIRE(session.key_fingerprint() ==
               bh61::core::session_key_fingerprint(expected_key));
  runner.send_event(session, {4U, true}, 1U, "contact-opened");
  BH61_REQUIRE(session.state() == bh61::core::SensorSessionState::Joined);
  BH61_REQUIRE(radio.transaction_calls == 2U);
  BH61_REQUIRE(radio.transmitted_frames.size() == 2U);
  BH61_REQUIRE(radio.transmitted_sample_counts[0] > 0U);
  BH61_REQUIRE(radio.receive_limits == std::vector<std::size_t>({8U, 8U}));
  BH61_REQUIRE(radio.receive_timeouts ==
               std::vector<std::chrono::milliseconds>({
                   std::chrono::milliseconds(100),
                   std::chrono::milliseconds(100)}));
  const auto event = decode_transmitted_event(fixture,
                                               radio.transmitted_frames[1]);
  BH61_REQUIRE(event.type == 4U);
  BH61_REQUIRE(event.active);
  BH61_REQUIRE(std::filesystem::exists(evidence_path / "join.frame.bin"));
  BH61_REQUIRE(std::filesystem::exists(evidence_path / "join.waveform.cf32"));
  BH61_REQUIRE(
      std::filesystem::exists(evidence_path / "contact-opened.frame.bin"));
  std::filesystem::remove_all(session_path);
  std::filesystem::remove_all(evidence_path);
}

BH61_TEST("sensor runner accepts a matching zero-status EventReq AckRsp") {
  Fixture fixture;
  const auto session_path = temp_path("event-ackrsp-session");
  const auto join_evidence = temp_path("event-ackrsp-join");
  const auto event_evidence = temp_path("event-ackrsp-event");
  const auto second_event_evidence = temp_path("event-ackrsp-second-event");
  auto session = bh61::core::SensorSession::create(
      session_path, fixture.identity, fixture.sensor_keys.private_scalar);
  FakeFrameRadio radio;
  radio.receive_batches = {
      join_frames(fixture),
      {protected_rpc_from_hub(
          fixture, 0x00U, 0x44U,
          std::array<std::uint8_t, 4>{0, 0, 0, 0}, 0x5678U, 0xf2U,
          0x01U)},
      {},
      {protected_rpc_from_hub(
          fixture, 0x00U, 0x44U,
          std::array<std::uint8_t, 4>{0, 0, 0, 0}, 0x5679U, 0xf3U,
          0x01U)},
      {}};

  bh61::app::SensorSessionRunner join_runner(
      radio, runner_config(fixture, join_evidence));
  join_runner.join(session);
  bh61::app::SensorSessionRunner event_runner(
      radio, runner_config(fixture, event_evidence));
  event_runner.send_heartbeat(session, 1U);

  BH61_REQUIRE(session.state() == bh61::core::SensorSessionState::Joined);
  BH61_REQUIRE(session.receive_counter() == 0x5679U);
  BH61_REQUIRE(std::filesystem::exists(
      event_evidence / "heartbeat-ack-response.json"));
  bh61::app::SensorSessionRunner second_event_runner(
      radio, runner_config(fixture, second_event_evidence));
  second_event_runner.send_event(session, {4U, false}, 2U,
                                 "contact-closed");
  BH61_REQUIRE(session.state() == bh61::core::SensorSessionState::Joined);
  BH61_REQUIRE(session.send_counter() == 0x1236U);
  BH61_REQUIRE(session.receive_counter() == 0x567aU);
  BH61_REQUIRE(std::filesystem::exists(
      second_event_evidence / "contact-closed-ack-response.json"));
  std::filesystem::remove_all(session_path);
  std::filesystem::remove_all(join_evidence);
  std::filesystem::remove_all(event_evidence);
  std::filesystem::remove_all(second_event_evidence);
}

BH61_TEST("coordinator read request builder fixes every allowed wire ID and body") {
  using bh61::app::CoordinatorReadOperation;
  struct Case {
    CoordinatorReadOperation operation;
    std::vector<std::uint8_t> echo;
    std::optional<std::uint16_t> selector;
    std::optional<std::uint8_t> index;
    std::uint8_t request_id;
    std::uint8_t response_id;
    std::vector<std::uint8_t> body;
  };
  const std::vector<Case> cases{
      {CoordinatorReadOperation::Echo, {0xc0, 0xff, 0xee}, {}, {}, 0x01,
       0x02, {0xc0, 0xff, 0xee}},
      {CoordinatorReadOperation::ImageState, {}, {}, {}, 0x04, 0x05, {}},
      {CoordinatorReadOperation::RangeStatus, {}, {}, {}, 0x0b, 0x0c, {}},
      {CoordinatorReadOperation::Config, {}, 0x1234, {}, 0x11, 0x12,
       {0x12, 0x34}},
      {CoordinatorReadOperation::Statistics, {}, 0x000b, {}, 0x15, 0x16,
       {0x00, 0x0b}},
      {CoordinatorReadOperation::Peer, {}, {}, 0x1f, 0x21, 0x22, {0x1f}},
      {CoordinatorReadOperation::Mempool, {}, {}, 0x07, 0x23, 0x24, {0x07}}};
  for (const auto& test : cases) {
    const auto request = bh61::app::make_coordinator_read_request(
        test.operation, test.echo, test.selector, test.index, 0x45U);
    BH61_REQUIRE(request.request_id == test.request_id);
    BH61_REQUIRE(request.response_id == test.response_id);
    BH61_REQUIRE(request.transaction == 0x45U);
    BH61_REQUIRE(request.payload == test.body);
  }
}

BH61_TEST("sensor runner validates every recovered read response layout") {
  using bh61::app::CoordinatorReadOperation;
  struct Case {
    CoordinatorReadOperation operation;
    std::vector<std::uint8_t> echo;
    std::optional<std::uint16_t> selector;
    std::optional<std::uint8_t> index;
    std::uint8_t response_id;
    std::vector<std::uint8_t> response;
  };
  const std::vector<Case> cases{
      {CoordinatorReadOperation::Echo, {0xaa}, {}, {}, 0x02, {0xaa}},
      {CoordinatorReadOperation::ImageState, {}, {}, {}, 0x05,
       std::vector<std::uint8_t>(14U)},
      {CoordinatorReadOperation::RangeStatus, {}, {}, {}, 0x0c,
       std::vector<std::uint8_t>(18U)},
      {CoordinatorReadOperation::Config, {}, 0x0007, {}, 0x12,
       {0x00, 0x07, 0x44}},
      {CoordinatorReadOperation::Statistics, {}, 0x000b, {}, 0x16,
       {0x00, 0x00, 0x00, 0x2a}},
      {CoordinatorReadOperation::Peer, {}, {}, 0x00, 0x22,
       std::vector<std::uint8_t>(16U)},
      {CoordinatorReadOperation::Mempool, {}, {}, 0x00, 0x24,
       std::vector<std::uint8_t>(9U)}};

  for (std::size_t i = 0; i < cases.size(); ++i) {
    Fixture fixture;
    const auto session_path = temp_path("rpc-layout-session");
    const auto join_evidence = temp_path("rpc-layout-join");
    const auto rpc_evidence = temp_path("rpc-layout-evidence");
    auto session = bh61::core::SensorSession::create(
        session_path, fixture.identity, fixture.sensor_keys.private_scalar);
    FakeFrameRadio radio;
    radio.receive_batches = {
        join_frames(fixture),
        {protected_rpc_from_hub(fixture, cases[i].response_id, 0x45U,
                                cases[i].response, 0x5678U, 0xf2U)}};
    bh61::app::SensorSessionRunner join_runner(
        radio, runner_config(fixture, join_evidence));
    join_runner.join(session);
    bh61::app::SensorSessionRunner rpc_runner(
        radio, runner_config(fixture, rpc_evidence));
    const auto request = bh61::app::make_coordinator_read_request(
        cases[i].operation, cases[i].echo, cases[i].selector, cases[i].index,
        0x45U);
    const auto result = rpc_runner.read_coordinator(
        session, request, bh61::app::coordinator_read_operation_name(
                              cases[i].operation));
    BH61_REQUIRE(result.has_value());
    BH61_REQUIRE(result->payload == cases[i].response);
    BH61_REQUIRE(session.state() == bh61::core::SensorSessionState::Joined);
    std::filesystem::remove_all(session_path);
    std::filesystem::remove_all(join_evidence);
    std::filesystem::remove_all(rpc_evidence);
  }
}

BH61_TEST("sensor runner sends and validates an authenticated read RPC") {
  Fixture fixture;
  const auto session_path = temp_path("rpc-read-session");
  const auto join_evidence = temp_path("rpc-read-join");
  const auto rpc_evidence = temp_path("rpc-read-evidence");
  auto session = bh61::core::SensorSession::create(
      session_path, fixture.identity, fixture.sensor_keys.private_scalar);
  FakeFrameRadio radio;
  radio.receive_batches = {
      join_frames(fixture),
      {protected_rpc_from_hub(
          fixture, 0x02U, 0x45U,
          std::array<std::uint8_t, 3>{0xc0, 0xff, 0xee}, 0x5678U, 0xf2U)}};

  bh61::app::SensorSessionRunner join_runner(
      radio, runner_config(fixture, join_evidence));
  join_runner.join(session);
  bh61::app::SensorSessionRunner rpc_runner(
      radio, runner_config(fixture, rpc_evidence));
  const auto request = bh61::app::make_coordinator_read_request(
      bh61::app::CoordinatorReadOperation::Echo,
      std::array<std::uint8_t, 3>{0xc0, 0xff, 0xee}, {}, {}, 0x45U);
  const auto result = rpc_runner.read_coordinator(session, request, "echo");

  BH61_REQUIRE(result.has_value());
  BH61_REQUIRE(result->response_id == 0x02U);
  BH61_REQUIRE(result->transaction == 0x45U);
  BH61_REQUIRE(result->counter == 0x5678U);
  BH61_REQUIRE(result->payload ==
               (std::vector<std::uint8_t>{0xc0, 0xff, 0xee}));
  BH61_REQUIRE(session.state() == bh61::core::SensorSessionState::Joined);
  BH61_REQUIRE(session.send_counter() == 0x1235U);
  BH61_REQUIRE(session.receive_counter() == 0x5679U);
  BH61_REQUIRE(radio.transmitted_frames.size() == 3U);
  const auto transmitted =
      decode_protected_rpc_to_hub(fixture, radio.transmitted_frames[1]);
  BH61_REQUIRE(transmitted.message_id == 0x01U);
  BH61_REQUIRE(transmitted.transaction == 0x45U);
  BH61_REQUIRE(transmitted.payload == result->payload);
  BH61_REQUIRE(radio.transmitted_frames.back() ==
               bh61::core::encode_mac_ack_frame(0xf2U));
  BH61_REQUIRE(std::filesystem::exists(rpc_evidence / "echo.frame.bin"));
  BH61_REQUIRE(std::filesystem::exists(rpc_evidence / "echo-response.json"));
  std::filesystem::remove_all(session_path);
  std::filesystem::remove_all(join_evidence);
  std::filesystem::remove_all(rpc_evidence);
}

BH61_TEST("sensor runner rejects a mismatched or malformed read response") {
  Fixture fixture;
  const auto session_path = temp_path("rpc-reject-session");
  const auto join_evidence = temp_path("rpc-reject-join");
  const auto rpc_evidence = temp_path("rpc-reject-evidence");
  auto session = bh61::core::SensorSession::create(
      session_path, fixture.identity, fixture.sensor_keys.private_scalar);
  FakeFrameRadio radio;
  radio.receive_batches = {
      join_frames(fixture),
      {protected_rpc_from_hub(
          fixture, 0x05U, 0x46U,
          std::array<std::uint8_t, 13>{}, 0x5678U, 0xf2U)}};
  bh61::app::SensorSessionRunner join_runner(
      radio, runner_config(fixture, join_evidence));
  join_runner.join(session);
  bh61::app::SensorSessionRunner rpc_runner(
      radio, runner_config(fixture, rpc_evidence));
  const auto request = bh61::app::make_coordinator_read_request(
      bh61::app::CoordinatorReadOperation::ImageState, {}, {}, {}, 0x45U);
  bool rejected = false;
  try {
    static_cast<void>(
        rpc_runner.read_coordinator(session, request, "image-state"));
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  BH61_REQUIRE(rejected);
  BH61_REQUIRE(session.receive_counter() == 0x5678U);
  std::filesystem::remove_all(session_path);
  std::filesystem::remove_all(join_evidence);
  std::filesystem::remove_all(rpc_evidence);
}

BH61_TEST("sensor runner acknowledges type 9 before waiting for type 10") {
  Fixture fixture;
  const auto session_path = temp_path("ack-gated-session");
  const auto evidence_path = temp_path("ack-gated-evidence");
  auto session = bh61::core::SensorSession::create(
      session_path, fixture.identity, fixture.sensor_keys.private_scalar);
  AckGatedJoinRadio radio(fixture);

  bh61::app::SensorSessionRunner runner(
      radio, runner_config(fixture, evidence_path));
  runner.join(session);

  BH61_REQUIRE(session.state() == bh61::core::SensorSessionState::Joined);
  BH61_REQUIRE(radio.transmitted_frames.size() == 2U);
  BH61_REQUIRE(radio.transmitted_frames[1] ==
               bh61::core::encode_mac_ack_frame(1U));
  BH61_REQUIRE(std::filesystem::exists(evidence_path / "join-mac-ack.frame.bin"));
  BH61_REQUIRE(
      std::filesystem::exists(evidence_path / "join-mac-ack.waveform.cf32"));
  BH61_REQUIRE(
      std::filesystem::exists(evidence_path / "join-mac-ack.tx.json"));

  std::filesystem::remove_all(session_path);
  std::filesystem::remove_all(evidence_path);
}

BH61_TEST("sensor runner acknowledges each gated downlink before SenseAck") {
  Fixture fixture;
  const auto session_path = temp_path("heartbeat-ack-gated-session");
  const auto join_evidence = temp_path("heartbeat-ack-gated-join");
  const auto event_evidence = temp_path("heartbeat-ack-gated-event");
  auto session = bh61::core::SensorSession::create(
      session_path, fixture.identity, fixture.sensor_keys.private_scalar);
  AckGatedHeartbeatRadio radio(fixture);

  bh61::app::SensorSessionRunner join_runner(
      radio, runner_config(fixture, join_evidence));
  join_runner.join(session);
  BH61_REQUIRE(session.state() == bh61::core::SensorSessionState::Joined);

  bh61::app::SensorSessionRunner event_runner(
      radio, runner_config(fixture, event_evidence));
  event_runner.send_heartbeat(session, 1U);

  BH61_REQUIRE(session.state() ==
               bh61::core::SensorSessionState::Joined);
  BH61_REQUIRE(std::find(radio.transmitted_frames.begin(),
                         radio.transmitted_frames.end(),
                         bh61::core::encode_mac_ack_frame(0xf2U)) !=
               radio.transmitted_frames.end());
  BH61_REQUIRE(std::find(radio.transmitted_frames.begin(),
                         radio.transmitted_frames.end(),
                         bh61::core::encode_mac_ack_frame(0xf3U)) !=
               radio.transmitted_frames.end());
  BH61_REQUIRE(std::find(radio.transmitted_frames.begin(),
                         radio.transmitted_frames.end(),
                         bh61::core::encode_mac_ack_frame(0xf4U)) !=
               radio.transmitted_frames.end());
  BH61_REQUIRE(std::filesystem::exists(
      event_evidence / "heartbeat-mac-ack-0.frame.bin"));
  BH61_REQUIRE(std::filesystem::exists(
      event_evidence / "heartbeat-mac-ack-1.frame.bin"));
  BH61_REQUIRE(std::filesystem::exists(
      event_evidence / "heartbeat-mac-ack-2.frame.bin"));
  BH61_REQUIRE(std::filesystem::exists(
      event_evidence / "heartbeat-mac-ack-0.tx.json"));
  std::ifstream ack_metadata(
      event_evidence / "heartbeat-mac-ack-0.tx.json");
  std::string ack_metadata_line;
  std::getline(ack_metadata, ack_metadata_line);
  BH61_REQUIRE(ack_metadata_line.find("\"turnaround_us\":514") !=
               std::string::npos);

  std::filesystem::remove_all(session_path);
  std::filesystem::remove_all(join_evidence);
  std::filesystem::remove_all(event_evidence);
}

BH61_TEST("sensor runner does not accept a response after its oracle is lost") {
  Fixture fixture;
  const auto session_path = temp_path("post-rx-oracle-session");
  const auto evidence_path = temp_path("post-rx-oracle-evidence");
  auto session = bh61::core::SensorSession::create(
      session_path, fixture.identity, fixture.sensor_keys.private_scalar);
  FakeFrameRadio radio;
  radio.receive_batches = {join_frames(fixture)};
  auto config = runner_config(fixture, evidence_path);
  auto observations = std::make_shared<std::size_t>(0U);
  config.oracle = [observations] {
    ++*observations;
    return bh61::app::RunnerOracleStatus{
        *observations == 1U, *observations == 1U ? "healthy" : "lost"};
  };
  bh61::app::SensorSessionRunner runner(radio, config);
  bool failed = false;
  try { runner.join(session); } catch (const std::exception&) { failed = true; }
  BH61_REQUIRE(failed);
  BH61_REQUIRE(radio.transmitted_frames.size() == 1U);
  BH61_REQUIRE(session.state() == bh61::core::SensorSessionState::JoinSent);
  BH61_REQUIRE(std::filesystem::exists(evidence_path / "join-rx-0.frame.bin"));
  std::filesystem::remove_all(session_path);
  std::filesystem::remove_all(evidence_path);
}

BH61_TEST("sensor runner preserves evidence on timeout and malformed response") {
  Fixture fixture;
  for (const bool malformed : {false, true}) {
    const auto session_path = temp_path(malformed ? "malformed-session" : "timeout-session");
    const auto evidence_path = temp_path(malformed ? "malformed-evidence" : "timeout-evidence");
    auto session = bh61::core::SensorSession::create(
        session_path, fixture.identity, fixture.sensor_keys.private_scalar);
    FakeFrameRadio radio;
    if (malformed) radio.receive_batches = {{{0x01, 0x02}}};
    bh61::app::SensorSessionRunner runner(radio, runner_config(fixture, evidence_path));
    bool failed = false;
    try { runner.join(session); } catch (const std::exception&) { failed = true; }
    BH61_REQUIRE(failed);
    BH61_REQUIRE(std::filesystem::exists(evidence_path / "join.frame.bin"));
    BH61_REQUIRE(session.state() == bh61::core::SensorSessionState::JoinSent);
    std::filesystem::remove_all(session_path);
    std::filesystem::remove_all(evidence_path);
  }
}

BH61_TEST("sensor runner retries an unanswered join without changing identity") {
  Fixture fixture;
  const auto session_path = temp_path("retry-session");
  const auto first_evidence = temp_path("retry-first-evidence");
  const auto retry_evidence = temp_path("retry-second-evidence");
  auto session = bh61::core::SensorSession::create(
      session_path, fixture.identity, fixture.sensor_keys.private_scalar);
  const auto original_identity = session.identity();
  const auto original_private_scalar = session.private_scalar();
  FakeFrameRadio radio;

  bh61::app::SensorSessionRunner first(
      radio, runner_config(fixture, first_evidence));
  bool first_failed = false;
  try { first.join(session); } catch (const std::exception&) { first_failed = true; }
  BH61_REQUIRE(first_failed);
  BH61_REQUIRE(session.state() == bh61::core::SensorSessionState::JoinSent);

  radio.receive_batches = {join_frames(fixture)};
  bh61::app::SensorSessionRunner retry(
      radio, runner_config(fixture, retry_evidence));
  retry.join(session);
  BH61_REQUIRE(session.state() == bh61::core::SensorSessionState::Joined);
  BH61_REQUIRE(session.identity() == original_identity);
  BH61_REQUIRE(session.private_scalar() == original_private_scalar);
  BH61_REQUIRE(radio.transmitted_frames.size() == 2U);
  BH61_REQUIRE(radio.transmitted_frames[0] == radio.transmitted_frames[1]);
  BH61_REQUIRE(std::filesystem::exists(retry_evidence / "join.frame.bin"));

  std::filesystem::remove_all(session_path);
  std::filesystem::remove_all(first_evidence);
  std::filesystem::remove_all(retry_evidence);
}

BH61_TEST("sensor runner fails closed when the health oracle is lost") {
  Fixture fixture;
  const auto session_path = temp_path("oracle-session");
  const auto evidence_path = temp_path("oracle-evidence");
  auto session = bh61::core::SensorSession::create(
      session_path, fixture.identity, fixture.sensor_keys.private_scalar);
  FakeFrameRadio radio;
  auto config = runner_config(fixture, evidence_path);
  config.oracle = [] { return bh61::app::RunnerOracleStatus{false, "lost"}; };
  bh61::app::SensorSessionRunner runner(radio, config);
  bool failed = false;
  try { runner.join(session); } catch (const std::exception&) { failed = true; }
  BH61_REQUIRE(failed);
  BH61_REQUIRE(radio.transmitted_frames.empty());
  BH61_REQUIRE(session.state() == bh61::core::SensorSessionState::Created);
  std::filesystem::remove_all(session_path);
  std::filesystem::remove_all(evidence_path);
}
