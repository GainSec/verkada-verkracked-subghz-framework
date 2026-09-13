#include "bh61/app/sensor_session_runner.hpp"

#include "bh61/core/bytes.hpp"
#include "bh61/core/frame.hpp"
#include "bh61/core/messages.hpp"
#include "bh61/core/p256.hpp"
#include "bh61/core/session.hpp"
#include "bh61/core/vcmp.hpp"
#include "bh61/core/vmac.hpp"
#include "bh61/dsp/acquisition.hpp"
#include "bh61/dsp/demodulator.hpp"
#include "bh61/dsp/modulator.hpp"
#include "bh61/dsp/profile.hpp"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace bh61::app {
namespace {

constexpr std::uint8_t kJoinType = 0x07;
constexpr std::uint8_t kJoinResponseType = 0x09;
constexpr std::uint8_t kJoinSignatureType = 0x0a;
constexpr std::uint8_t kEncryptedManagementType = 0x01;
constexpr std::uint8_t kAckResponseId = 0x00;
constexpr std::uint8_t kEventRequestId = 0x03;
constexpr std::uint8_t kSenseAckRequestId = 0x1f;
constexpr std::uint8_t kHeartbeatEventType = 0x06;
constexpr std::uint8_t kEventIdTlv = 0x01;
constexpr std::uint16_t kShortDestinationFcf = 0xc841;
constexpr std::uint16_t kMacAckRequestFlag = 0x0020;
constexpr auto kMacAckTurnaround = std::chrono::microseconds(514);
struct MatchedJoinAckPrefix {
  std::uint8_t phr{};
  std::uint8_t sequence{};
  std::size_t start_sample{};
  double score{};
};

auto match_join_ack_prefix(
    std::span<const std::complex<float>> samples, std::uint32_t sample_rate,
    double carrier_offset_hz, std::uint16_t expected_pan,
    const std::array<std::uint8_t, 8>& target_eui)
    -> std::optional<MatchedJoinAckPrefix> {
  dsp::DecodeOptions options;
  options.carrier_hypotheses_hz = {
      carrier_offset_hz, carrier_offset_hz + 250.0,
      carrier_offset_hz - 250.0};
  options.coarse_start_stride = 32U;
  options.coarse_template_stride = 64U;
  options.maximum_coarse_regions = 1U;
  options.maximum_symbol_distance = 12U;
  options.maximum_sample_clock_correction_ppm = 0.0;
  const auto decoded =
      dsp::decode_frame_prefix(samples, sample_rate, options, 5U);
  const auto* prefix = std::get_if<dsp::DecodedFramePrefix>(&decoded);
  if (prefix == nullptr) return std::nullopt;
  const auto& bytes = prefix->psdu_prefix;
  const auto frame_control = static_cast<std::uint16_t>(
      bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8U));
  const auto pan = static_cast<std::uint16_t>(
      bytes[3] | (static_cast<std::uint16_t>(bytes[4]) << 8U));
  if (frame_control != 0xcc61U || pan != expected_pan) return std::nullopt;

  std::vector<std::uint8_t> expected{
      prefix->phr, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]};
  expected.insert(expected.end(), target_eui.begin(), target_eui.end());
  const auto waveform =
      dsp::modulate_efr32_custom_oqpsk(expected, sample_rate);
  double template_energy = 0.0;
  for (const auto sample : waveform.samples) {
    template_energy += std::norm(static_cast<std::complex<double>>(sample));
  }

  std::vector<std::complex<double>> corrected_samples;
  corrected_samples.reserve(samples.size());
  for (std::size_t absolute = 0U; absolute < samples.size(); ++absolute) {
    auto observed = static_cast<std::complex<double>>(samples[absolute]);
    if (prefix->acquisition.conjugated) observed = std::conj(observed);
    const auto angle = 2.0 * std::numbers::pi *
                       prefix->acquisition.carrier_offset_hz *
                       static_cast<double>(absolute) /
                       static_cast<double>(sample_rate);
    corrected_samples.push_back(
        observed * std::complex<double>{std::cos(angle), std::sin(angle)});
  }

  std::size_t winner_start{};
  double winner_score = -std::numeric_limits<double>::infinity();
  constexpr std::int32_t kTimingRadiusSamples = 4;
  for (std::int32_t timing = -kTimingRadiusSamples;
       timing <= kTimingRadiusSamples; ++timing) {
      const auto signed_start =
          static_cast<std::int64_t>(prefix->acquisition.start_sample) + timing;
      if (signed_start < 0 ||
          static_cast<std::uint64_t>(signed_start) + waveform.samples.size() >
              samples.size()) {
        continue;
      }
      std::complex<double> correlation{};
      double observed_energy = 0.0;
      for (std::size_t index = 0U; index < waveform.samples.size(); ++index) {
        const auto absolute = static_cast<std::size_t>(signed_start) + index;
        const auto observed = corrected_samples[absolute];
        correlation +=
            std::conj(static_cast<std::complex<double>>(waveform.samples[index])) *
            observed;
        observed_energy += std::norm(observed);
      }
      if (observed_energy <= std::numeric_limits<double>::epsilon() ||
          template_energy <= std::numeric_limits<double>::epsilon()) {
        continue;
      }
      const auto score = std::abs(correlation) /
                         std::sqrt(observed_energy * template_energy);
      if (score > winner_score) {
        winner_score = score;
        winner_start = static_cast<std::size_t>(signed_start);
      }
  }
  constexpr double kMinimumMatchScore = 0.60;
  if (winner_score < kMinimumMatchScore) return std::nullopt;
  return MatchedJoinAckPrefix{prefix->phr, bytes[2], winner_start,
                              winner_score};
}

void write_all(int descriptor, std::span<const std::uint8_t> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = ::write(descriptor, bytes.data() + offset,
                               bytes.size() - offset);
    if (count < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "write runner evidence");
    }
    offset += static_cast<std::size_t>(count);
  }
}

void write_exclusive(const std::filesystem::path& path,
                     std::span<const std::uint8_t> bytes) {
  const int descriptor =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "create runner evidence");
  }
  try {
    write_all(descriptor, bytes);
    if (::fsync(descriptor) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "fsync runner evidence");
    }
    if (::close(descriptor) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "close runner evidence");
    }
  } catch (...) {
    (void)::close(descriptor);
    throw;
  }
}

void write_waveform(const std::filesystem::path& path,
                    std::span<const std::complex<float>> samples) {
  std::vector<std::uint8_t> bytes;
  bytes.resize(samples.size() * sizeof(float) * 2U);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const std::array<float, 2> iq{samples[index].real(), samples[index].imag()};
    std::memcpy(bytes.data() + index * sizeof(iq), iq.data(), sizeof(iq));
  }
  write_exclusive(path, bytes);
}

auto encode_radio(const core::VmacFrame& vmac) -> std::vector<std::uint8_t> {
  return core::encode_radio_frame({0, core::encode_vmac(vmac), 0, 0});
}

auto plaintext_body(const core::VcmpFrame& frame)
    -> const std::vector<std::uint8_t>* {
  return std::get_if<core::VcmpPlaintext>(&frame.body) == nullptr
             ? nullptr
             : &std::get<core::VcmpPlaintext>(frame.body).bytes;
}

auto exact_sensor_destination(const core::VmacFrame& frame,
                              const std::array<std::uint8_t, 8>& sensor_eui)
    -> bool {
  const auto* destination =
      std::get_if<std::array<std::uint8_t, 8>>(&frame.destination);
  return destination != nullptr && *destination == sensor_eui;
}

auto hex_bytes(std::span<const std::uint8_t> bytes) -> std::string {
  constexpr char alphabet[] = "0123456789abcdef";
  std::string encoded(bytes.size() * 2U, '0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    encoded[index * 2U] = alphabet[bytes[index] >> 4U];
    encoded[index * 2U + 1U] = alphabet[bytes[index] & 0x0fU];
  }
  return encoded;
}

auto be16_at(std::span<const std::uint8_t> bytes, std::size_t offset)
    -> std::uint16_t {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(bytes[offset]) << 8U) |
      bytes[offset + 1U]);
}

auto be32_at(std::span<const std::uint8_t> bytes, std::size_t offset)
    -> std::uint32_t {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
         bytes[offset + 3U];
}

void validate_read_response(const CoordinatorReadRequest& request,
                            std::span<const std::uint8_t> payload) {
  switch (request.operation) {
    case CoordinatorReadOperation::Echo:
      if (!std::equal(payload.begin(), payload.end(), request.payload.begin(),
                      request.payload.end())) {
        throw std::runtime_error("EchoRsp does not match EchoReq data");
      }
      return;
    case CoordinatorReadOperation::ImageState:
      if (payload.size() != 14U) {
        throw std::runtime_error("ImageStateRsp must contain exactly 14 bytes");
      }
      return;
    case CoordinatorReadOperation::RangeStatus:
      if (payload.size() != 18U) {
        throw std::runtime_error(
            "RangeStatusCoordRsp must contain exactly 18 bytes");
      }
      return;
    case CoordinatorReadOperation::Config:
      if (payload.size() < 2U || request.payload.size() != 2U ||
          payload[0] != request.payload[0] ||
          payload[1] != request.payload[1]) {
        throw std::runtime_error("CfgGetRsp does not match the requested ID");
      }
      return;
    case CoordinatorReadOperation::Statistics:
      if (payload.empty() || payload.size() % 4U != 0U) {
        throw std::runtime_error(
            "StatReadRsp must contain complete 32-bit counters");
      }
      return;
    case CoordinatorReadOperation::Peer:
      if (payload.size() < 16U || payload.size() != 16U + payload[15]) {
        throw std::runtime_error("PeerReadRsp serial length is invalid");
      }
      return;
    case CoordinatorReadOperation::Mempool:
      if (payload.size() < 9U || payload.size() != 9U + payload[8]) {
        throw std::runtime_error("MempoolReadRsp name length is invalid");
      }
      return;
  }
  throw std::logic_error("unknown coordinator read operation");
}

