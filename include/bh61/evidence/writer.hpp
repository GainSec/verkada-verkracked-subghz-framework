#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace bh61::evidence {

struct CaptureMetadata {
  std::uint32_t sample_rate{};
  std::uint64_t center_frequency_hz{};
  std::string backend;
  std::string backend_configuration;
  std::string utc_start;
  std::uint64_t monotonic_start_ns{};
  bool discontinuity_at_start{};
};

struct DecodedEvent {
  std::string utc;
  std::uint64_t monotonic_ns{};
  std::string frame_hex;
  double acquisition_score{};
  std::size_t start_sample{};
  bool conjugated{};
  double carrier_offset_hz{};
};

struct DiscontinuityEvent {
  std::string utc;
  std::uint64_t monotonic_ns{};
  std::uint64_t missing_samples{};
  std::string reason;
};

struct Artifacts {
  std::filesystem::path data_path;
  std::filesystem::path meta_path;
  std::filesystem::path events_path;
  std::filesystem::path reproduction_path;
  std::filesystem::path manifest_path;
  std::filesystem::path partial_path;
};

auto sha256_file(const std::filesystem::path& path) -> std::string;

class Writer {
 public:
  Writer(std::filesystem::path directory, std::string stem);
  ~Writer();

  Writer(const Writer&) = delete;
  auto operator=(const Writer&) -> Writer& = delete;

  void begin_capture(const CaptureMetadata& metadata);
  void append_samples(std::span<const std::complex<float>> samples,
                      std::uint64_t source_first_sample,
                      std::uint64_t monotonic_time_ns, bool discontinuity,
                      std::string discontinuity_reason);
  void write_sigmf(std::span<const std::complex<float>> samples,
                   const CaptureMetadata& metadata);
  void append_decoded_event(const DecodedEvent& event);
  void append_discontinuity(const DiscontinuityEvent& event);
  auto finalize(const std::string& reproduction_command) -> Artifacts;
  auto artifacts() const noexcept -> const Artifacts&;

 private:
  struct BlockRecord {
    std::uint64_t stored_first_sample{};
    std::uint64_t source_first_sample{};
    std::uint64_t monotonic_time_ns{};
    std::uint64_t sample_count{};
    bool discontinuity{};
    std::string discontinuity_reason;
  };

  void write_partial_state();
  void write_metadata(const std::filesystem::path& path) const;
  void require_active() const;

  std::filesystem::path directory_;
  std::string stem_;
  Artifacts artifacts_;
  std::filesystem::path partial_data_path_;
  std::filesystem::path partial_events_path_;
  CaptureMetadata metadata_;
  std::vector<BlockRecord> blocks_;
  std::ofstream data_stream_;
  std::ofstream events_stream_;
  std::uint64_t stored_sample_count_{};
  bool capture_started_{};
  bool finalized_{};
};

}  // namespace bh61::evidence
