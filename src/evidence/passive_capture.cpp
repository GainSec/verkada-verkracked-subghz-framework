#include "bh61/evidence/passive_capture.hpp"

#include "bh61/core/frame.hpp"
#include "bh61/core/vcmp.hpp"
#include "bh61/core/vmac.hpp"

#include <bit>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace bh61::evidence {
namespace {

auto read_text(const std::filesystem::path& path) -> std::optional<std::string> {
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

auto value_offset(std::string_view json, std::string_view key)
    -> std::optional<std::size_t> {
  const auto marker = std::string("\"") + std::string(key) + "\"";
  const auto found = json.find(marker);
  if (found == std::string_view::npos) return std::nullopt;
  auto offset = json.find(':', found + marker.size());
  if (offset == std::string_view::npos) return std::nullopt;
  ++offset;
  while (offset < json.size() && std::isspace(static_cast<unsigned char>(json[offset]))) {
    ++offset;
  }
  return offset;
}

auto json_string(std::string_view json, std::string_view key)
    -> std::optional<std::string> {
  const auto start_value = value_offset(json, key);
  if (!start_value || *start_value >= json.size() || json[*start_value] != '"') {
    return std::nullopt;
  }
  std::string result;
  bool escaped = false;
  for (auto offset = *start_value + 1U; offset < json.size(); ++offset) {
    const auto value = json[offset];
    if (escaped) {
      if (value == 'n') result.push_back('\n');
      else if (value == 'r') result.push_back('\r');
      else if (value == 't') result.push_back('\t');
      else if (value == '"' || value == '\\' || value == '/') result.push_back(value);
      else return std::nullopt;
      escaped = false;
    } else if (value == '\\') {
      escaped = true;
    } else if (value == '"') {
      return result;
    } else {
      result.push_back(value);
    }
  }
  return std::nullopt;
}

template <typename Integer>
auto json_integer(std::string_view json, std::string_view key)
    -> std::optional<Integer> {
  const auto start = value_offset(json, key);
  if (!start) return std::nullopt;
  auto end = *start;
  while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end]))) ++end;
  if (end == *start) return std::nullopt;
  Integer result{};
  const auto parsed = std::from_chars(json.data() + *start, json.data() + end, result);
  if (parsed.ec != std::errc{} || parsed.ptr != json.data() + end) return std::nullopt;
  return result;
}