auto read_response_json(const CoordinatorReadResult& result) -> std::string {
  const auto& payload = result.payload;
  std::string fields;
  switch (result.operation) {
    case CoordinatorReadOperation::Echo:
      fields = ",\"data_hex\":\"" + hex_bytes(payload) + "\"";
      break;
    case CoordinatorReadOperation::ImageState:
      fields = ",\"running_version\":" + std::to_string(be32_at(payload, 0)) +
               ",\"idle_version\":" + std::to_string(be32_at(payload, 4)) +
               ",\"boot_version\":" + std::to_string(be32_at(payload, 8)) +
               ",\"running_flags\":" + std::to_string(payload[12]) +
               ",\"idle_flags\":" + std::to_string(payload[13]);
      break;
    case CoordinatorReadOperation::RangeStatus:
      fields = ",\"target_eui64_hex\":\"" +
               hex_bytes(std::span<const std::uint8_t>(payload).first(8U)) +
               "\",\"events_received\":" +
               std::to_string(be16_at(payload, 8)) +
               ",\"rssi_hex\":\"" +
               hex_bytes(std::span<const std::uint8_t>(payload).subspan(10U)) +
               "\"";
      break;
    case CoordinatorReadOperation::Config:
      fields = ",\"config_id\":" + std::to_string(be16_at(payload, 0)) +
               ",\"value_hex\":\"" +
               hex_bytes(std::span<const std::uint8_t>(payload).subspan(2U)) +
               "\"";
      break;
    case CoordinatorReadOperation::Statistics: {
      fields = ",\"counters\":[";
      for (std::size_t offset = 0; offset < payload.size(); offset += 4U) {
        if (offset != 0U) fields += ',';
        fields += std::to_string(be32_at(payload, offset));
      }
      fields += ']';
      break;
    }
    case CoordinatorReadOperation::Peer:
      fields = ",\"eui64_hex\":\"" +
               hex_bytes(std::span<const std::uint8_t>(payload).first(8U)) +
               "\",\"seconds_until_expiry\":" +
               std::to_string(be32_at(payload, 8)) +
               ",\"join_version\":" + std::to_string(payload[12]) +
               ",\"bsp_id\":" + std::to_string(payload[13]) +
               ",\"state\":" + std::to_string(payload[14]) +
               ",\"serial_hex\":\"" +
               hex_bytes(std::span<const std::uint8_t>(payload).subspan(16U)) +
               "\"";
      break;
    case CoordinatorReadOperation::Mempool:
      fields = ",\"block_size\":" + std::to_string(be16_at(payload, 0)) +
               ",\"num_blocks\":" + std::to_string(be16_at(payload, 2)) +
               ",\"num_free\":" + std::to_string(be16_at(payload, 4)) +
               ",\"min_free\":" + std::to_string(be16_at(payload, 6)) +
               ",\"name_hex\":\"" +
               hex_bytes(std::span<const std::uint8_t>(payload).subspan(9U)) +
               "\"";
      break;
  }
  return "{\"schema\":\"bh61.coordinator-read-response/v1\",\"operation\":\"" +
         std::string(coordinator_read_operation_name(result.operation)) +
         "\",\"response_id\":" + std::to_string(result.response_id) +
         ",\"transaction\":" + std::to_string(result.transaction) +
         ",\"counter\":" + std::to_string(result.counter) +
         ",\"payload_hex\":\"" + hex_bytes(payload) + "\"" + fields +
         "}\n";
}

auto event_iv(const SensorRunnerConfig& config)
    -> std::array<std::uint8_t, 16> {
  if (config.fixed_event_iv) return *config.fixed_event_iv;
  std::array<std::uint8_t, 16> iv{};
  if (RAND_bytes(iv.data(), static_cast<int>(iv.size())) != 1) {
    throw std::runtime_error("operating-system random IV generation failed");
  }
  return iv;
}

struct ActiveSampleRange {
  std::size_t begin{};
  std::size_t end{};
  double peak_power{};
};

