#include "bh61/evidence/writer.hpp"

#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bh61::evidence {
namespace {

constexpr auto rotate_right(std::uint32_t value, unsigned amount)
    -> std::uint32_t {
  return (value >> amount) | (value << (32U - amount));
}

class Sha256 {
 public:
  void update(std::span<const std::uint8_t> input) {
    total_bytes_ += static_cast<std::uint64_t>(input.size());
    std::size_t offset = 0;
    while (offset < input.size()) {
      const auto count =
          std::min(buffer_.size() - buffer_size_, input.size() - offset);
      std::memcpy(buffer_.data() + buffer_size_, input.data() + offset, count);
      buffer_size_ += count;
      offset += count;
      if (buffer_size_ == buffer_.size()) {
        transform(buffer_);
        buffer_size_ = 0;
      }
    }
  }

  auto finish() -> std::array<std::uint8_t, 32> {
    const auto bit_count = total_bytes_ * 8U;
    buffer_[buffer_size_++] = 0x80U;
    if (buffer_size_ > 56U) {
      while (buffer_size_ < buffer_.size()) {
        buffer_[buffer_size_++] = 0U;
      }
      transform(buffer_);
      buffer_size_ = 0;
    }
    while (buffer_size_ < 56U) {
      buffer_[buffer_size_++] = 0U;
    }
    for (std::size_t index = 0; index < 8U; ++index) {
      const auto shift = static_cast<unsigned>((7U - index) * 8U);
      buffer_[56U + index] =
          static_cast<std::uint8_t>((bit_count >> shift) & 0xffU);
    }
    transform(buffer_);

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t word = 0; word < state_.size(); ++word) {
      for (std::size_t byte = 0; byte < 4U; ++byte) {
        const auto shift = static_cast<unsigned>((3U - byte) * 8U);
        digest[word * 4U + byte] =
            static_cast<std::uint8_t>((state_[word] >> shift) & 0xffU);
      }
    }
    return digest;
  }

