#pragma once

#include "bh61/radio/device.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace bh61::evidence {

struct PassiveFrameRecord {
  std::string utc;
  std::uint64_t monotonic_ns{};
  std::uint64_t center_frequency_hz{};
  std::uint32_t sample_rate{};
  std::optional<double> rssi_dbm;
  std::string frame_hex;
  std::uint8_t vcmp_type{};
  std::string catalog_event;
};

struct PassiveCapture {
  std::string source;
  std::string utc_start;
  std::uint64_t center_frequency_hz{};
  std::uint32_t sample_rate{};
  std::uint64_t sample_count{};
  std::vector<PassiveFrameRecord> frames;
};

struct PassiveIngestError {
  std::string code;
  std::string message;
  std::size_t line{};
};

using PassiveIngestResult = std::variant<PassiveCapture, PassiveIngestError>;

auto ingest_sigmf(const std::filesystem::path& metadata,
                  const std::filesystem::path& data,
                  const std::filesystem::path& events)
    -> PassiveIngestResult;
auto ingest_frame_jsonl(const std::filesystem::path& path)
    -> PassiveIngestResult;
auto collect_receive_only(radio::Device& device, std::size_t maximum_samples)
    -> std::vector<std::complex<float>>;

}  // namespace bh61::evidence