auto active_sample_ranges(std::span<const std::complex<float>> samples,
                          std::uint32_t sample_rate)
    -> std::vector<ActiveSampleRange> {
  if (samples.empty()) return {};
  const auto window_samples =
      std::max<std::size_t>(256U, sample_rate / 4'000U);
  const auto window_count =
      (samples.size() + window_samples - 1U) / window_samples;
  std::vector<double> powers(window_count, 0.0);
  for (std::size_t window = 0; window < window_count; ++window) {
    const auto begin = window * window_samples;
    const auto end = std::min(samples.size(), begin + window_samples);
    double sum{};
    for (std::size_t index = begin; index < end; ++index) {
      sum += std::norm(samples[index]);
    }
    powers[window] = sum / static_cast<double>(end - begin);
  }

  auto sorted = powers;
  const auto middle = sorted.begin() +
                      static_cast<std::ptrdiff_t>(sorted.size() / 2U);
  std::nth_element(sorted.begin(), middle, sorted.end());
  const auto noise_floor = *middle;
  const auto peak = *std::max_element(powers.begin(), powers.end());
  if (peak <= std::numeric_limits<double>::epsilon()) {
    return {};
  }
  if (noise_floor > 0.0 && peak < noise_floor * 4.0) {
    if (samples.size() <= sample_rate / 10U) {
      return {{0U, samples.size(), peak}};
    }
    return {};
  }
  const auto threshold = noise_floor + (peak - noise_floor) * 0.05;
  const auto margin = std::max<std::size_t>(window_samples,
                                            sample_rate / 100U);
  std::vector<ActiveSampleRange> ranges;
  std::size_t window = 0U;
  while (window < powers.size()) {
    if (powers[window] < threshold) {
      ++window;
      continue;
    }
    const auto first = window;
    auto range_peak = powers[window];
    while (window + 1U < powers.size() &&
           powers[window + 1U] >= threshold) {
      ++window;
      range_peak = std::max(range_peak, powers[window]);
    }
    const auto last = window;
    const auto unpadded_begin = first * window_samples;
    const auto unpadded_end =
        std::min(samples.size(), (last + 1U) * window_samples);
    const auto begin = unpadded_begin > margin ? unpadded_begin - margin : 0U;
    const auto end = std::min(samples.size(), unpadded_end + margin);
    if (!ranges.empty() && begin <= ranges.back().end) {
      ranges.back().end = std::max(ranges.back().end, end);
      ranges.back().peak_power =
          std::max(ranges.back().peak_power, range_peak);
    } else {
      ranges.push_back({begin, end, range_peak});
    }
    ++window;
  }
  constexpr std::size_t kMaximumActiveRanges = 8U;
  if (ranges.size() > kMaximumActiveRanges) {
    std::partial_sort(
        ranges.begin(), ranges.begin() + kMaximumActiveRanges, ranges.end(),
        [](const ActiveSampleRange& left, const ActiveSampleRange& right) {
          return left.peak_power > right.peak_power;
        });
    ranges.resize(kMaximumActiveRanges);
    std::sort(ranges.begin(), ranges.end(),
              [](const ActiveSampleRange& left,
                 const ActiveSampleRange& right) {
                return left.begin < right.begin;
              });
  }
  return ranges;
}

auto frame_matches_target(
    std::span<const std::uint8_t> frame,
    const std::optional<std::array<std::uint8_t, 8>>& target_eui) -> bool {
  if (!target_eui) return true;
  const auto parsed_radio = core::parse_radio_frame(frame);
  const auto* radio_frame = std::get_if<core::RadioFrame>(&parsed_radio);
  if (radio_frame == nullptr) return false;
  const auto parsed_vmac = core::parse_vmac(radio_frame->psdu);
  const auto* mac = std::get_if<core::VmacFrame>(&parsed_vmac);
  if (mac == nullptr) return false;
  const auto* destination =
      std::get_if<std::array<std::uint8_t, 8>>(&mac->destination);
  return destination != nullptr && *destination == *target_eui;
}

auto requested_ack_sequence(std::span<const std::uint8_t> frame)
    -> std::optional<std::uint8_t> {
  const auto parsed_radio = core::parse_radio_frame(frame);
  const auto* radio_frame = std::get_if<core::RadioFrame>(&parsed_radio);
  if (radio_frame == nullptr) return std::nullopt;
  const auto parsed_vmac = core::parse_vmac(radio_frame->psdu);
  const auto* mac = std::get_if<core::VmacFrame>(&parsed_vmac);
  if (mac == nullptr || (mac->frame_control & 0x0020U) == 0U) {
    return std::nullopt;
  }
  return mac->sequence;
}

auto is_join_signature_frame(std::span<const std::uint8_t> frame) -> bool {
  const auto parsed_radio = core::parse_radio_frame(frame);
  const auto* radio_frame = std::get_if<core::RadioFrame>(&parsed_radio);
  if (radio_frame == nullptr) return false;
  const auto parsed_vmac = core::parse_vmac(radio_frame->psdu);
  const auto* mac = std::get_if<core::VmacFrame>(&parsed_vmac);
  if (mac == nullptr) return false;
  const auto parsed_vcmp = core::parse_vcmp(mac->payload);
  const auto* management = std::get_if<core::VcmpFrame>(&parsed_vcmp);
  return management != nullptr && management->flags == 0U &&
         management->type == kJoinSignatureType;
}

auto fine_decode_options(double center_hz) -> dsp::DecodeOptions {
  dsp::DecodeOptions options;
  options.maximum_symbol_distance = 16U;
  options.carrier_hypotheses_hz = {center_hz};
  constexpr double kStepHz = 25.0;
  constexpr double kSpanHz = 250.0;
  for (double delta = kStepHz; delta <= kSpanHz; delta += kStepHz) {
    options.carrier_hypotheses_hz.push_back(center_hz + delta);
    options.carrier_hypotheses_hz.push_back(center_hz - delta);
  }
  options.coarse_start_stride = 8U;
  options.coarse_template_stride = 32U;
  options.maximum_sample_clock_correction_ppm = 0.0;
  return options;
}

auto average_power(std::span<const std::complex<float>> samples) -> double {
  if (samples.empty()) return 0.0;
  double sum{};
  for (const auto sample : samples) sum += std::norm(sample);
  return sum / static_cast<double>(samples.size());
}

auto median_power(const std::deque<double>& powers) -> double {
  if (powers.empty()) return 0.0;
  std::vector<double> sorted(powers.begin(), powers.end());
  const auto middle = sorted.begin() +
                      static_cast<std::ptrdiff_t>(sorted.size() / 2U);
  std::nth_element(sorted.begin(), middle, sorted.end());
  return *middle;
}

auto first_active_sample(std::span<const std::complex<float>> samples,
                         std::uint32_t sample_rate, double threshold)
    -> std::size_t {
  const auto window = std::max<std::size_t>(16U, sample_rate / 100'000U);
  for (std::size_t begin = 0U; begin < samples.size(); begin += window) {
    const auto end = std::min(samples.size(), begin + window);
    if (average_power(samples.subspan(begin, end - begin)) > threshold) {
      return begin;
    }
  }
  return 0U;
}

}  // namespace

auto coordinator_read_operation(std::string_view name)
    -> std::optional<CoordinatorReadOperation> {
  if (name == "echo") return CoordinatorReadOperation::Echo;
  if (name == "image-state") return CoordinatorReadOperation::ImageState;
  if (name == "range-status") return CoordinatorReadOperation::RangeStatus;
  if (name == "config") return CoordinatorReadOperation::Config;
  if (name == "statistics") return CoordinatorReadOperation::Statistics;
  if (name == "peer") return CoordinatorReadOperation::Peer;
  if (name == "mempool") return CoordinatorReadOperation::Mempool;
  return std::nullopt;
}

auto coordinator_read_operation_name(CoordinatorReadOperation operation)
    -> std::string_view {
  switch (operation) {
    case CoordinatorReadOperation::Echo: return "echo";
    case CoordinatorReadOperation::ImageState: return "image-state";
    case CoordinatorReadOperation::RangeStatus: return "range-status";
    case CoordinatorReadOperation::Config: return "config";
    case CoordinatorReadOperation::Statistics: return "statistics";
    case CoordinatorReadOperation::Peer: return "peer";
    case CoordinatorReadOperation::Mempool: return "mempool";
  }
  throw std::logic_error("unknown coordinator read operation");
}

auto make_coordinator_read_request(
    CoordinatorReadOperation operation, std::span<const std::uint8_t> echo_payload,
    std::optional<std::uint16_t> selector, std::optional<std::uint8_t> index,
    std::uint8_t transaction) -> CoordinatorReadRequest {
  CoordinatorReadRequest request{operation, 0, 0, transaction, {}};
  const auto reject_irrelevant = [&] {
    if (!echo_payload.empty() || selector || index) {
      throw std::invalid_argument(
          "read operation received an option that does not apply");
    }
  };
  switch (operation) {
    case CoordinatorReadOperation::Echo:
      if (selector || index || echo_payload.empty() || echo_payload.size() > 64U) {
        throw std::invalid_argument(
            "echo requires 1..64 payload bytes and no selector or index");
      }
      request.request_id = 0x01U;
      request.response_id = 0x02U;
      request.payload.assign(echo_payload.begin(), echo_payload.end());
      break;
    case CoordinatorReadOperation::ImageState:
      reject_irrelevant(); request.request_id = 0x04U; request.response_id = 0x05U;
      break;
    case CoordinatorReadOperation::RangeStatus:
      reject_irrelevant(); request.request_id = 0x0bU; request.response_id = 0x0cU;
      break;
    case CoordinatorReadOperation::Config:
      if (!selector || !echo_payload.empty() || index) {
        throw std::invalid_argument("config requires only a 16-bit config ID");
      }
      request.request_id = 0x11U; request.response_id = 0x12U;
      request.payload = {static_cast<std::uint8_t>(*selector >> 8U),
                         static_cast<std::uint8_t>(*selector)};
      break;
    case CoordinatorReadOperation::Statistics:
      if (!selector || *selector > 11U || !echo_payload.empty() || index) {
        throw std::invalid_argument(
            "statistics requires only a stat group in 0..11");
      }
      request.request_id = 0x15U; request.response_id = 0x16U;
      request.payload = {0, static_cast<std::uint8_t>(*selector)};
      break;
    case CoordinatorReadOperation::Peer:
      if (!index || !echo_payload.empty() || selector) {
        throw std::invalid_argument("peer requires only an 8-bit index");
      }
      request.request_id = 0x21U; request.response_id = 0x22U;
      request.payload = {*index};
      break;
    case CoordinatorReadOperation::Mempool:
      if (!index || !echo_payload.empty() || selector) {
        throw std::invalid_argument("mempool requires only an 8-bit index");
      }
      request.request_id = 0x23U; request.response_id = 0x24U;
      request.payload = {*index};
      break;
  }
  return request;
}

auto SensorFrameRadio::transact_frame_with_mac_ack(
    std::span<const std::uint8_t> frame,
    std::span<const std::complex<float>> waveform,
    std::size_t maximum_frames, std::chrono::milliseconds timeout,
    std::uint32_t sample_rate, std::uint16_t expected_pan,
    std::chrono::microseconds turnaround,
    const MacAckPreparedCallback& before_ack_transmit,
    std::size_t maximum_request_attempts,
    std::chrono::milliseconds request_retry_interval)
    -> std::vector<std::vector<std::uint8_t>> {
  static_cast<void>(expected_pan);
  static_cast<void>(turnaround);
  static_cast<void>(maximum_request_attempts);
  static_cast<void>(request_retry_interval);
  auto received = transact_frame(frame, waveform, maximum_frames, timeout);
  std::size_t batch_begin{};
  while (batch_begin < received.size()) {
    std::optional<std::uint8_t> sequence;
    for (std::size_t index = batch_begin; index < received.size(); ++index) {
      const auto requested = requested_ack_sequence(received[index]);
      if (!requested) continue;
      if (sequence && *sequence != *requested) {
        throw std::runtime_error(
            "received frames requested inconsistent MAC acknowledgements");
      }
      sequence = requested;
    }
    if (!sequence) return received;
    const auto ack_frame = core::encode_mac_ack_frame(*sequence);
    const auto ack_waveform =
        dsp::modulate_efr32_custom_oqpsk(ack_frame, sample_rate);
    before_ack_transmit(ack_frame, ack_waveform.samples, 0U);
    transmit_frame(ack_frame, ack_waveform.samples);
    if (std::any_of(received.begin() +
                        static_cast<std::ptrdiff_t>(batch_begin),
                    received.end(), is_join_signature_frame) ||
        received.size() >= maximum_frames) {
      return received;
    }
    const auto next_batch_begin = received.size();
    auto continuation =
        receive_frames(maximum_frames - received.size(), timeout);
    if (continuation.empty()) return received;
    received.insert(received.end(),
                    std::make_move_iterator(continuation.begin()),
                    std::make_move_iterator(continuation.end()));
    batch_begin = next_batch_begin;
  }
  return received;
}

DeviceSensorFrameRadio::DeviceSensorFrameRadio(
    radio::Device& device, std::uint32_t sample_rate,
    std::size_t samples_per_receive, double carrier_offset_hz,
    std::optional<std::filesystem::path> raw_iq_path,
    std::optional<std::array<std::uint8_t, 8>> target_eui,
    std::size_t target_frame_count)
    : device_(device), sample_rate_(sample_rate),
      samples_per_receive_(samples_per_receive),
      carrier_offset_hz_(carrier_offset_hz),
      raw_iq_path_(std::move(raw_iq_path)), target_eui_(target_eui),
      target_frame_count_(target_frame_count) {
  if (sample_rate_ < 4'000'000U || sample_rate_ > 20'000'000U ||
      samples_per_receive_ == 0U || !std::isfinite(carrier_offset_hz_) ||
      std::abs(carrier_offset_hz_) > 100'000.0 ||
      (target_frame_count_ > 0U && !target_eui_)) {
    throw std::invalid_argument("bounded frame-radio configuration is invalid");
  }
}

auto DeviceSensorFrameRadio::next_raw_iq_path()
    -> std::optional<std::filesystem::path> {
  if (!raw_iq_path_) return std::nullopt;
  auto capture_path = *raw_iq_path_;
  const auto sequence = raw_capture_sequence_++;
  if (sequence == 0U) return capture_path;
  capture_path.replace_filename(capture_path.stem().string() + "-" +
                                std::to_string(sequence) +
                                capture_path.extension().string());
  return capture_path;
}

void DeviceSensorFrameRadio::transmit_frame(
    std::span<const std::uint8_t> frame,
    std::span<const std::complex<float>> waveform) {
  if (frame.empty() || waveform.empty()) {
    throw std::invalid_argument("frame and waveform must be nonempty");
  }
  if (!device_.capabilities().tx) {
    throw std::runtime_error("selected radio is receive-only");
  }
  device_.transmit(waveform, 0U);
}

auto DeviceSensorFrameRadio::transact_frame(
    std::span<const std::uint8_t> frame,
    std::span<const std::complex<float>> waveform,
    std::size_t maximum_frames, std::chrono::milliseconds timeout)
    -> std::vector<std::vector<std::uint8_t>> {
  if (!device_.capabilities().full_duplex) {
    return SensorFrameRadio::transact_frame(frame, waveform, maximum_frames,
                                             timeout);
  }
  device_.start_receive_stream();
  continuous_receive_active_ = true;
  std::future<void> transmission;
  try {
    transmission = std::async(std::launch::async, [&] {
      transmit_frame(frame, waveform);
    });
    auto result = receive_frames_impl(maximum_frames, timeout,
                                      [&] { transmission.get(); });
    if (continuous_receive_active_) {
      device_.stop_receive_stream();
      continuous_receive_active_ = false;
    }
    return result;
  } catch (...) {
    if (transmission.valid()) {
      try {
        transmission.get();
      } catch (...) {
      }
    }
    if (continuous_receive_active_) {
      try {
        device_.stop_receive_stream();
      } catch (...) {
      }
      continuous_receive_active_ = false;
    }
    throw;
  }
}

auto DeviceSensorFrameRadio::transact_frame_with_mac_ack(
    std::span<const std::uint8_t> frame,
    std::span<const std::complex<float>> waveform,
    std::size_t maximum_frames, std::chrono::milliseconds timeout,
    std::uint32_t sample_rate, std::uint16_t expected_pan,
    std::chrono::microseconds turnaround,
    const MacAckPreparedCallback& before_ack_transmit,
    std::size_t maximum_request_attempts,
    std::chrono::milliseconds request_retry_interval)
    -> std::vector<std::vector<std::uint8_t>> {
  if (!device_.capabilities().full_duplex ||
      !device_.capabilities().hardware_timestamps ||
      !device_.capabilities().timed_tx || !target_eui_ ||
      sample_rate != sample_rate_) {
    return SensorFrameRadio::transact_frame_with_mac_ack(
        frame, waveform, maximum_frames, timeout, sample_rate, expected_pan,
        turnaround, before_ack_transmit, maximum_request_attempts,
        request_retry_interval);
  }
  if (maximum_frames == 0U || timeout.count() <= 0 ||
      turnaround.count() < 0 || turnaround > std::chrono::milliseconds(5) ||
      maximum_request_attempts == 0U || maximum_request_attempts > 16U ||
      request_retry_interval.count() < 0 ||
      request_retry_interval > std::chrono::seconds(5)) {
    throw std::invalid_argument("reactive MAC-ACK bounds are invalid");
  }

  const auto reactive_block_samples =
      std::min<std::size_t>(samples_per_receive_, sample_rate_ / 1'000U);
  const auto maximum_receive_blocks = std::min<std::size_t>(
      65'536U,
      static_cast<std::size_t>(
          std::ceil(static_cast<long double>(timeout.count()) *
                    static_cast<long double>(sample_rate_) /
                    (1'000.0L *
                     static_cast<long double>(reactive_block_samples)))) +
          2U);
  std::deque<double> idle_powers;
  std::vector<std::complex<float>> active_burst;
  std::vector<std::vector<std::complex<float>>> captured_bursts;
  std::uint64_t active_burst_time_ns{};
  bool burst_active{};
  std::size_t inactive_blocks{};
  bool ack_scheduled_for_burst{};
  struct PendingAck {
    std::uint8_t sequence{};
    std::uint8_t phr{};
    std::vector<std::uint8_t> frame;
    std::vector<std::complex<float>> waveform;
  };
  std::optional<PendingAck> pending_ack;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::future<std::optional<MatchedJoinAckPrefix>> prefix_match;
  bool prefix_match_started{};
  std::optional<std::uint64_t> prefix_match_enable_time_ns;
  std::optional<std::uint64_t> stop_after_device_time_ns;
  std::atomic<bool> first_request_transmitted{};
  std::atomic<bool> stop_request_retries{};
  std::mutex request_retry_mutex;
  std::condition_variable request_retry_condition;
  constexpr std::size_t kNoiseCalibrationBlocks = 8U;
  constexpr std::size_t kMaximumNoiseBlocks = 64U;
  // A 61-byte hub frame occupies about 6.75 ms at this PHY. The reactive path
  // consumes 1 ms receive blocks, which leaves about 5.75 ms when the next
  // matching copy begins. Require 5 ms of hardware-clock lead and defer when
  // host processing has consumed that bounded scheduling window.
  constexpr std::uint64_t kMinimumTimedTxLeadNs = 5'000'000ULL;
  // The hub can repeat an unacknowledged downlink several seconds later.
  // Keep RX active long enough to re-ACK one retry and receive the gated
  // SenseAck; the caller's bounded wall-clock timeout remains authoritative.
  constexpr std::uint64_t kPostAckCaptureNs = 6'000'000'000ULL;
  constexpr std::uint64_t kPostRequestTxGuardNs = 0ULL;
  constexpr std::size_t kReactivePrefixSamples = 9'000U;

  const auto schedule_ack = [&](const PendingAck& pending,
                                std::uint64_t frame_start_time_ns) {
    const auto total_symbols =
        dsp::phy::preamble_symbols + dsp::phy::sync_symbols.size() +
        (static_cast<std::size_t>(pending.phr) + 1U) * 2U;
    const auto duration_samples =
        total_symbols * sample_rate_ / dsp::phy::symbol_rate;
    const auto normal_ack_time_ns =
        frame_start_time_ns +
        static_cast<std::uint64_t>(duration_samples) * 1'000'000'000ULL /
            sample_rate_ +
        static_cast<std::uint64_t>(turnaround.count()) * 1'000ULL;
    const auto ack_time_ns = normal_ack_time_ns;
    if (ack_time_ns <= device_.current_time_ns() + kMinimumTimedTxLeadNs) {
      return false;
    }
    before_ack_transmit(pending.frame, pending.waveform, ack_time_ns);
    device_.transmit(pending.waveform, ack_time_ns);
    ack_scheduled_for_burst = true;
    stop_after_device_time_ns = ack_time_ns + kPostAckCaptureNs;
    return true;
  };

  const auto consume_prefix_match = [&] {
    if (!prefix_match.valid() ||
        prefix_match.wait_for(std::chrono::milliseconds(0)) !=
            std::future_status::ready) {
      return;
    }
    const auto matched = prefix_match.get();
    if (matched && !pending_ack) {
      stop_request_retries.store(true);
      request_retry_condition.notify_all();
      const auto transmitted_sequence = matched->sequence;
      auto ack_frame = core::encode_mac_ack_frame(transmitted_sequence);
      auto ack_waveform =
          dsp::modulate_efr32_custom_oqpsk(ack_frame, sample_rate_);
      auto prepared = PendingAck{transmitted_sequence, matched->phr,
                                 std::move(ack_frame),
                                 std::move(ack_waveform.samples)};
      if (ack_scheduled_for_burst ||
          !schedule_ack(prepared, active_burst_time_ns)) {
        pending_ack = std::move(prepared);
      }
    }
  };

  int raw_descriptor = -1;
  const auto capture_path = next_raw_iq_path();
  if (capture_path) {
    raw_descriptor =
        ::open(capture_path->c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (raw_descriptor < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "create reactive raw receive evidence");
    }
  }
  const auto persist_capture = [&] {
    if (raw_descriptor < 0) return;
    if (::fsync(raw_descriptor) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "fsync reactive raw receive evidence");
    }
    if (::close(raw_descriptor) != 0) {
      raw_descriptor = -1;
      throw std::system_error(errno, std::generic_category(),
                              "close reactive raw receive evidence");
    }
    raw_descriptor = -1;
  };

  device_.start_receive_stream();
  continuous_receive_active_ = true;
  std::future<void> request_tx;
  try {
    request_tx = std::async(std::launch::async, [&] {
      for (std::size_t attempt = 0U; attempt < maximum_request_attempts;
           ++attempt) {
        if (stop_request_retries.load()) break;
        transmit_frame(frame, waveform);
        first_request_transmitted.store(true);
        if (attempt + 1U == maximum_request_attempts) break;
        std::unique_lock<std::mutex> lock(request_retry_mutex);
        request_retry_condition.wait_for(
            lock, request_retry_interval,
            [&] { return stop_request_retries.load(); });
      }
    });
    for (std::size_t block_index = 0U;
         block_index < maximum_receive_blocks &&
         std::chrono::steady_clock::now() < deadline;
         ++block_index) {
      auto block = device_.receive(reactive_block_samples);
      if (block.status == radio::SampleBlockStatus::Timeout ||
          block.status == radio::SampleBlockStatus::EndOfStream ||
          block.status == radio::SampleBlockStatus::Cancelled) {
        break;
      }
      if (block.status != radio::SampleBlockStatus::Data ||
          block.samples.empty() || block.discontinuity) {
        throw std::runtime_error("reactive radio receive was discontinuous: " +
                                 block.discontinuity_reason);
      }
      const auto block_power = average_power(block.samples);
      const auto noise_floor = median_power(idle_powers);
      const auto activity_threshold =
          std::max(1e-10, noise_floor * 6.0);
      const auto calibrated = idle_powers.size() >= kNoiseCalibrationBlocks;
      const auto active = calibrated && block_power > activity_threshold;
      if (!prefix_match_enable_time_ns &&
          first_request_transmitted.load()) {
        prefix_match_enable_time_ns =
            device_.current_time_ns() + kPostRequestTxGuardNs;
      }
      consume_prefix_match();
      if (raw_descriptor >= 0) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(
            block.samples.data());
        write_all(raw_descriptor,
                  std::span<const std::uint8_t>(
                      bytes, block.samples.size() *
                                 sizeof(std::complex<float>)));
      }

      if (!burst_active && active) {
        burst_active = true;
        inactive_blocks = 0U;
        const auto activity_offset =
            first_active_sample(block.samples, sample_rate_,
                                activity_threshold);
        active_burst_time_ns =
            block.monotonic_time_ns +
            static_cast<std::uint64_t>(activity_offset) *
                1'000'000'000ULL / sample_rate_;
        active_burst.assign(
            block.samples.begin() +
                static_cast<std::ptrdiff_t>(activity_offset),
            block.samples.end());
        if (pending_ack && !ack_scheduled_for_burst) {
          auto pending = std::move(*pending_ack);
          pending_ack.reset();
          if (!schedule_ack(pending, active_burst_time_ns)) {
            pending_ack = std::move(pending);
          }
        }
      } else if (burst_active) {
        active_burst.insert(active_burst.end(), block.samples.begin(),
                            block.samples.end());
        inactive_blocks = active ? 0U : inactive_blocks + 1U;
      } else {
        idle_powers.push_back(block_power);
        if (idle_powers.size() > kMaximumNoiseBlocks) idle_powers.pop_front();
      }

      if (burst_active && !prefix_match_started && !prefix_match.valid() &&
          !pending_ack && !ack_scheduled_for_burst &&
          prefix_match_enable_time_ns &&
          active_burst_time_ns >= *prefix_match_enable_time_ns &&
          active_burst.size() >= kReactivePrefixSamples) {
        prefix_match_started = true;
        auto snapshot = std::vector<std::complex<float>>(
            active_burst.begin(),
            active_burst.begin() +
                static_cast<std::ptrdiff_t>(kReactivePrefixSamples));
        prefix_match = std::async(
            std::launch::async,
            [snapshot = std::move(snapshot), sample_rate = sample_rate_,
             carrier_offset_hz = carrier_offset_hz_, expected_pan,
             target_eui = *target_eui_] {
              return match_join_ack_prefix(snapshot, sample_rate,
                                           carrier_offset_hz, expected_pan,
                                           target_eui);
            });
      }

      const auto block_end_time_ns =
          block.monotonic_time_ns +
          static_cast<std::uint64_t>(block.samples.size()) *
              1'000'000'000ULL / sample_rate_;
      if (stop_after_device_time_ns &&
          block_end_time_ns >= *stop_after_device_time_ns) {
        break;
      }
      if (burst_active && inactive_blocks >= 2U) {
        captured_bursts.push_back(active_burst);
        burst_active = false;
        active_burst.clear();
        prefix_match_started = false;
        ack_scheduled_for_burst = false;
        inactive_blocks = 0U;
        idle_powers.push_back(block_power);
        if (idle_powers.size() > kMaximumNoiseBlocks) idle_powers.pop_front();
      }
    }
    stop_request_retries.store(true);
    request_retry_condition.notify_all();
    request_tx.get();
    if (burst_active && !active_burst.empty()) {
      captured_bursts.push_back(active_burst);
      burst_active = false;
    }
    device_.stop_receive_stream();
    continuous_receive_active_ = false;
    if (prefix_match.valid()) {
      prefix_match.wait();
      consume_prefix_match();
    }
    persist_capture();
  } catch (...) {
    stop_request_retries.store(true);
    request_retry_condition.notify_all();
    if (request_tx.valid()) {
      try {
        request_tx.get();
      } catch (...) {
      }
    }
    if (continuous_receive_active_) {
      try {
        device_.stop_receive_stream();
      } catch (...) {
      }
      continuous_receive_active_ = false;
    }
    try {
      persist_capture();
    } catch (...) {
      if (raw_descriptor >= 0) (void)::close(raw_descriptor);
    }
    throw;
  }

  std::vector<std::vector<std::uint8_t>> frames;
  dsp::DecodeOptions options;
  options.carrier_hypotheses_hz = {carrier_offset_hz_};
  options.maximum_symbol_distance = 16U;
  options.coarse_start_stride = 8U;
  options.coarse_template_stride = 32U;
  for (const auto& burst_samples : captured_bursts) {
    auto decoded = dsp::decode_bursts(
        burst_samples, sample_rate_, options, 16U);
    if (decoded.empty()) {
      decoded = dsp::decode_bursts(
          burst_samples, sample_rate_, fine_decode_options(carrier_offset_hz_),
          16U);
    }
    for (const auto& burst : decoded) {
      if (!frame_matches_target(burst.frame_bytes, target_eui_) ||
          std::find(frames.begin(), frames.end(), burst.frame_bytes) !=
              frames.end()) {
        continue;
      }
      frames.push_back(burst.frame_bytes);
      if (frames.size() == maximum_frames ||
          (target_frame_count_ > 0U &&
           frames.size() >= target_frame_count_)) {
        return frames;
      }
    }
  }
  return frames;
}