auto json_double(std::string_view json, std::string_view key)
    -> std::optional<double> {
  const auto start = value_offset(json, key);
  if (!start) return std::nullopt;
  auto end = *start;
  while (end < json.size()) {
    const auto value = json[end];
    if (!(std::isdigit(static_cast<unsigned char>(value)) || value == '-' ||
          value == '+' || value == '.' || value == 'e' || value == 'E')) break;
    ++end;
  }
  if (end == *start) return std::nullopt;
  try {
    std::size_t consumed{};
    const auto text = std::string(json.substr(*start, end - *start));
    const auto result = std::stod(text, &consumed);
    if (consumed != text.size()) return std::nullopt;
    return result;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

auto hex_value(char value) -> int {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

auto frame_type(std::string_view encoded) -> std::optional<std::uint8_t> {
  if (encoded.size() % 2U != 0U) return std::nullopt;
  std::vector<std::uint8_t> bytes;
  bytes.reserve(encoded.size() / 2U);
  for (std::size_t offset = 0; offset < encoded.size(); offset += 2U) {
    const auto high = hex_value(encoded[offset]);
    const auto low = hex_value(encoded[offset + 1U]);
    if (high < 0 || low < 0) return std::nullopt;
    bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  const auto radio = core::parse_radio_frame(bytes);
  if (!std::holds_alternative<core::RadioFrame>(radio)) return std::nullopt;
  const auto vmac = core::parse_vmac(std::get<core::RadioFrame>(radio).psdu);
  if (!std::holds_alternative<core::VmacFrame>(vmac)) return std::nullopt;
  const auto vcmp = core::parse_vcmp(std::get<core::VmacFrame>(vmac).payload);
  if (!std::holds_alternative<core::VcmpFrame>(vcmp)) return std::nullopt;
  return std::get<core::VcmpFrame>(vcmp).type;
}

auto catalog_event(std::uint8_t type) -> std::string {
  switch (type) {
    case 0: return "bulletin-rx";
    case 1: return "management-rpc-rx";
    case 4: return "counter-sync";
    case 7: return "type7-join";
    case 11: return "scan-info-rx";
    default: return "outer-type-unmapped";
  }
}

auto ingest_lines(const std::filesystem::path& path,
                  std::uint32_t fallback_rate,
                  std::uint64_t fallback_frequency,
                  std::string_view fallback_utc) -> PassiveIngestResult {
  std::ifstream input(path);
  if (!input) {
    return PassiveIngestError{"open", "cannot open JSONL input: " + path.string(), 0U};
  }
  PassiveCapture capture;
  capture.source = "jsonl";
  capture.sample_rate = fallback_rate;
  capture.center_frequency_hz = fallback_frequency;
  capture.utc_start = std::string(fallback_utc);
  std::string line;
  std::size_t line_number{};
  while (std::getline(input, line)) {
    ++line_number;
    if (line.empty()) continue;
    if (line.front() != '{' || line.back() != '}') {
      return PassiveIngestError{"json", "line is not a JSON object", line_number};
    }
    const auto kind = json_string(line, "kind");
    if (kind && *kind != "decoded_frame") continue;
    const auto encoded = json_string(line, "frame_hex");
    if (!encoded) {
      return PassiveIngestError{"frame", "decoded row omits frame_hex", line_number};
    }
    const auto type = frame_type(*encoded);
    if (!type) {
      return PassiveIngestError{"frame", "frame_hex is not a valid BH61 frame", line_number};
    }
    const auto rate = json_integer<std::uint32_t>(line, "sample_rate").value_or(fallback_rate);
    const auto frequency = json_integer<std::uint64_t>(line, "center_frequency_hz").value_or(fallback_frequency);
    if (rate == 0U || frequency == 0U) {
      return PassiveIngestError{"metadata", "sample rate and center frequency are required", line_number};
    }
    PassiveFrameRecord record;
    record.utc = json_string(line, "utc").value_or(std::string(fallback_utc));
    record.monotonic_ns = json_integer<std::uint64_t>(line, "monotonic_ns").value_or(0U);
    record.center_frequency_hz = frequency;
    record.sample_rate = rate;
    record.rssi_dbm = json_double(line, "rssi_dbm");
    record.frame_hex = *encoded;
    record.vcmp_type = *type;
    record.catalog_event = catalog_event(*type);
    capture.frames.push_back(std::move(record));
  }
  if (!input.eof()) {
    return PassiveIngestError{"read", "failed while reading JSONL input", line_number};
  }
  return capture;
}

}  // namespace

auto ingest_frame_jsonl(const std::filesystem::path& path)
    -> PassiveIngestResult {
  return ingest_lines(path, 0U, 0U, "");
}

auto ingest_sigmf(const std::filesystem::path& metadata,
                  const std::filesystem::path& data,
                  const std::filesystem::path& events)
    -> PassiveIngestResult {
  if constexpr (std::endian::native != std::endian::little) {
    return PassiveIngestError{"datatype", "cf32_le requires a little-endian host", 0U};
  }
  const auto text = read_text(metadata);
  if (!text) return PassiveIngestError{"open", "cannot open SigMF metadata", 0U};
  const auto datatype = json_string(*text, "core:datatype");
  if (!datatype || *datatype != "cf32_le") {
    return PassiveIngestError{"datatype", "only SigMF cf32_le is supported", 0U};
  }
  const auto rate = json_integer<std::uint32_t>(*text, "core:sample_rate");
  const auto frequency = json_integer<std::uint64_t>(*text, "core:frequency");
  const auto utc = json_string(*text, "core:datetime");
  if (!rate || *rate == 0U || !frequency || *frequency == 0U || !utc) {
    return PassiveIngestError{"metadata", "SigMF rate, frequency, or datetime is invalid", 0U};
  }
  std::error_code size_error;
  const auto bytes = std::filesystem::file_size(data, size_error);
  if (size_error) return PassiveIngestError{"open", "cannot stat SigMF data", 0U};
  constexpr std::uint64_t bytes_per_sample = sizeof(float) * 2U;
  if (bytes % bytes_per_sample != 0U) {
    return PassiveIngestError{"length", "SigMF data has a partial cf32 sample", 0U};
  }
  const auto samples = bytes / bytes_per_sample;
  const auto declared = json_integer<std::uint64_t>(*text, "bh61:stored_sample_count");
  if (declared && *declared != samples) {
    return PassiveIngestError{"length", "SigMF stored sample count differs from data", 0U};
  }
  auto event_result = ingest_lines(events, *rate, *frequency, *utc);
  if (std::holds_alternative<PassiveIngestError>(event_result)) return event_result;
  auto capture = std::get<PassiveCapture>(std::move(event_result));
  capture.source = "sigmf+jsonl";
  capture.sample_rate = *rate;
  capture.center_frequency_hz = *frequency;
  capture.utc_start = *utc;
  capture.sample_count = samples;
  return capture;
}

auto collect_receive_only(radio::Device& device, std::size_t maximum_samples)
    -> std::vector<std::complex<float>> {
  std::vector<std::complex<float>> result;
  while (result.size() < maximum_samples) {
    const auto remaining = maximum_samples - result.size();
    auto block = device.receive(remaining);
    if (block.samples.empty()) break;
    if (block.samples.size() > remaining) {
      throw std::runtime_error("device returned more samples than requested");
    }
    result.insert(result.end(), block.samples.begin(), block.samples.end());
  }
  return result;
}

}  // namespace bh61::evidence