 private:
  void transform(const std::array<std::uint8_t, 64>& block) {
    static constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16U; ++index) {
      const auto offset = index * 4U;
      words[index] =
          (static_cast<std::uint32_t>(block[offset]) << 24U) |
          (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
          (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
          static_cast<std::uint32_t>(block[offset + 3U]);
    }
    for (std::size_t index = 16U; index < words.size(); ++index) {
      const auto s0 = rotate_right(words[index - 15U], 7U) ^
                      rotate_right(words[index - 15U], 18U) ^
                      (words[index - 15U] >> 3U);
      const auto s1 = rotate_right(words[index - 2U], 17U) ^
                      rotate_right(words[index - 2U], 19U) ^
                      (words[index - 2U] >> 10U);
      words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto sum1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
                        rotate_right(e, 25U);
      const auto choose = (e & f) ^ ((~e) & g);
      const auto temp1 = h + sum1 + choose + constants[index] + words[index];
      const auto sum0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
                        rotate_right(a, 22U);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_{
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffer_size_{};
  std::uint64_t total_bytes_{};
};

auto json_escape(const std::string& input) -> std::string {
  std::ostringstream output;
  for (const auto value : input) {
    switch (value) {
      case '\\':
        output << "\\\\";
        break;
      case '"':
        output << "\\\"";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        output << value;
        break;
    }
  }
  return output.str();
}

auto boolean_json(bool value) -> const char* { return value ? "true" : "false"; }

void require_stream(const std::ios& stream, const std::string& operation) {
  if (!stream) {
    throw std::runtime_error(operation);
  }
}

}  // namespace

auto sha256_file(const std::filesystem::path& path) -> std::string {
  std::ifstream input(path, std::ios::binary);
  require_stream(input, "cannot open file for SHA-256: " + path.string());
  Sha256 hash;
  std::array<char, 8192> block{};
  while (input) {
    input.read(block.data(), static_cast<std::streamsize>(block.size()));
    const auto count = input.gcount();
    if (count > 0) {
      std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
      std::memcpy(bytes.data(), block.data(), bytes.size());
      hash.update(bytes);
    }
  }
  const auto digest = hash.finish();
  std::ostringstream encoded;
  encoded << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    encoded << std::setw(2) << static_cast<unsigned>(byte);
  }
  return encoded.str();
}

Writer::Writer(std::filesystem::path directory, std::string stem)
    : directory_(std::move(directory)), stem_(std::move(stem)) {
  if (stem_.empty()) {
    throw std::invalid_argument("evidence artifact stem must not be empty");
  }
  std::filesystem::create_directories(directory_);
  artifacts_ = Artifacts{directory_ / (stem_ + ".sigmf-data"),
                         directory_ / (stem_ + ".sigmf-meta"),
                         directory_ / (stem_ + ".events.jsonl"),
                         directory_ / (stem_ + ".reproduce.txt"),
                         directory_ / (stem_ + ".sha256"),
                         directory_ / (stem_ + ".partial.json")};
  partial_data_path_ = directory_ / (stem_ + ".sigmf-data.partial");
  partial_events_path_ = directory_ / (stem_ + ".events.jsonl.partial");
}

Writer::~Writer() {
  if (data_stream_.is_open()) {
    data_stream_.flush();
    data_stream_.close();
  }
  if (events_stream_.is_open()) {
    events_stream_.flush();
    events_stream_.close();
  }
}

void Writer::begin_capture(const CaptureMetadata& metadata) {
  if (capture_started_) {
    throw std::logic_error("evidence capture is already started");
  }
  if (metadata.sample_rate == 0U) {
    throw std::invalid_argument("evidence sample rate must be nonzero");
  }
  if constexpr (std::endian::native != std::endian::little) {
    throw std::runtime_error("SigMF cf32_le writer requires a little-endian host");
  }
  static_assert(sizeof(std::complex<float>) == sizeof(float) * 2U);
  const std::array<std::filesystem::path, 8> conflicts{
      artifacts_.data_path,       artifacts_.meta_path,
      artifacts_.events_path,     artifacts_.reproduction_path,
      artifacts_.manifest_path,   artifacts_.partial_path,
      partial_data_path_,         partial_events_path_};
  for (const auto& path : conflicts) {
    if (std::filesystem::exists(path)) {
      throw std::runtime_error("evidence artifact already exists: " +
                               path.string());
    }
  }
  metadata_ = metadata;
  data_stream_.open(partial_data_path_, std::ios::binary | std::ios::trunc);
  require_stream(data_stream_, "failed to create partial SigMF data");
  events_stream_.open(partial_events_path_,
                      std::ios::binary | std::ios::trunc);
  require_stream(events_stream_, "failed to create partial event stream");
  capture_started_ = true;
  write_partial_state();
}

void Writer::append_samples(std::span<const std::complex<float>> samples,
                            std::uint64_t source_first_sample,
                            std::uint64_t monotonic_time_ns,
                            bool discontinuity,
                            std::string discontinuity_reason) {
  require_active();
  data_stream_.write(reinterpret_cast<const char*>(samples.data()),
                     static_cast<std::streamsize>(samples.size_bytes()));
  require_stream(data_stream_, "failed to append partial SigMF data");
  data_stream_.flush();
  require_stream(data_stream_, "failed to flush partial SigMF data");
  blocks_.push_back(BlockRecord{
      stored_sample_count_, source_first_sample, monotonic_time_ns,
      static_cast<std::uint64_t>(samples.size()), discontinuity,
      std::move(discontinuity_reason)});
  stored_sample_count_ += static_cast<std::uint64_t>(samples.size());
  write_partial_state();
}

void Writer::write_sigmf(std::span<const std::complex<float>> samples,
                         const CaptureMetadata& metadata) {
  begin_capture(metadata);
  append_samples(samples, 0U, metadata.monotonic_start_ns,
                 metadata.discontinuity_at_start,
                 metadata.discontinuity_at_start ? "source_start" : "");
}

void Writer::append_decoded_event(const DecodedEvent& event) {
  require_active();
  events_stream_ << "{\"kind\":\"decoded_frame\",\"utc\":\""
                 << json_escape(event.utc) << "\",\"monotonic_ns\":"
                 << event.monotonic_ns << ",\"frame_hex\":\""
                 << json_escape(event.frame_hex)
                 << "\",\"acquisition_score\":" << std::setprecision(17)
                 << event.acquisition_score << ",\"start_sample\":"
                 << event.start_sample << ",\"conjugated\":"
                 << boolean_json(event.conjugated)
                 << ",\"carrier_offset_hz\":" << event.carrier_offset_hz
                 << "}\n";
  events_stream_.flush();
  require_stream(events_stream_, "failed to append decoded evidence event");
}

void Writer::append_discontinuity(const DiscontinuityEvent& event) {
  require_active();
  events_stream_ << "{\"kind\":\"discontinuity\",\"utc\":\""
                 << json_escape(event.utc) << "\",\"monotonic_ns\":"
                 << event.monotonic_ns << ",\"missing_samples\":"
                 << event.missing_samples << ",\"reason\":\""
                 << json_escape(event.reason) << "\"}\n";
  events_stream_.flush();
  require_stream(events_stream_, "failed to append discontinuity event");
}

auto Writer::finalize(const std::string& reproduction_command) -> Artifacts {
  require_active();
  data_stream_.flush();
  events_stream_.flush();
  require_stream(data_stream_, "failed to flush SigMF data before finalize");
  require_stream(events_stream_,
                 "failed to flush event stream before finalize");
  data_stream_.close();
  events_stream_.close();

  std::filesystem::rename(partial_data_path_, artifacts_.data_path);
  std::filesystem::rename(partial_events_path_, artifacts_.events_path);

  const auto metadata_temporary =
      std::filesystem::path(artifacts_.meta_path.string() + ".tmp");
  write_metadata(metadata_temporary);
  std::filesystem::rename(metadata_temporary, artifacts_.meta_path);

  const auto reproduction_temporary =
      std::filesystem::path(artifacts_.reproduction_path.string() + ".tmp");
  {
    std::ofstream output(reproduction_temporary, std::ios::binary);
    output << reproduction_command << '\n';
    require_stream(output, "failed to write reproduction command");
  }
  std::filesystem::rename(reproduction_temporary,
                          artifacts_.reproduction_path);

  const auto manifest_temporary =
      std::filesystem::path(artifacts_.manifest_path.string() + ".tmp");
  {
    std::ofstream manifest(manifest_temporary, std::ios::binary);
    const std::array<std::filesystem::path, 4> hashed{
        artifacts_.data_path, artifacts_.meta_path, artifacts_.events_path,
        artifacts_.reproduction_path};
    for (const auto& path : hashed) {
      manifest << sha256_file(path) << "  " << path.filename().string() << '\n';
    }
    require_stream(manifest, "failed to write SHA-256 manifest");
  }
  std::filesystem::rename(manifest_temporary, artifacts_.manifest_path);
  std::filesystem::remove(artifacts_.partial_path);
  finalized_ = true;
  return artifacts_;
}

auto Writer::artifacts() const noexcept -> const Artifacts& {
  return artifacts_;
}

void Writer::write_partial_state() {
  std::ofstream partial(artifacts_.partial_path,
                        std::ios::binary | std::ios::trunc);
  partial << "{\"complete\":false,\"stored_sample_count\":"
          << stored_sample_count_ << ",\"data_path\":\""
          << json_escape(partial_data_path_.filename().string())
          << "\",\"events_path\":\""
          << json_escape(partial_events_path_.filename().string()) << "\"}\n";
  require_stream(partial, "failed to write partial evidence state");
}

void Writer::write_metadata(const std::filesystem::path& path) const {
  std::ofstream meta(path, std::ios::binary | std::ios::trunc);
  meta << "{\"global\":{\"core:datatype\":\"cf32_le\","
       << "\"core:version\":\"1.0.0\",\"core:sample_rate\":"
       << metadata_.sample_rate << ",\"bh61:backend\":\""
       << json_escape(metadata_.backend) << "\","
       << "\"bh61:backend_configuration\":\""
       << json_escape(metadata_.backend_configuration) << "\","
       << "\"bh61:stored_sample_count\":" << stored_sample_count_ << "},"
       << "\"captures\":[";
  for (std::size_t index = 0; index < blocks_.size(); ++index) {
    const auto& block = blocks_[index];
    if (index != 0U) {
      meta << ',';
    }
    const auto discontinuity =
        block.discontinuity ||
        (index == 0U && metadata_.discontinuity_at_start);
    meta << "{\"core:sample_start\":" << block.stored_first_sample
         << ",\"core:frequency\":" << metadata_.center_frequency_hz
         << ",\"core:datetime\":\"" << json_escape(metadata_.utc_start)
         << "\",\"bh61:source_sample_start\":"
         << block.source_first_sample << ",\"bh61:sample_count\":"
         << block.sample_count << ",\"bh61:monotonic_start_ns\":"
         << block.monotonic_time_ns << ",\"bh61:discontinuity\":"
         << boolean_json(discontinuity);
    if (!block.discontinuity_reason.empty()) {
      meta << ",\"bh61:discontinuity_reason\":\""
           << json_escape(block.discontinuity_reason) << '"';
    }
    meta << '}';
  }
  meta << "],\"annotations\":[]}\n";
  require_stream(meta, "failed to write SigMF metadata");
}

void Writer::require_active() const {
  if (!capture_started_) {
    throw std::logic_error("evidence capture is not started");
  }
  if (finalized_) {
    throw std::logic_error("evidence capture is already finalized");
  }
}

}  // namespace bh61::evidence