auto DeviceSensorFrameRadio::receive_frames(
    std::size_t maximum_frames, std::chrono::milliseconds timeout)
    -> std::vector<std::vector<std::uint8_t>> {
  return receive_frames_impl(maximum_frames, timeout, {});
}

auto DeviceSensorFrameRadio::receive_frames_impl(
    std::size_t maximum_frames, std::chrono::milliseconds timeout,
    const std::function<void()>& before_stream_stop)
    -> std::vector<std::vector<std::uint8_t>> {
  if (maximum_frames == 0U || timeout.count() <= 0) {
    throw std::invalid_argument("receive bounds must be positive");
  }
  if (target_frame_count_ > maximum_frames) {
    throw std::invalid_argument(
        "target frame count exceeds the receive frame bound");
  }
  std::vector<std::vector<std::uint8_t>> frames;
  std::vector<radio::SampleBlock> deferred_blocks;
  std::vector<std::complex<float>> previous_tail;
  int raw_descriptor = -1;
  bool raw_blocks_written = false;
  const auto capture_path = next_raw_iq_path();
  if (capture_path) {
    raw_descriptor =
        ::open(capture_path->c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (raw_descriptor < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "create raw receive evidence");
    }
  }
  const auto persist_raw_iq = [&] {
    if (raw_descriptor < 0) return;
    if (!raw_blocks_written) {
      for (const auto& block : deferred_blocks) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(
            block.samples.data());
        write_all(raw_descriptor,
                  std::span<const std::uint8_t>(
                      bytes, block.samples.size() *
                                 sizeof(std::complex<float>)));
      }
      raw_blocks_written = true;
    }
    if (::fsync(raw_descriptor) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "fsync raw receive evidence");
    }
    if (::close(raw_descriptor) != 0) {
      raw_descriptor = -1;
      throw std::system_error(errno, std::generic_category(),
                              "close raw receive evidence");
    }
    raw_descriptor = -1;
  };
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const auto block_seconds = static_cast<long double>(samples_per_receive_) /
                             static_cast<long double>(sample_rate_);
  const auto timeout_seconds =
      static_cast<long double>(timeout.count()) / 1'000.0L;
  constexpr std::size_t kAbsoluteMaximumReceiveBlocks = 4'096U;
  const auto requested_blocks =
      std::ceil(timeout_seconds / block_seconds) + 1.0L;
  const auto maximum_receive_blocks = static_cast<std::size_t>(
      std::min(requested_blocks,
               static_cast<long double>(kAbsoluteMaximumReceiveBlocks)));
  dsp::DecodeOptions decode_options;
  decode_options.carrier_hypotheses_hz = {carrier_offset_hz_};
  decode_options.coarse_start_stride = 8U;
  decode_options.coarse_template_stride = 32U;
  const auto receive_complete = [&] {
    return frames.size() == maximum_frames ||
           (target_frame_count_ > 0U &&
            frames.size() >= target_frame_count_);
  };
  const auto decode_samples =
      [&](std::span<const std::complex<float>> samples) {
        const auto ranges = active_sample_ranges(samples, sample_rate_);
        for (const auto& range : ranges) {
          auto decoded = dsp::decode_bursts(
              samples.subspan(range.begin, range.end - range.begin),
              sample_rate_, decode_options, 16U);
          if (decoded.empty()) {
            auto fine_options = decode_options;
            fine_options.maximum_sample_clock_correction_ppm = 0.0;
            constexpr double kFineCarrierStepHz = 25.0;
            constexpr double kFineCarrierSpanHz = 250.0;
            for (double delta = kFineCarrierStepHz;
                 delta <= kFineCarrierSpanHz; delta += kFineCarrierStepHz) {
              fine_options.carrier_hypotheses_hz.push_back(
                  carrier_offset_hz_ + delta);
              fine_options.carrier_hypotheses_hz.push_back(
                  carrier_offset_hz_ - delta);
            }
            decoded = dsp::decode_bursts(
                samples.subspan(range.begin, range.end - range.begin),
                sample_rate_, fine_options, 16U);
          }
          for (const auto& burst : decoded) {
            if (!frame_matches_target(burst.frame_bytes, target_eui_) ||
                std::find(frames.begin(), frames.end(), burst.frame_bytes) !=
                    frames.end()) {
              continue;
            }
            frames.push_back(burst.frame_bytes);
            if (receive_complete()) return;
          }
          if (receive_complete()) return;
        }
      };
  const auto decode_block =
      [&](std::span<const std::complex<float>> samples) {
        decode_samples(samples);
        if (!receive_complete() && !previous_tail.empty()) {
          std::vector<std::complex<float>> combined;
          combined.reserve(previous_tail.size() + samples.size());
          combined.insert(combined.end(), previous_tail.begin(),
                          previous_tail.end());
          combined.insert(combined.end(), samples.begin(), samples.end());
          decode_samples(combined);
        }
        const auto overlap_samples =
            std::min<std::size_t>(samples.size(), sample_rate_ / 50U);
        previous_tail.assign(
            samples.end() - static_cast<std::ptrdiff_t>(overlap_samples),
            samples.end());
      };
  try {
    for (std::size_t block_index = 0;
         block_index < maximum_receive_blocks &&
         std::chrono::steady_clock::now() < deadline;
         ++block_index) {
      auto block = device_.receive(samples_per_receive_);
      if (block.status == radio::SampleBlockStatus::Timeout ||
          block.status == radio::SampleBlockStatus::EndOfStream ||
          block.status == radio::SampleBlockStatus::Cancelled) {
        break;
      }
      if (block.status == radio::SampleBlockStatus::DeviceRemoved ||
          block.status == radio::SampleBlockStatus::Error) {
        throw std::runtime_error("radio receive failed: " +
                                 block.discontinuity_reason);
      }
      if (block.status != radio::SampleBlockStatus::Data ||
          block.samples.empty()) {
        continue;
      }
      deferred_blocks.push_back(std::move(block));
    }
    if (continuous_receive_active_ && before_stream_stop) {
      before_stream_stop();
    }
    if (continuous_receive_active_) {
      device_.stop_receive_stream();
      continuous_receive_active_ = false;
    }
    persist_raw_iq();
  } catch (...) {
    try {
      persist_raw_iq();
    } catch (...) {
      if (raw_descriptor >= 0) (void)::close(raw_descriptor);
      throw;
    }
    throw;
  }
  for (const auto& block : deferred_blocks) {
    decode_block(block.samples);
    if (receive_complete()) break;
  }
  return frames;
}

SensorSessionRunner::SensorSessionRunner(SensorFrameRadio& radio,
                                         SensorRunnerConfig config)
    : radio_(radio), config_(std::move(config)) {
  if (config_.pan == 0U || config_.hub_short_address > 2U ||
      config_.sample_rate < 4'000'000U || config_.sample_rate > 20'000'000U ||
      config_.receive_timeout.count() <= 0 ||
      config_.maximum_receive_frames == 0U ||
      config_.maximum_receive_frames > 64U ||
      config_.evidence_directory.empty() || !config_.oracle) {
    throw std::invalid_argument("sensor runner configuration is invalid");
  }
  if (!std::filesystem::create_directory(config_.evidence_directory)) {
    throw std::runtime_error("sensor runner evidence directory already exists");
  }
  std::filesystem::permissions(config_.evidence_directory,
                               std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
}

void SensorSessionRunner::require_healthy(std::string_view stage) const {
  const auto status = config_.oracle();
  if (!status.healthy) {
    throw std::runtime_error(std::string(stage) + " oracle failed: " +
                             status.detail);
  }
}

void SensorSessionRunner::preserve_received(
    std::string_view prefix,
    const std::vector<std::vector<std::uint8_t>>& frames) {
  for (std::size_t index = 0; index < frames.size(); ++index) {
    const auto filename = std::string(prefix) + "-" +
                          std::to_string(index) + ".frame.bin";
    write_exclusive(config_.evidence_directory / filename, frames[index]);
  }
}

void SensorSessionRunner::join(core::SensorSession& session) {
  if (session.state() != core::SensorSessionState::Created &&
      session.state() != core::SensorSessionState::JoinSent) {
    throw std::logic_error("join runner requires a created or unanswered join session");
  }
  const auto& identity = session.identity();
  const std::vector<std::uint8_t> serial(identity.serial.begin(),
                                         identity.serial.end());
  core::JoinRequest request;
  request.version = 6;
  request.bsp = 0;
  request.firmware_version = 1;
  request.physical_device_id = identity.physical_device_id;
  request.serial = serial;
  request.nonce = identity.nonce;
  request.regulatory_code = 1;
  request.tlvs = {{0, serial},
                  {1, {identity.nonce.begin(), identity.nonce.end()}},
                  {3, {1}}};

  const auto body = core::encode_join_request(request);
  const core::VcmpFrame vcmp{kJoinType, 0, 0, std::nullopt,
                             core::VcmpPlaintext{body}};
  const auto frame_control = static_cast<std::uint16_t>(
      kShortDestinationFcf |
      (config_.request_uplink_mac_ack ? kMacAckRequestFlag : 0U));
  const core::VmacFrame vmac{frame_control, 0, config_.pan,
                             config_.hub_short_address, identity.eui64,
                             core::encode_vcmp(vcmp)};
  const auto frame = encode_radio(vmac);
  auto waveform = dsp::modulate_efr32_custom_oqpsk(frame, config_.sample_rate);
  write_exclusive(config_.evidence_directory / "join.frame.bin", frame);
  write_waveform(config_.evidence_directory / "join.waveform.cf32",
                 waveform.samples);
  require_healthy("pre-join-TX");
  if (config_.dry_run) return;
  session.mark_join_sent();
  std::vector<std::uint8_t> transmitted_ack;
  std::optional<std::uint64_t> transmitted_ack_time_ns;
  const auto persist_ack_evidence = [&] {
    if (transmitted_ack.empty() || !transmitted_ack_time_ns) return;
    const auto ack_waveform = dsp::modulate_efr32_custom_oqpsk(
        transmitted_ack, config_.sample_rate);
    write_exclusive(config_.evidence_directory /
                        "join-mac-ack.frame.bin",
                    transmitted_ack);
    write_waveform(config_.evidence_directory /
                       "join-mac-ack.waveform.cf32",
                   ack_waveform.samples);
    const auto metadata = std::string("{\"requested_device_time_ns\":") +
                          std::to_string(*transmitted_ack_time_ns) +
                          ",\"turnaround_us\":" +
                          std::to_string(kMacAckTurnaround.count()) +
                          "}\n";
    write_exclusive(
        config_.evidence_directory / "join-mac-ack.tx.json",
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(metadata.data()),
            metadata.size()));
  };
  std::vector<std::vector<std::uint8_t>> received;
  try {
    received = radio_.transact_frame_with_mac_ack(
        frame, waveform.samples, config_.maximum_receive_frames,
        config_.receive_timeout, config_.sample_rate, config_.pan,
        kMacAckTurnaround,
        [&](std::span<const std::uint8_t> ack_frame,
            std::span<const std::complex<float>> ack_waveform,
            std::uint64_t requested_time_ns) {
          require_healthy("pre-join-MAC-ACK-TX");
          if (ack_frame.empty() || ack_waveform.empty()) {
            throw std::runtime_error("prepared MAC ACK is empty");
          }
          transmitted_ack.assign(ack_frame.begin(), ack_frame.end());
          transmitted_ack_time_ns = requested_time_ns;
        },
        5U, std::chrono::milliseconds(500));
  } catch (...) {
    try {
      persist_ack_evidence();
    } catch (...) {
    }
    throw;
  }
  persist_ack_evidence();
  preserve_received("join-rx", received);
  std::optional<core::JoinResponse> response;
  std::optional<core::JoinSignature> signature;
  std::optional<std::array<std::uint8_t, 8>> hub_eui;
  for (const auto& candidate : received) {
    const auto parsed_radio = core::parse_radio_frame(candidate);
    const auto* radio_frame = std::get_if<core::RadioFrame>(&parsed_radio);
    if (radio_frame == nullptr) continue;
    const auto parsed_vmac = core::parse_vmac(radio_frame->psdu);
    const auto* mac = std::get_if<core::VmacFrame>(&parsed_vmac);
    if (mac == nullptr || mac->destination_pan != config_.pan ||
        mac->source_eui == identity.eui64 ||
        !exact_sensor_destination(*mac, identity.eui64)) {
      continue;
    }
    const auto parsed_vcmp = core::parse_vcmp(mac->payload);
    const auto* management = std::get_if<core::VcmpFrame>(&parsed_vcmp);
    if (management == nullptr || management->flags != 0U) continue;
    const auto* plaintext = plaintext_body(*management);
    if (plaintext == nullptr) continue;
    if (management->type == kJoinResponseType) {
      const auto parsed = core::parse_join_response(*plaintext);
      const auto* value = std::get_if<core::JoinResponse>(&parsed);
      if (value == nullptr) continue;
      if (response && *response != *value) {
        throw std::runtime_error("ambiguous join responses were received");
      }
      response = *value;
      if (hub_eui && *hub_eui != mac->source_eui) {
        throw std::runtime_error("join response sources are inconsistent");
      }
      hub_eui = mac->source_eui;
    } else if (management->type == kJoinSignatureType) {
      const auto parsed = core::parse_join_signature(*plaintext);
      const auto* value = std::get_if<core::JoinSignature>(&parsed);
      if (value == nullptr) continue;
      if (signature && signature->signature != value->signature) {
        throw std::runtime_error("ambiguous join signatures were received");
      }
      signature = *value;
      if (hub_eui && *hub_eui != mac->source_eui) {
        throw std::runtime_error("join signature source is inconsistent");
      }
      hub_eui = mac->source_eui;
    }
  }
  require_healthy("post-join-RX");
  if (!response || !signature) {
    throw std::runtime_error("bounded join receive did not yield one response and signature");
  }
  session.accept_join_response(*response);
  auto shared = core::p256_shared_x(session.private_scalar(),
                                    response->local_public_xy);
  auto key = core::derive_peer_key(shared);
  session.install_key_fingerprint(core::session_key_fingerprint(key));
  OPENSSL_cleanse(shared.data(), shared.size());
  OPENSSL_cleanse(key.data(), key.size());
  session.mark_joined(*signature);
}

void SensorSessionRunner::send_heartbeat(core::SensorSession& session,
                                         std::uint32_t event_id) {
  send_event(session, {kHeartbeatEventType, true}, event_id, "heartbeat");
}

auto SensorSessionRunner::read_coordinator(
    core::SensorSession& session, const CoordinatorReadRequest& request,
    std::string_view evidence_stem)
    -> std::optional<CoordinatorReadResult> {
  if (session.state() != core::SensorSessionState::Joined) {
    throw std::logic_error("coordinator read requires a joined session");
  }

  std::optional<std::uint16_t> selector;
  std::optional<std::uint8_t> index;
  std::span<const std::uint8_t> echo;
  switch (request.operation) {
    case CoordinatorReadOperation::Echo:
      echo = request.payload;
      break;
    case CoordinatorReadOperation::Config:
    case CoordinatorReadOperation::Statistics:
      if (request.payload.size() != 2U) {
        throw std::invalid_argument("coordinator read selector is malformed");
      }
      selector = be16_at(request.payload, 0U);
      break;
    case CoordinatorReadOperation::Peer:
    case CoordinatorReadOperation::Mempool:
      if (request.payload.size() != 1U) {
        throw std::invalid_argument("coordinator read index is malformed");
      }
      index = request.payload[0];
      break;
    case CoordinatorReadOperation::ImageState:
    case CoordinatorReadOperation::RangeStatus:
      if (!request.payload.empty()) {
        throw std::invalid_argument("coordinator read body must be empty");
      }
      break;
  }
  const auto canonical = make_coordinator_read_request(
      request.operation, echo, selector, index, request.transaction);
  if (request.request_id != canonical.request_id ||
      request.response_id != canonical.response_id ||
      request.payload != canonical.payload) {
    throw std::invalid_argument(
        "coordinator read request differs from the fixed allowlist");
  }

  const auto rpc = core::encode_rpc(
      {request.request_id, request.transaction,
       static_cast<std::uint8_t>(request.payload.size()), 0,
       request.payload, {}});
  auto shared = core::p256_shared_x(session.private_scalar(),
                                    session.remote_public_xy());
  auto key = core::derive_peer_key(shared);
  try {
    const auto counter = session.send_counter();
    const auto protected_body = core::seal_vcmp(
        kEncryptedManagementType, 1, counter, rpc, key, event_iv(config_));
    const core::VmacFrame vmac{
        static_cast<std::uint16_t>(kShortDestinationFcf |
                                   kMacAckRequestFlag),
        1, config_.pan, config_.hub_short_address, session.identity().eui64,
        core::encode_vcmp(protected_body)};
    const auto frame = encode_radio(vmac);
    const auto waveform =
        dsp::modulate_efr32_custom_oqpsk(frame, config_.sample_rate).samples;
    const auto stem = std::string(evidence_stem);
    write_exclusive(config_.evidence_directory / (stem + ".frame.bin"), frame);
    write_waveform(config_.evidence_directory / (stem + ".waveform.cf32"),
                   waveform);
    const auto request_record =
        "{\"schema\":\"bh61.coordinator-read-request/v1\",\"operation\":\"" +
        std::string(coordinator_read_operation_name(request.operation)) +
        "\",\"request_id\":" + std::to_string(request.request_id) +
        ",\"response_id\":" + std::to_string(request.response_id) +
        ",\"transaction\":" + std::to_string(request.transaction) +
        ",\"counter\":" + std::to_string(counter) +
        ",\"payload_hex\":\"" + hex_bytes(request.payload) + "\"}\n";
    write_exclusive(
        config_.evidence_directory / (stem + "-request.json"),
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(request_record.data()),
            request_record.size()));

    require_healthy("pre-read-rpc-TX");
    if (config_.dry_run) {
      OPENSSL_cleanse(shared.data(), shared.size());
      OPENSSL_cleanse(key.data(), key.size());
      return std::nullopt;
    }

    session.mark_protected_request_sent();
    std::size_t ack_index = 0U;
    const auto received = radio_.transact_frame_with_mac_ack(
        frame, waveform, config_.maximum_receive_frames,
        config_.receive_timeout, config_.sample_rate, config_.pan,
        kMacAckTurnaround,
        [&](std::span<const std::uint8_t> ack_frame,
            std::span<const std::complex<float>> ack_waveform,
            std::uint64_t requested_time_ns) {
          require_healthy("pre-read-rpc-MAC-ACK-TX");
          const auto prefix =
              stem + "-mac-ack-" + std::to_string(ack_index++);
          write_exclusive(
              config_.evidence_directory / (prefix + ".frame.bin"),
              ack_frame);
          write_waveform(
              config_.evidence_directory / (prefix + ".waveform.cf32"),
              ack_waveform);
          const auto metadata =
              "{\"requested_device_time_ns\":" +
              std::to_string(requested_time_ns) +
              ",\"turnaround_us\":" +
              std::to_string(kMacAckTurnaround.count()) + "}\n";
          write_exclusive(
              config_.evidence_directory / (prefix + ".tx.json"),
              std::span<const std::uint8_t>(
                  reinterpret_cast<const std::uint8_t*>(metadata.data()),
                  metadata.size()));
        });
    preserve_received(stem + "-rx", received);
    require_healthy("post-read-rpc-RX");

    std::optional<CoordinatorReadResult> result;
    for (const auto& candidate : received) {
      const auto parsed_radio = core::parse_radio_frame(candidate);
      const auto* radio_frame = std::get_if<core::RadioFrame>(&parsed_radio);
      if (radio_frame == nullptr) continue;
      const auto parsed_vmac = core::parse_vmac(radio_frame->psdu);
      const auto* mac = std::get_if<core::VmacFrame>(&parsed_vmac);
      if (mac == nullptr || mac->destination_pan != config_.pan ||
          mac->source_eui == session.identity().eui64 ||
          !exact_sensor_destination(*mac, session.identity().eui64)) {
        continue;
      }
      const auto parsed_vcmp = core::parse_vcmp(mac->payload);
      const auto* management = std::get_if<core::VcmpFrame>(&parsed_vcmp);
      if (management == nullptr ||
          management->type != kEncryptedManagementType ||
          (management->flags & 1U) == 0U) {
        continue;
      }
      const auto opened = core::open_vcmp(*management, key);
      const auto* plaintext = std::get_if<core::OpenedVcmp>(&opened);
      if (plaintext == nullptr) continue;
      const auto parsed_rpc = core::parse_rpc(plaintext->payload);
      const auto* response = std::get_if<core::RpcMessage>(&parsed_rpc);
      if (response == nullptr || response->message_id != request.response_id ||
          response->transaction != request.transaction) {
        continue;
      }
      if (!core::counter_is_acceptable(session.receive_counter(),
                                       plaintext->counter)) {
        throw std::runtime_error(
            "coordinator read response counter is outside the receive window");
      }
      validate_read_response(request, response->payload);
      CoordinatorReadResult observed{
          request.operation, response->message_id, response->transaction,
          plaintext->counter, response->payload};
      if (result && (result->counter != observed.counter ||
                     result->payload != observed.payload)) {
        throw std::runtime_error(
            "ambiguous coordinator read responses were received");
      }
      result = std::move(observed);
    }
    if (!result) {
      throw std::runtime_error(
          "bounded receive did not yield the expected coordinator read response");
    }
    session.accept_received_counter(result->counter);
    const auto response_record = read_response_json(*result);
    write_exclusive(
        config_.evidence_directory / (stem + "-response.json"),
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(response_record.data()),
            response_record.size()));
    OPENSSL_cleanse(shared.data(), shared.size());
    OPENSSL_cleanse(key.data(), key.size());
    return result;
  } catch (...) {
    OPENSSL_cleanse(shared.data(), shared.size());
    OPENSSL_cleanse(key.data(), key.size());
    throw;
  }
}

void SensorSessionRunner::send_event(core::SensorSession& session,
                                     core::EventSemantic semantic,
                                     std::uint32_t event_id,
                                     std::string_view evidence_stem) {
  if (session.state() != core::SensorSessionState::Joined) {
    throw std::logic_error("sensor event requires a joined session");
  }
  const auto event_id_bytes = core::store_be32(event_id);
  const core::EventRequest event{semantic.type, semantic.active, 0,
                                 {{kEventIdTlv,
                                   {event_id_bytes.begin(), event_id_bytes.end()}}}};
  const auto event_body = core::encode_event_request(event);
  if (event_body.front() != semantic.type ||
      (event_body[1] != 0U) != semantic.active) {
    throw std::logic_error("runner generated the wrong sensor event");
  }
  const auto rpc = core::encode_rpc(
      {kEventRequestId, 0x44, static_cast<std::uint8_t>(event_body.size()),
       0, event_body, {}});
  auto shared = core::p256_shared_x(session.private_scalar(),
                                    session.remote_public_xy());
  auto key = core::derive_peer_key(shared);
  const auto iv = event_iv(config_);
  const auto protected_body = core::seal_vcmp(
      kEncryptedManagementType, 1, session.send_counter(), rpc, key, iv);
  const auto frame_control = static_cast<std::uint16_t>(
      kShortDestinationFcf |
      (config_.request_uplink_mac_ack ? kMacAckRequestFlag : 0U));
  const core::VmacFrame vmac{frame_control, 1, config_.pan,
                             config_.hub_short_address,
                             session.identity().eui64,
                             core::encode_vcmp(protected_body)};
  const auto frame = encode_radio(vmac);
  auto waveform = dsp::modulate_efr32_custom_oqpsk(frame, config_.sample_rate);
  const auto stem = std::string(evidence_stem);
  write_exclusive(config_.evidence_directory / (stem + ".frame.bin"), frame);
  write_waveform(config_.evidence_directory / (stem + ".waveform.cf32"),
                 waveform.samples);
  require_healthy("pre-event-TX");
  if (config_.dry_run) {
    OPENSSL_cleanse(shared.data(), shared.size());
    OPENSSL_cleanse(key.data(), key.size());
    return;
  }
  struct MacAckEvidence {
    std::vector<std::uint8_t> frame;
    std::uint64_t requested_time_ns{};
  };
  std::vector<MacAckEvidence> mac_ack_evidence;
  const auto persist_mac_ack_evidence = [&] {
    for (std::size_t index = 0U; index < mac_ack_evidence.size(); ++index) {
      const auto prefix = stem + "-mac-ack-" + std::to_string(index);
      write_exclusive(config_.evidence_directory / (prefix + ".frame.bin"),
                      mac_ack_evidence[index].frame);
      const auto ack_waveform = dsp::modulate_efr32_custom_oqpsk(
          mac_ack_evidence[index].frame, config_.sample_rate);
      write_waveform(config_.evidence_directory / (prefix + ".waveform.cf32"),
                     ack_waveform.samples);
      const auto metadata =
          std::string("{\"requested_device_time_ns\":") +
          std::to_string(mac_ack_evidence[index].requested_time_ns) +
          ",\"turnaround_us\":" +
          std::to_string(kMacAckTurnaround.count()) + "}\n";
      write_exclusive(
          config_.evidence_directory / (prefix + ".tx.json"),
          std::span<const std::uint8_t>(
              reinterpret_cast<const std::uint8_t*>(metadata.data()),
              metadata.size()));
    }
  };
  std::vector<std::vector<std::uint8_t>> received;
  try {
    received = radio_.transact_frame_with_mac_ack(
        frame, waveform.samples, config_.maximum_receive_frames,
        config_.receive_timeout, config_.sample_rate, config_.pan,
        kMacAckTurnaround,
        [&](std::span<const std::uint8_t> ack_frame,
            std::span<const std::complex<float>> ack_waveform,
            std::uint64_t requested_time_ns) {
          mac_ack_evidence.push_back(
              {{ack_frame.begin(), ack_frame.end()}, requested_time_ns});
          static_cast<void>(ack_waveform);
        },
        1U, std::chrono::milliseconds(0));
  } catch (...) {
    try {
      persist_mac_ack_evidence();
    } catch (...) {
    }
    throw;
  }
  persist_mac_ack_evidence();
  session.mark_event_sent(event_id);
  preserve_received(stem + "-rx", received);
  std::optional<core::SenseAckRequest> acknowledgement;
  std::optional<std::uint16_t> acknowledgement_counter;
  std::optional<core::AckResponse> transaction_acknowledgement;
  std::optional<std::uint16_t> transaction_acknowledgement_counter;
  for (const auto& candidate : received) {
    const auto parsed_radio = core::parse_radio_frame(candidate);
    const auto* radio_frame = std::get_if<core::RadioFrame>(&parsed_radio);
    if (radio_frame == nullptr) continue;
    const auto parsed_vmac = core::parse_vmac(radio_frame->psdu);
    const auto* mac = std::get_if<core::VmacFrame>(&parsed_vmac);
    if (mac == nullptr || mac->destination_pan != config_.pan ||
        mac->source_eui == session.identity().eui64 ||
        !exact_sensor_destination(*mac, session.identity().eui64)) {
      continue;
    }
    const auto parsed_vcmp = core::parse_vcmp(mac->payload);
    const auto* management = std::get_if<core::VcmpFrame>(&parsed_vcmp);
    if (management == nullptr || management->type != kEncryptedManagementType ||
        (management->flags & 1U) == 0U) {
      continue;
    }
    const auto opened = core::open_vcmp(*management, key);
    const auto* plaintext = std::get_if<core::OpenedVcmp>(&opened);
    if (plaintext == nullptr ||
        !core::counter_is_acceptable(session.receive_counter(),
                                     plaintext->counter)) {
      continue;
    }
    const auto parsed_rpc = core::parse_rpc(plaintext->payload);
    const auto* rpc_message = std::get_if<core::RpcMessage>(&parsed_rpc);
    if (rpc_message == nullptr) continue;
    if (rpc_message->message_id == kSenseAckRequestId) {
      const auto parsed_ack = core::parse_sense_ack_request(rpc_message->payload);
      const auto* ack = std::get_if<core::SenseAckRequest>(&parsed_ack);
      if (ack == nullptr || ack->event_id != event_id) continue;
      if (acknowledgement &&
          (acknowledgement->event_id != ack->event_id ||
           *acknowledgement_counter != plaintext->counter)) {
        throw std::runtime_error(
            "ambiguous sensor-event acknowledgements were received");
      }
      session.mark_acknowledged(*ack, plaintext->counter);
      acknowledgement = *ack;
      acknowledgement_counter = plaintext->counter;
      continue;
    }
    if (rpc_message->message_id == kAckResponseId &&
        rpc_message->transaction == 0x44U &&
        (rpc_message->flags & 1U) != 0U) {
      const auto parsed_ack = core::parse_ack_response(rpc_message->payload);
      const auto* ack = std::get_if<core::AckResponse>(&parsed_ack);
      if (ack != nullptr) {
        if (transaction_acknowledgement &&
            (transaction_acknowledgement->status != ack->status ||
             *transaction_acknowledgement_counter != plaintext->counter)) {
          throw std::runtime_error(
              "ambiguous EventReq AckRsp messages were received");
        }
        transaction_acknowledgement = *ack;
        transaction_acknowledgement_counter = plaintext->counter;
      }
    }
    session.accept_received_counter(plaintext->counter);
  }
  require_healthy("post-event-RX");
  OPENSSL_cleanse(shared.data(), shared.size());
  OPENSSL_cleanse(key.data(), key.size());
  if (transaction_acknowledgement && transaction_acknowledgement_counter) {
    const auto record =
        std::string("{\"rpc_id\":0,\"transaction\":68,\"status\":") +
        std::to_string(transaction_acknowledgement->status) +
        ",\"counter\":" +
        std::to_string(*transaction_acknowledgement_counter) +
        ",\"sense_acknowledged\":" +
        (acknowledgement ? "true" : "false") + "}\n";
    write_exclusive(
        config_.evidence_directory / (stem + "-ack-response.json"),
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(record.data()),
            record.size()));
  }
  if (acknowledgement && acknowledgement_counter) {
    session.complete_event();
    return;
  }
  if (transaction_acknowledgement) {
    if (transaction_acknowledgement->status != 0U) {
      throw std::runtime_error(
          "EventReq AckRsp returned status " +
          std::to_string(transaction_acknowledgement->status));
    }
    session.complete_event();
    return;
  }
  {
    throw std::runtime_error(
        "bounded receive yielded neither EventReq AckRsp nor SenseAckReq");
  }
}

}  // namespace bh61::app
