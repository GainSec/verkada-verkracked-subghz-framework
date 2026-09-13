#include "bh61/app/arguments.hpp"
#include "bh61/app/sensor_session_runner.hpp"

#include "bh61/core/frame.hpp"
#include "bh61/core/sensor_model.hpp"
#include "bh61/core/session.hpp"
#include "bh61/core/sensor_session.hpp"
#include "bh61/core/vcmp.hpp"
#include "bh61/core/vmac.hpp"
#include "bh61/dsp/demodulator.hpp"
#include "bh61/dsp/modulator.hpp"
#include "bh61/evidence/writer.hpp"
#include "bh61/evidence/lab_event.hpp"
#include "bh61/radio/file_device.hpp"
#include "bh61/version.hpp"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <climits>
#include <cctype>
#include <complex>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace bh61::app {
namespace {

auto command_from_name(std::string_view name) -> std::optional<Command> {
  if (name == "--help" || name == "help") return Command::Help;
  if (name == "--version") return Command::Version;
  if (name == "devices") return Command::Devices;
  if (name == "inspect") return Command::Inspect;
  if (name == "build") return Command::Build;
  if (name == "secure-frame") return Command::SecureFrame;
  if (name == "join-frame") return Command::JoinFrame;
  if (name == "scan-complete-frame") return Command::ScanCompleteFrame;
  if (name == "waveform") return Command::Waveform;
  if (name == "decode") return Command::Decode;
  if (name == "capture") return Command::Capture;
  if (name == "scan") return Command::Scan;
  if (name == "fixtures") return Command::Fixtures;
  if (name == "transmit") return Command::Transmit;
  if (name == "simulate") return Command::Simulate;
  if (name == "pcap") return Command::Pcap;
  if (name == "df") return Command::Df;
  if (name == "df-capture") return Command::DfCapture;
  if (name == "coverage") return Command::Coverage;
  if (name == "sensor-create") return Command::SensorCreate;
  if (name == "sensor-join") return Command::SensorJoin;
  if (name == "sensor-event") return Command::SensorEvent;
  if (name == "sensor-rpc-read") return Command::SensorRpcRead;
  if (name == "sensor-status") return Command::SensorStatus;
  if (name == "sensor-retire") return Command::SensorRetire;
  return std::nullopt;
}

template <typename Integer>
auto parse_integer(std::string_view value, std::string_view option)
    -> std::variant<Integer, ArgumentError> {
  Integer parsed{};
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    return ArgumentError{"invalid_value",
                         std::string(option) + " requires an unsigned integer"};
  }
  return parsed;
}

auto required_value(std::span<const std::string_view> arguments,
                    std::size_t& index, std::string_view option)
    -> std::variant<std::string, ArgumentError> {
  if (index + 1U >= arguments.size()) {
    return ArgumentError{"missing_value",
                         std::string(option) + " requires a value"};
  }
  ++index;
  return std::string(arguments[index]);
}

auto missing(std::string_view option) -> ArgumentResult {
  return ArgumentError{"missing_option",
                       std::string("required option is absent: ") +
                           std::string(option)};
}

auto hex_value(char value) -> int {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

auto decode_hex(std::string_view encoded) -> std::vector<std::uint8_t> {
  if (encoded.size() % 2U != 0U) {
    throw std::invalid_argument("hex string has an odd number of digits");
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve(encoded.size() / 2U);
  for (std::size_t index = 0; index < encoded.size(); index += 2U) {
    const auto high = hex_value(encoded[index]);
    const auto low = hex_value(encoded[index + 1U]);
    if (high < 0 || low < 0) {
      throw std::invalid_argument("hex string contains a non-hex character");
    }
    bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return bytes;
}

auto encode_hex(std::span<const std::uint8_t> bytes) -> std::string {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : bytes) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
}

template <std::size_t Size>
auto decode_fixed_hex(std::string_view encoded, std::string_view option)
    -> std::array<std::uint8_t, Size> {
  const auto bytes = decode_hex(encoded);
  if (bytes.size() != Size) {
    throw std::invalid_argument(std::string(option) + " must contain exactly " +
                                std::to_string(Size) + " bytes");
  }
  std::array<std::uint8_t, Size> result{};
  std::copy(bytes.begin(), bytes.end(), result.begin());
  return result;
}

auto build_secure_frame(const Arguments& arguments) -> std::vector<std::uint8_t> {
  const auto source = decode_fixed_hex<8>(*arguments.source_eui, "--source-eui");
  const auto key = decode_fixed_hex<16>(*arguments.key_hex, "--key-hex");
  const auto iv = decode_fixed_hex<16>(*arguments.iv_hex, "--iv-hex");
  const auto payload = decode_hex(*arguments.payload_hex);
  const auto protected_body = core::seal_vcmp(
      static_cast<std::uint8_t>(*arguments.type),
      static_cast<std::uint8_t>(*arguments.flags),
      static_cast<std::uint16_t>(*arguments.counter), payload, key, iv);
  core::VmacFrame mac;
  mac.frame_control = 0xc841U;
  mac.sequence = static_cast<std::uint8_t>(*arguments.sequence);
  mac.destination_pan = static_cast<std::uint16_t>(*arguments.pan);
  mac.destination = static_cast<std::uint16_t>(*arguments.destination);
  mac.source_eui = source;
  mac.payload = core::encode_vcmp(protected_body);
  core::RadioFrame radio;
  radio.psdu = core::encode_vmac(mac);
  return core::encode_radio_frame(radio);
}

auto build_join_frame(const Arguments& arguments) -> std::vector<std::uint8_t> {
  const auto source = decode_fixed_hex<8>(*arguments.source_eui, "--source-eui");
  const auto serial = decode_hex(*arguments.serial_hex);
  if (serial.empty() || serial.size() > 90U) {
    throw std::invalid_argument("--serial-hex must contain 1-90 bytes");
  }
  std::vector<std::uint8_t> join_payload{0x04U};
  join_payload.insert(join_payload.end(), 13U, 0x00U);
  join_payload.push_back(static_cast<std::uint8_t>(serial.size()));
  join_payload.insert(join_payload.end(), serial.begin(), serial.end());
  join_payload.insert(join_payload.end(), 8U, 0x00U);
  join_payload.push_back(0x00U);
  join_payload.push_back(0x01U);

  core::VcmpFrame join;
  join.type = 7U;
  join.flags = 0U;
  join.body = core::VcmpPlaintext{std::move(join_payload)};
  core::VmacFrame mac;
  mac.frame_control = 0xc841U;
  mac.sequence = static_cast<std::uint8_t>(*arguments.sequence);
  mac.destination_pan = static_cast<std::uint16_t>(*arguments.pan);
  mac.destination = static_cast<std::uint16_t>(*arguments.destination);
  mac.source_eui = source;
  mac.payload = core::encode_vcmp(join);
  core::RadioFrame radio;
  radio.psdu = core::encode_vmac(mac);
  return core::encode_radio_frame(radio);
}

auto build_scan_complete_frame(const Arguments& arguments)
    -> std::vector<std::uint8_t> {
  const auto source = decode_fixed_hex<8>(*arguments.source_eui, "--source-eui");
  const auto channel = static_cast<std::uint16_t>(*arguments.channel);
  core::VcmpFrame scan;
  scan.type = 11U;
  scan.flags = 0U;
  scan.body = core::VcmpPlaintext{{
      static_cast<std::uint8_t>(channel >> 8U),
      static_cast<std::uint8_t>(channel),
      static_cast<std::uint8_t>(*arguments.regulatory_code), 0x00U,
      static_cast<std::uint8_t>(*arguments.scan_sequence)}};
  core::VmacFrame mac;
  mac.frame_control = 0xc841U;
  mac.sequence = static_cast<std::uint8_t>(*arguments.sequence);
  mac.destination_pan = static_cast<std::uint16_t>(*arguments.pan);
  mac.destination = static_cast<std::uint16_t>(*arguments.destination);
  mac.source_eui = source;
  mac.payload = core::encode_vcmp(scan);
  core::RadioFrame radio;
  radio.psdu = core::encode_vmac(mac);
  return core::encode_radio_frame(radio);
}

auto json_escape(std::string_view input) -> std::string {
  std::ostringstream output;
  for (const auto value : input) {
    if (value == '\\') {
      output << "\\\\";
    } else if (value == '"') {
      output << "\\\"";
    } else if (value == '\n') {
      output << "\\n";
    } else {
      output << value;
    }
  }
  return output.str();
}

auto sample_format_name(radio::SampleFormat format) -> std::string_view {
  switch (format) {
    case radio::SampleFormat::ComplexFloat32:
      return "cf32";
    case radio::SampleFormat::SignedInt8:
      return "s8";
    case radio::SampleFormat::UnsignedInt8:
      return "u8";
  }
  return "unknown";
}

void structured_error(std::ostream& error, std::string_view code,
                      std::string_view message) {
  error << "{\"error\":{\"code\":\"" << json_escape(code)
        << "\",\"message\":\"" << json_escape(message) << "\"}}\n";
}

void random_bytes(std::span<std::uint8_t> bytes) {
  if (bytes.empty() || bytes.size() > static_cast<std::size_t>(INT_MAX) ||
      RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    throw std::runtime_error("operating-system random generation failed");
  }
}

void write_private_text(const std::filesystem::path& path,
                        std::string_view text) {
  if (std::filesystem::exists(path)) {
    throw std::runtime_error("refusing to overwrite evidence: " +
                             path.string());
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) throw std::runtime_error("cannot create " + path.string());
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  stream.close();
  if (!stream) throw std::runtime_error("cannot write " + path.string());
  std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);
}

auto isolation_preflight_status(const std::filesystem::path& path)
    -> RunnerOracleStatus {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size == 0U || size > 1'048'576U) {
    return {false, "preflight evidence is missing, empty, or oversized"};
  }
  std::ifstream stream(path, std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(stream),
                         std::istreambuf_iterator<char>()};
  std::string compact;
  compact.reserve(text.size());
  std::copy_if(text.begin(), text.end(), std::back_inserter(compact),
               [](unsigned char value) { return std::isspace(value) == 0; });
  if (!stream ||
      compact.find("\"schema\":\"bh61.isolation-preflight/v1\"") ==
          std::string::npos ||
      compact.find("\"overall\":\"pass\"") == std::string::npos) {
    return {false, "preflight evidence does not report a schema-v1 pass"};
  }
  return {true, "isolation preflight passed"};
}

class DryRunSensorFrameRadio final : public SensorFrameRadio {
 public:
  void transmit_frame(std::span<const std::uint8_t>,
                      std::span<const std::complex<float>>) override {
    throw std::logic_error("dry-run attempted radio transmission");
  }
  auto receive_frames(std::size_t, std::chrono::milliseconds)
      -> std::vector<std::vector<std::uint8_t>> override {
    throw std::logic_error("dry-run attempted radio reception");
  }
};

auto session_is_retired(const std::filesystem::path& session) -> bool {
  return std::filesystem::exists(session / "retired.json");
}

auto read_cf32(const std::filesystem::path& path)
    -> std::vector<std::complex<float>> {
  if constexpr (std::endian::native != std::endian::little) {
    throw std::runtime_error("cf32_le input requires a little-endian host");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open IQ input: " + path.string());
  }
  input.seekg(0, std::ios::end);
  const auto byte_count = input.tellg();
  if (byte_count < 0 ||
      byte_count % static_cast<std::streamoff>(sizeof(std::complex<float>)) !=
          0) {
    throw std::runtime_error("IQ input size is not a whole number of cf32 samples");
  }
  input.seekg(0, std::ios::beg);
  const auto sample_count = static_cast<std::size_t>(
      byte_count / static_cast<std::streamoff>(sizeof(std::complex<float>)));
  std::vector<std::complex<float>> samples(sample_count);
  input.read(reinterpret_cast<char*>(samples.data()), byte_count);
  if (!input) {
    throw std::runtime_error("failed while reading IQ input");
  }
  return samples;
}

struct IqInput {
  std::vector<std::complex<float>> samples;
  std::uint32_t sample_rate{};
};

auto read_iq_input(const Arguments& arguments) -> IqInput {
  auto path = std::filesystem::path(*arguments.input);
  auto sample_rate = arguments.sample_rate;
  if (path.extension() == ".sigmf-meta") {
    std::ifstream metadata(path, std::ios::binary);
    if (!metadata) {
      throw std::runtime_error("cannot open SigMF metadata: " + path.string());
    }
    const std::string text((std::istreambuf_iterator<char>(metadata)),
                           std::istreambuf_iterator<char>());
    constexpr std::string_view key = "\"core:sample_rate\":";
    const auto key_offset = text.find(key);
    if (key_offset == std::string::npos) {
      throw std::runtime_error("SigMF metadata omits core:sample_rate");
    }
    const auto number_offset = key_offset + key.size();
    const auto number_end = text.find_first_not_of("0123456789", number_offset);
    const auto encoded_rate = std::string_view(text).substr(
        number_offset, number_end == std::string::npos
                           ? std::string::npos
                           : number_end - number_offset);
    const auto parsed_rate =
        parse_integer<std::uint32_t>(encoded_rate, "core:sample_rate");
    if (std::holds_alternative<ArgumentError>(parsed_rate) ||
        std::get<std::uint32_t>(parsed_rate) == 0U) {
      throw std::runtime_error("SigMF core:sample_rate is invalid");
    }
    const auto metadata_rate = std::get<std::uint32_t>(parsed_rate);
    if (arguments.sample_rate_explicit && sample_rate != metadata_rate) {
      throw std::runtime_error(
          "--sample-rate conflicts with SigMF core:sample_rate");
    }
    sample_rate = metadata_rate;
    path.replace_extension(".sigmf-data");
  }
  return IqInput{read_cf32(path), sample_rate};
}

void write_cf32(const std::filesystem::path& path,
                std::span<const std::complex<float>> samples) {
  if constexpr (std::endian::native != std::endian::little) {
    throw std::runtime_error("cf32_le output requires a little-endian host");
  }
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(samples.data()),
               static_cast<std::streamsize>(samples.size_bytes()));
  if (!output) {
    throw std::runtime_error("failed while writing IQ output");
  }
}

auto inspect_frame(std::span<const std::uint8_t> bytes, bool jsonl,
                   std::ostream& output) -> int {
  const auto radio = core::parse_radio_frame(bytes);
  if (std::holds_alternative<core::ParseError>(radio)) {
    const auto& failure = std::get<core::ParseError>(radio);
    throw std::runtime_error(failure.layer + " at " +
                             std::to_string(failure.offset) + ": " +
                             failure.reason);
  }
  const auto& frame = std::get<core::RadioFrame>(radio);
  const auto vmac = core::parse_vmac(frame.psdu);
  if (std::holds_alternative<core::ParseError>(vmac)) {
    const auto& failure = std::get<core::ParseError>(vmac);
    throw std::runtime_error(failure.layer + " at " +
                             std::to_string(failure.offset) + ": " +
                             failure.reason);
  }
  const auto& mac = std::get<core::VmacFrame>(vmac);
  const auto vcmp = core::parse_vcmp(mac.payload);
  if (jsonl) {
    output << "{\"frame_hex\":\"" << encode_hex(bytes)
           << "\",\"phr\":" << static_cast<unsigned>(frame.phr)
           << ",\"radio_fcs_valid\":true,\"vmac_sequence\":"
           << static_cast<unsigned>(mac.sequence);
    if (std::holds_alternative<core::VcmpFrame>(vcmp)) {
      output << ",\"vcmp_type\":"
             << static_cast<unsigned>(std::get<core::VcmpFrame>(vcmp).type);
    }
    output << "}\n";
  } else {
    output << "frame_hex=" << encode_hex(bytes) << '\n'
           << "phr=" << static_cast<unsigned>(frame.phr) << '\n'
           << "radio_fcs=valid\n"
           << "vmac.sequence=" << static_cast<unsigned>(mac.sequence) << '\n';
    if (std::holds_alternative<core::VcmpFrame>(vcmp)) {
      output << "vcmp.type="
             << static_cast<unsigned>(std::get<core::VcmpFrame>(vcmp).type)
             << '\n';
    }
  }
  return 0;
}

auto decode_iq(const Arguments& arguments, std::ostream& output,
               bool scan_mode) -> int {
  const auto input = read_iq_input(arguments);
  dsp::DecodeOptions options;
  options.carrier_hypotheses_hz = {arguments.carrier_offset_hz};
  const auto decoded = dsp::decode_first_burst(input.samples,
                                                input.sample_rate, options);
  if (std::holds_alternative<dsp::DecodeError>(decoded)) {
    const auto& failure = std::get<dsp::DecodeError>(decoded);
    throw std::runtime_error("decode at " + std::to_string(failure.offset) +
                             ": " + failure.reason);
  }
  const auto& burst = std::get<dsp::DecodedBurst>(decoded);
  if (arguments.jsonl) {
    output << "{\"kind\":\"decoded_frame\",\"frame_hex\":\""
           << encode_hex(burst.frame_bytes)
           << "\",\"radio_fcs_valid\":true,\"start_sample\":"
           << burst.acquisition.start_sample << ",\"score\":"
           << std::setprecision(17) << burst.acquisition.score
           << ",\"conjugated\":"
           << (burst.acquisition.conjugated ? "true" : "false")
           << ",\"carrier_offset_hz\":"
           << burst.acquisition.carrier_offset_hz << "}\n";
  } else {
    output << (scan_mode ? "scan.frame=" : "frame=")
           << encode_hex(burst.frame_bytes) << '\n'
           << "radio_fcs=valid\n"
           << "start_sample=" << burst.acquisition.start_sample << '\n'
           << "score=" << std::setprecision(17) << burst.acquisition.score
           << '\n';
  }
  return 0;
}

auto transmit_iq(const Arguments& arguments, radio::Device* device,
                 std::ostream& output) -> int {
  const auto directory = std::filesystem::path(*arguments.output);
  std::filesystem::create_directories(directory);
  const auto record_path = directory / (arguments.stem + ".tx.json");
  const auto manifest_path = directory / (arguments.stem + ".tx.sha256");
  const auto event_path = directory / (arguments.stem + ".events.jsonl");
  const auto generated_path =
      directory / (arguments.stem + ".waveform.cf32");
  if (std::filesystem::exists(record_path) ||
      std::filesystem::exists(manifest_path) ||
      std::filesystem::exists(event_path) ||
      (arguments.frame_hex && std::filesystem::exists(generated_path))) {
    throw std::runtime_error("TX evidence artifact already exists");
  }
  IqInput input;
  std::filesystem::path input_path;
  bool hardware_exact = false;
  std::string_view waveform_model = "external-iq";
  double applied_amplitude = 1.0;
  std::size_t frame_count = 0U;
  if (arguments.frame_hex) {
    std::vector<std::string_view> encoded_frames;
    std::string_view remaining = *arguments.frame_hex;
    while (true) {
      const auto separator = remaining.find(',');
      encoded_frames.push_back(remaining.substr(0, separator));
      if (separator == std::string_view::npos) break;
      remaining.remove_prefix(separator + 1U);
    }
    if (encoded_frames.empty() || encoded_frames.size() > 100U ||
        std::any_of(encoded_frames.begin(), encoded_frames.end(),
                    [](const auto value) { return value.empty(); })) {
      throw std::runtime_error("TX frame sequence must contain 1-100 frames");
    }
    frame_count = encoded_frames.size();
    const auto gap_samples = static_cast<std::size_t>(
        (arguments.interval_ms * arguments.sample_rate) / 1000U);
    for (std::size_t index = 0; index < encoded_frames.size(); ++index) {
      const auto frame = decode_hex(encoded_frames[index]);
      if (std::holds_alternative<core::ParseError>(
              core::parse_radio_frame(frame))) {
        throw std::runtime_error("TX input is not a valid BH61 radio frame");
      }
      auto waveform =
          dsp::modulate_efr32_custom_oqpsk(frame, arguments.sample_rate);
      if (index != 0U) input.samples.insert(input.samples.end(), gap_samples, {});
      for (const auto sample : waveform.samples) {
        input.samples.push_back(sample * static_cast<float>(arguments.amplitude));
      }
    }
    applied_amplitude = arguments.amplitude;
    input.sample_rate = arguments.sample_rate;
    hardware_exact = true;
    waveform_model = "efr32-custom-oqpsk-mode2";
    input_path = generated_path;
    write_cf32(input_path, input.samples);
  } else {
    input = read_iq_input(arguments);
    input_path = *arguments.input;
  }
  if (arguments.center_frequency_hz < 902'000'000U ||
      arguments.center_frequency_hz > 928'000'000U) {
    throw std::runtime_error(
        "TX center frequency is outside the BH61 902-928 MHz profile");
  }
  if (input.samples.empty()) {
    throw std::runtime_error("TX waveform contains no samples");
  }
  constexpr std::uint64_t max_duration_ns = 10'000'000'000ULL;
  const auto duration_ns =
      (static_cast<std::uint64_t>(input.samples.size()) * 1'000'000'000ULL) /
      input.sample_rate;
  if (duration_ns > max_duration_ns) {
    throw std::runtime_error("TX waveform exceeds the 10 second limit");
  }
  if (arguments.repeat_count == 0U || arguments.repeat_count > 100U) {
    throw std::runtime_error("TX repeat count must be between 1 and 100");
  }
  if (arguments.interval_ms > 60'000U) {
    throw std::runtime_error("TX interval exceeds the 60 second limit");
  }
  if (duration_ns * arguments.repeat_count > 60'000'000'000ULL) {
    throw std::runtime_error("total TX airtime exceeds the 60 second limit");
  }

  if (!arguments.dry_run) {
    if (device == nullptr) {
      throw std::runtime_error("transmit requires a selected TX device");
    }
    if (!device->capabilities().tx) {
      throw std::runtime_error("selected device does not support transmit");
    }
    for (std::size_t repetition = 0; repetition < arguments.repeat_count;
         ++repetition) {
      auto requested_time_ns = arguments.requested_time_ns;
      if (requested_time_ns != 0U) {
        requested_time_ns += repetition *
            (duration_ns + arguments.interval_ms * 1'000'000ULL);
      }
      device->transmit(input.samples, requested_time_ns);
      if (repetition + 1U < arguments.repeat_count &&
          arguments.interval_ms != 0U) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(arguments.interval_ms));
      }
    }
  }
  const auto waveform_hash = evidence::sha256_file(input_path);
  {
    std::ofstream record(record_path, std::ios::binary);
    record << "{\"schema\":\"bh61.tx.record/v1\",\"utc\":\""
           << json_escape(arguments.utc) << "\",\"backend\":\""
           << json_escape(arguments.backend.value_or("selected-device"))
           << "\",\"dry_run\":"
           << (arguments.dry_run ? "true" : "false")
           << ",\"acknowledged\":"
           << (arguments.enable_tx ? "true" : "false")
           << ",\"sample_rate\":" << input.sample_rate
           << ",\"center_frequency_hz\":"
           << arguments.center_frequency_hz << ",\"tx_gain_db\":"
           << arguments.tx_gain_db << ",\"sample_count\":"
           << input.samples.size() << ",\"duration_ns\":" << duration_ns
           << ",\"frame_count\":" << frame_count
           << ",\"repeat_count\":" << arguments.repeat_count
           << ",\"interval_ms\":" << arguments.interval_ms
           << ",\"requested_time_ns\":" << arguments.requested_time_ns
           << ",\"hardware_exact\":"
           << (hardware_exact ? "true" : "false")
           << ",\"waveform_model\":\"" << waveform_model
           << "\",\"amplitude\":" << applied_amplitude
           << ",\"waveform_sha256\":\""
           << waveform_hash << "\",\"input\":\""
           << json_escape(input_path.string()) << "\"}\n";
    if (!record) throw std::runtime_error("failed to write TX record");
  }
  {
    evidence::LabEvent event;
    event.event_id = arguments.stem + "-tx";
    event.utc = arguments.utc;
    event.source = "sdr-framework";
    event.correlation_id = arguments.stem;
    event.type = "rf.tx";
    event.direction = "outbound";
    event.evidence_refs = {record_path.filename().string(),
                           manifest_path.filename().string()};
    std::ostringstream payload;
    payload << "{\"backend\":\""
            << json_escape(arguments.backend.value_or("selected-device"))
            << "\",\"center_frequency_hz\":"
            << arguments.center_frequency_hz << ",\"sample_rate\":"
            << input.sample_rate << ",\"sample_count\":"
            << input.samples.size() << ",\"tx_gain_db\":"
            << arguments.tx_gain_db << ",\"repeat_count\":"
            << arguments.repeat_count << ",\"interval_ms\":"
            << arguments.interval_ms << ",\"dry_run\":"
            << (arguments.dry_run ? "true" : "false")
            << ",\"requested_time_ns\":" << arguments.requested_time_ns
            << ",\"hardware_exact\":"
            << (hardware_exact ? "true" : "false")
            << ",\"waveform_model\":\"" << waveform_model
            << "\",\"amplitude\":" << applied_amplitude
            << ",\"waveform_sha256\":\""
            << waveform_hash << "\"}";
    event.payload_json = payload.str();
    std::ofstream events(event_path, std::ios::binary);
    events << evidence::serialize_lab_event(event) << '\n';
    if (!events) throw std::runtime_error("failed to write TX event");
  }
  {
    std::ofstream manifest(manifest_path, std::ios::binary);
    manifest << evidence::sha256_file(record_path) << "  "
             << record_path.filename().string() << '\n'
             << waveform_hash << "  "
             << input_path.filename().string()
             << '\n'
             << evidence::sha256_file(event_path) << "  "
             << event_path.filename().string() << '\n';
    if (!manifest) throw std::runtime_error("failed to write TX manifest");
  }
  output << record_path.string() << " dry_run="
         << (arguments.dry_run ? "true" : "false") << '\n';
  return 0;
}

void write_u32le(std::ostream& stream, std::uint32_t value) {
  const std::array<unsigned char, 4> bytes{
      static_cast<unsigned char>(value),
      static_cast<unsigned char>(value >> 8U),
      static_cast<unsigned char>(value >> 16U),
      static_cast<unsigned char>(value >> 24U)};
  stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void write_u16le(std::ostream& stream, std::uint16_t value) {
  const std::array<unsigned char, 2> bytes{static_cast<unsigned char>(value),
                                          static_cast<unsigned char>(value >> 8U)};
  stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void write_pcap(const std::filesystem::path& path,
                std::span<const std::uint8_t> frame) {
  if (std::filesystem::exists(path)) throw std::runtime_error("PCAP output already exists");
  std::ofstream stream(path, std::ios::binary);
  write_u32le(stream, 0xa1b2c3d4U);
  write_u16le(stream, 2U); write_u16le(stream, 4U);
  write_u32le(stream, 0U); write_u32le(stream, 0U);
  write_u32le(stream, 65535U); write_u32le(stream, 147U);
  write_u32le(stream, 0U); write_u32le(stream, 0U);
  write_u32le(stream, static_cast<std::uint32_t>(frame.size()));
  write_u32le(stream, static_cast<std::uint32_t>(frame.size()));
  stream.write(reinterpret_cast<const char*>(frame.data()),
               static_cast<std::streamsize>(frame.size()));
  if (!stream) throw std::runtime_error("failed to write PCAP");
}

}  // namespace

auto parse_arguments(std::span<const std::string_view> arguments)
    -> ArgumentResult {
  if (arguments.empty()) {
    return Arguments{};
  }
  const auto command = command_from_name(arguments[0]);
  if (!command.has_value()) {
    return ArgumentError{"unknown_command",
                         "unknown command: " + std::string(arguments[0])};
  }
  Arguments parsed;
  parsed.command = *command;
  std::set<std::string_view> seen;
  for (std::size_t index = 1U; index < arguments.size(); ++index) {
    const auto option = arguments[index];
    if (!seen.insert(option).second) {
      return ArgumentError{"duplicate_option",
                           "option appears more than once: " +
                               std::string(option)};
    }
    if (option == "--jsonl") {
      parsed.jsonl = true;
      continue;
    }
    if (option == "--enable-tx") {
      parsed.enable_tx = true;
      continue;
    }
    if (option == "--dry-run") {
      parsed.dry_run = true;
      continue;
    }
    if (option == "--request-uplink-mac-ack") {
      parsed.request_uplink_mac_ack = true;
      continue;
    }
    const auto value_result = required_value(arguments, index, option);
    if (std::holds_alternative<ArgumentError>(value_result)) {
      return std::get<ArgumentError>(value_result);
    }
    const auto value = std::get<std::string>(value_result);
    if (option == "--hex") parsed.hex = value;
    else if (option == "--psdu-hex") parsed.psdu_hex = value;
    else if (option == "--frame-hex") parsed.frame_hex = value;
    else if (option == "--source-eui") parsed.source_eui = value;
    else if (option == "--payload-hex") parsed.payload_hex = value;
    else if (option == "--key-hex") parsed.key_hex = value;
    else if (option == "--iv-hex") parsed.iv_hex = value;
    else if (option == "--serial-hex") parsed.serial_hex = value;
    else if (option == "--input") parsed.input = value;
    else if (option == "--input-b") parsed.input_b = value;
    else if (option == "--output") parsed.output = value;
    else if (option == "--backend") parsed.backend = value;
    else if (option == "--serial") parsed.serial = value;
    else if (option == "--fpga") parsed.fpga = value;
    else if (option == "--location") parsed.location = value;
    else if (option == "--antenna") parsed.antenna = value;
    else if (option == "--session") parsed.session = value;
    else if (option == "--preflight") parsed.preflight = value;
    else if (option == "--model") parsed.model = value;
    else if (option == "--event") parsed.event = value;
    else if (option == "--rpc") parsed.rpc = value;
    else if (option == "--stem") parsed.stem = value;
    else if (option == "--utc") parsed.utc = value;
    else if (option == "--waveform-model") parsed.waveform_model = value;
    else if (option == "--calibration-phase-rad") {
      std::size_t used{};
      try { parsed.calibration_phase_rad = std::stod(value, &used); }
      catch (...) { return ArgumentError{"invalid_value", "--calibration-phase-rad requires a number"}; }
      if (used != value.size() || !std::isfinite(parsed.calibration_phase_rad))
        return ArgumentError{"invalid_value", "--calibration-phase-rad requires a finite number"};
    }
    else if (option == "--carrier-offset-hz") {
      std::size_t used{};
      try { parsed.carrier_offset_hz = std::stod(value, &used); }
      catch (...) { return ArgumentError{"invalid_value", "--carrier-offset-hz requires a number"}; }
      if (used != value.size() || !std::isfinite(parsed.carrier_offset_hz))
        return ArgumentError{"invalid_value", "--carrier-offset-hz requires a finite number"};
    }
    else if (option == "--gain-db" || option == "--score" ||
             option == "--amplitude") {
      std::size_t used{}; double number{};
      try { number = std::stod(value, &used); }
      catch (...) { return ArgumentError{"invalid_value", std::string(option) + " requires a number"}; }
      if (used != value.size() || !std::isfinite(number)) return ArgumentError{"invalid_value", std::string(option) + " requires a finite number"};
      if (option == "--gain-db") parsed.gain_db = number;
      else if (option == "--amplitude") parsed.amplitude = number;
      else parsed.score = number;
    }
    else if (option == "--destination" || option == "--pan" ||
             option == "--sequence" || option == "--type" ||
             option == "--flags" || option == "--counter" ||
             option == "--channel" || option == "--regulatory-code" ||
             option == "--scan-sequence" || option == "--event-id" ||
             option == "--config-id" || option == "--stat-group" ||
             option == "--index") {
      const auto number = parse_integer<std::uint32_t>(value, option);
      if (std::holds_alternative<ArgumentError>(number)) {
        return std::get<ArgumentError>(number);
      }
      const auto result = std::get<std::uint32_t>(number);
      if (option == "--destination") parsed.destination = result;
      else if (option == "--pan") parsed.pan = result;
      else if (option == "--sequence") parsed.sequence = result;
      else if (option == "--type") parsed.type = result;
      else if (option == "--flags") parsed.flags = result;
      else if (option == "--counter") parsed.counter = result;
      else if (option == "--channel") parsed.channel = result;
      else if (option == "--regulatory-code") parsed.regulatory_code = result;
      else if (option == "--event-id") parsed.event_id = result;
      else if (option == "--config-id") parsed.config_id = result;
      else if (option == "--stat-group") parsed.stat_group = result;
      else if (option == "--index") parsed.index = result;
      else parsed.scan_sequence = result;
    }
    else if (option == "--sample-rate") {
      const auto number = parse_integer<std::uint32_t>(value, option);
      if (std::holds_alternative<ArgumentError>(number)) {
        return std::get<ArgumentError>(number);
      }
      parsed.sample_rate = std::get<std::uint32_t>(number);
      parsed.sample_rate_explicit = true;
      if (parsed.sample_rate == 0U) {
        return ArgumentError{"invalid_value", "--sample-rate must be nonzero"};
      }
    } else if (option == "--center-frequency") {
      const auto number = parse_integer<std::uint64_t>(value, option);
      if (std::holds_alternative<ArgumentError>(number)) {
        return std::get<ArgumentError>(number);
      }
      parsed.center_frequency_hz = std::get<std::uint64_t>(number);
    } else if (option == "--tx-gain") {
      const auto number = parse_integer<std::uint32_t>(value, option);
      if (std::holds_alternative<ArgumentError>(number)) return std::get<ArgumentError>(number);
      parsed.tx_gain_db = std::get<std::uint32_t>(number);
      if (parsed.tx_gain_db > 100U)
        return ArgumentError{"invalid_value", "--tx-gain must be 0-100 dB"};
    } else if (option == "--sample-count") {
      const auto number = parse_integer<std::size_t>(value, option);
      if (std::holds_alternative<ArgumentError>(number)) {
        return std::get<ArgumentError>(number);
      }
      parsed.sample_count = std::get<std::size_t>(number);
    } else if (option == "--repeat") {
      const auto number = parse_integer<std::size_t>(value, option);
      if (std::holds_alternative<ArgumentError>(number)) {
        return std::get<ArgumentError>(number);
      }
      parsed.repeat_count = std::get<std::size_t>(number);
    } else if (option == "--interval-ms") {
      const auto number = parse_integer<std::uint64_t>(value, option);
      if (std::holds_alternative<ArgumentError>(number)) {
        return std::get<ArgumentError>(number);
      }
      parsed.interval_ms = std::get<std::uint64_t>(number);
    } else if (option == "--rx-timeout-ms") {
      const auto number = parse_integer<std::uint64_t>(value, option);
      if (std::holds_alternative<ArgumentError>(number)) {
        return std::get<ArgumentError>(number);
      }
      parsed.rx_timeout_ms = std::get<std::uint64_t>(number);
    } else if (option == "--at-ns") {
      const auto number = parse_integer<std::uint64_t>(value, option);
      if (std::holds_alternative<ArgumentError>(number)) {
        return std::get<ArgumentError>(number);
      }
      parsed.requested_time_ns = std::get<std::uint64_t>(number);
    } else if (option == "--packets" || option == "--valid") {
      const auto number = parse_integer<std::size_t>(value, option);
      if (std::holds_alternative<ArgumentError>(number)) return std::get<ArgumentError>(number);
      if (option == "--packets") parsed.packet_count = std::get<std::size_t>(number);
      else parsed.valid_count = std::get<std::size_t>(number);
    } else {
      return ArgumentError{"unknown_option",
                           "unknown option: " + std::string(option)};
    }
  }

  if (!(parsed.amplitude > 0.0 && parsed.amplitude <= 0.9))
    return ArgumentError{"invalid_value", "--amplitude must be in (0, 0.9]"};

  if (parsed.command == Command::Inspect && !parsed.hex.has_value())
    return missing("--hex");
  if (parsed.command == Command::Build && !parsed.psdu_hex.has_value())
    return missing("--psdu-hex");
  if (parsed.command == Command::SecureFrame) {
    if (!parsed.source_eui) return missing("--source-eui");
    if (!parsed.destination) return missing("--destination");
    if (!parsed.pan) return missing("--pan");
    if (!parsed.sequence) return missing("--sequence");
    if (!parsed.type) return missing("--type");
    if (!parsed.flags) return missing("--flags");
    if (!parsed.counter) return missing("--counter");
    if (!parsed.payload_hex) return missing("--payload-hex");
    if (!parsed.key_hex) return missing("--key-hex");
    if (!parsed.iv_hex) return missing("--iv-hex");
    if (*parsed.destination > 2U)
      return ArgumentError{"invalid_value", "--destination must be 0, 1, or 2"};
    if (*parsed.pan > 0xffffU || *parsed.counter > 0xffffU)
      return ArgumentError{"invalid_value", "--pan and --counter must fit 16 bits"};
    if (*parsed.sequence > 0xffU || *parsed.type > 0xffU ||
        *parsed.flags > 0xffU)
      return ArgumentError{"invalid_value", "--sequence, --type, and --flags must fit 8 bits"};
    if ((*parsed.flags & 0x01U) == 0U)
      return ArgumentError{"invalid_value", "--flags must set encrypted bit 0"};
  }
  if (parsed.command == Command::JoinFrame) {
    if (!parsed.source_eui) return missing("--source-eui");
    if (!parsed.destination) return missing("--destination");
    if (!parsed.pan) return missing("--pan");
    if (!parsed.sequence) return missing("--sequence");
    if (!parsed.serial_hex) return missing("--serial-hex");
    if (*parsed.destination > 2U)
      return ArgumentError{"invalid_value", "--destination must be 0, 1, or 2"};
    if (*parsed.pan > 0xffffU)
      return ArgumentError{"invalid_value", "--pan must fit 16 bits"};
    if (*parsed.sequence > 0xffU)
      return ArgumentError{"invalid_value", "--sequence must fit 8 bits"};
  }
  if (parsed.command == Command::ScanCompleteFrame) {
    if (!parsed.source_eui) return missing("--source-eui");
    if (!parsed.destination) return missing("--destination");
    if (!parsed.pan) return missing("--pan");
    if (!parsed.sequence) return missing("--sequence");
    if (!parsed.channel) return missing("--channel");
    if (!parsed.regulatory_code) return missing("--regulatory-code");
    if (!parsed.scan_sequence) return missing("--scan-sequence");
    if (*parsed.destination > 2U)
      return ArgumentError{"invalid_value", "--destination must be 0, 1, or 2"};
    if (*parsed.pan > 0xffffU || *parsed.channel > 0xffffU)
      return ArgumentError{"invalid_value", "--pan and --channel must fit 16 bits"};
    if (*parsed.sequence > 0xffU || *parsed.regulatory_code > 0xffU ||
        *parsed.scan_sequence > 0xffU)
      return ArgumentError{"invalid_value", "sequence and regulatory fields must fit 8 bits"};
  }
  if (parsed.command == Command::Waveform && !parsed.frame_hex.has_value())
    return missing("--frame-hex");
  if (parsed.command == Command::Waveform && !parsed.output.has_value())
    return missing("--output");
  if ((parsed.command == Command::Decode || parsed.command == Command::Scan) &&
      !parsed.input.has_value())
    return missing("--input");
  if (parsed.command == Command::Capture && !parsed.output.has_value())
    return missing("--output");
  if (parsed.command == Command::Transmit && !parsed.input.has_value() &&
      !parsed.frame_hex.has_value())
    return ArgumentError{"missing_option",
                         "transmit requires --input or --frame-hex"};
  if (parsed.command == Command::Transmit && parsed.input.has_value() &&
      parsed.frame_hex.has_value())
    return ArgumentError{"invalid_value",
                         "transmit accepts only one of --input or --frame-hex"};
  if (parsed.command == Command::Transmit && !parsed.output.has_value())
    return missing("--output");
  if (parsed.command == Command::Transmit && !parsed.enable_tx &&
      !parsed.dry_run)
    return ArgumentError{"tx_acknowledgement_required",
                         "transmit requires --enable-tx or --dry-run"};
  if (parsed.command == Command::Transmit && parsed.input.has_value() &&
      parsed.backend.has_value() && !parsed.dry_run &&
      !parsed.sample_rate_explicit) {
    return ArgumentError{
        "missing_option",
        "live IQ transmit requires explicit --sample-rate before device open"};
  }
  if (parsed.command == Command::Simulate && !parsed.input.has_value()) return missing("--input");
  if (parsed.command == Command::Simulate && !parsed.output.has_value()) return missing("--output");
  if (parsed.command == Command::Pcap && !parsed.hex.has_value()) return missing("--hex");
  if (parsed.command == Command::Pcap && !parsed.output.has_value()) return missing("--output");
  if (parsed.command == Command::Df && !parsed.input.has_value()) return missing("--input");
  if (parsed.command == Command::Df && !parsed.input_b.has_value()) return missing("--input-b");
  if (parsed.command == Command::Df && !parsed.output.has_value()) return missing("--output");
  if (parsed.command == Command::DfCapture && !parsed.output.has_value()) return missing("--output");
  if (parsed.command == Command::DfCapture && parsed.sample_count == 0U)
    return ArgumentError{"invalid_value", "df-capture requires nonzero --sample-count"};
  if (parsed.command == Command::DfCapture && !parsed.antenna.has_value()) return missing("--antenna");
  if (parsed.command == Command::Coverage && !parsed.output.has_value()) return missing("--output");
  if (parsed.command == Command::Coverage && !parsed.location.has_value()) return missing("--location");
  if (parsed.command == Command::Coverage && !parsed.antenna.has_value()) return missing("--antenna");
  if (parsed.command == Command::Coverage && (parsed.packet_count == 0U || parsed.valid_count > parsed.packet_count || parsed.score < 0.0 || parsed.score > 1.0)) return ArgumentError{"invalid_value", "coverage counts or score are invalid"};
  if (parsed.command == Command::SensorCreate) {
    if (!parsed.session) return missing("--session");
    if (!parsed.serial) return missing("--serial");
    if (!parsed.model) return missing("--model");
    if (*parsed.model != "door-contact" && *parsed.model != "glass-break" &&
        *parsed.model != "motion" && *parsed.model != "panic" &&
        *parsed.model != "water" && *parsed.model != "wireless-relay") {
      return ArgumentError{"invalid_value", "unsupported synthetic sensor model"};
    }
  }
  if (parsed.command == Command::SensorStatus && !parsed.session) {
    return missing("--session");
  }
  if (parsed.command == Command::SensorRetire) {
    if (!parsed.session) return missing("--session");
    if (!parsed.output) return missing("--output");
  }
  if (parsed.command == Command::SensorJoin ||
      parsed.command == Command::SensorEvent ||
      parsed.command == Command::SensorRpcRead) {
    if (!parsed.session) return missing("--session");
    if (!parsed.output) return missing("--output");
    if (!parsed.preflight) return missing("--preflight");
    if (!parsed.backend) return missing("--backend");
    if (*parsed.backend != "b210") {
      return ArgumentError{"invalid_value",
                           "stateful sensor transactions require --backend b210"};
    }
    if (!parsed.serial) return missing("--serial");
    if (!parsed.fpga) return missing("--fpga");
    if (!parsed.sample_rate_explicit) return missing("--sample-rate");
    if (parsed.sample_rate < 4'000'000U || parsed.sample_rate > 20'000'000U) {
      return ArgumentError{"invalid_value",
                           "sensor sample rate must be 4000000..20000000"};
    }
    if (parsed.center_frequency_hz < 902'000'000U ||
        parsed.center_frequency_hz > 928'000'000U) {
      return ArgumentError{"invalid_value",
                           "sensor frequency must be inside 902-928 MHz"};
    }
    if (parsed.rx_timeout_ms < 100U || parsed.rx_timeout_ms > 20'000U) {
      return ArgumentError{"invalid_value", "--rx-timeout-ms must be 100..20000"};
    }
    if (parsed.tx_gain_db > 85U) {
      return ArgumentError{"invalid_value",
                           "staged sensor transactions limit TX gain to 85 dB"};
    }
    if (parsed.pan && *parsed.pan > 0xffffU) {
      return ArgumentError{"invalid_value", "--pan must fit 16 bits"};
    }
    if (parsed.destination && *parsed.destination > 2U) {
      return ArgumentError{"invalid_value", "--destination must be 0, 1, or 2"};
    }
    if (parsed.enable_tx == parsed.dry_run) {
      return ArgumentError{
          "tx_acknowledgement_required",
          "sensor transaction requires exactly one of --enable-tx or --dry-run"};
    }
  }
  if (parsed.command == Command::SensorEvent) {
    if (!parsed.event) return missing("--event");
    if (!core::event_semantic(*parsed.event).has_value()) {
      return ArgumentError{"invalid_value",
                           "sensor event must be a recovered event semantic"};
    }
    if (!parsed.event_id) return missing("--event-id");
  }
  if (parsed.command == Command::SensorRpcRead) {
    if (!parsed.rpc) return missing("--rpc");
    const auto operation = coordinator_read_operation(*parsed.rpc);
    if (!operation) {
      return ArgumentError{"invalid_value",
                           "sensor RPC must be a named read-only coordinator operation"};
    }
    const auto has_payload = parsed.payload_hex.has_value();
    const auto has_config = parsed.config_id.has_value();
    const auto has_stat = parsed.stat_group.has_value();
    const auto has_index = parsed.index.has_value();
    switch (*operation) {
      case CoordinatorReadOperation::Echo:
        if (!has_payload) return missing("--payload-hex");
        if (parsed.payload_hex->empty() || parsed.payload_hex->size() > 128U ||
            parsed.payload_hex->size() % 2U != 0U ||
            !std::all_of(parsed.payload_hex->begin(), parsed.payload_hex->end(),
                         [](unsigned char byte) { return std::isxdigit(byte); })) {
          return ArgumentError{"invalid_value",
                               "echo payload must be 1..64 complete hex bytes"};
        }
        if (has_config || has_stat || has_index) {
          return ArgumentError{"invalid_value", "echo accepts only --payload-hex"};
        }
        break;
      case CoordinatorReadOperation::ImageState:
      case CoordinatorReadOperation::RangeStatus:
        if (has_payload || has_config || has_stat || has_index) {
          return ArgumentError{"invalid_value",
                               "selected read RPC accepts no body option"};
        }
        break;
      case CoordinatorReadOperation::Config:
        if (!has_config) return missing("--config-id");
        if (*parsed.config_id > 0xffffU || has_payload || has_stat || has_index) {
          return ArgumentError{"invalid_value",
                               "config requires only a 16-bit --config-id"};
        }
        break;
      case CoordinatorReadOperation::Statistics:
        if (!has_stat) return missing("--stat-group");
        if (*parsed.stat_group > 11U || has_payload || has_config || has_index) {
          return ArgumentError{"invalid_value",
                               "statistics requires only --stat-group 0..11"};
        }
        break;
      case CoordinatorReadOperation::Peer:
      case CoordinatorReadOperation::Mempool:
        if (!has_index) return missing("--index");
        if (*parsed.index > 0xffU || has_payload || has_config || has_stat) {
          return ArgumentError{"invalid_value",
                               "peer and mempool require only an 8-bit --index"};
        }
        break;
    }
  }
  return parsed;
}

auto run_cli(std::span<const std::string_view> arguments, std::ostream& output,
             std::ostream& error, CliEnvironment environment) -> int {
  const auto parsed_result = parse_arguments(arguments);
  if (std::holds_alternative<ArgumentError>(parsed_result)) {
    const auto& failure = std::get<ArgumentError>(parsed_result);
    structured_error(error, failure.code, failure.message);
    return 2;
  }
  const auto& parsed = std::get<Arguments>(parsed_result);
  try {
    auto* selected_device = environment.device;
    if (selected_device == nullptr && environment.factory != nullptr &&
        parsed.backend.has_value() && !parsed.dry_run &&
        (parsed.command == Command::Transmit ||
         parsed.command == Command::Capture ||
         parsed.command == Command::DfCapture ||
         parsed.command == Command::SensorJoin ||
         parsed.command == Command::SensorEvent ||
         parsed.command == Command::SensorRpcRead)) {
      const auto transmit = parsed.command == Command::Transmit ||
                            parsed.command == Command::SensorJoin ||
                            parsed.command == Command::SensorEvent ||
                            parsed.command == Command::SensorRpcRead;
      selected_device = environment.factory->select(radio::DeviceRequest{
          *parsed.backend, parsed.serial.value_or(""),
          parsed.fpga.value_or(""), parsed.sample_rate,
          parsed.center_frequency_hz, parsed.tx_gain_db,
          parsed.command == Command::Capture ||
              parsed.command == Command::SensorJoin ||
              parsed.command == Command::SensorEvent ||
              parsed.command == Command::SensorRpcRead,
          parsed.command == Command::DfCapture, transmit});
      if (selected_device == nullptr) {
        throw std::runtime_error("device factory did not select a device");
      }
    }
    switch (parsed.command) {
      case Command::Help:
        output << "usage: bh61-radio <devices|inspect|build|secure-frame|join-frame|scan-complete-frame|waveform|decode|"
                  "capture|scan|fixtures|transmit|simulate|pcap|df|df-capture|coverage|"
                  "sensor-create|sensor-join|sensor-event|sensor-rpc-read|sensor-status|sensor-retire> [options]\n";
        return 0;
      case Command::Version:
        output << bh61::version() << '\n';
        return 0;
      case Command::Devices:
        for (const auto capability : bh61::build_capabilities()) {
          if (parsed.jsonl) {
            output << "{\"kind\":\"compiled_backend\",\"name\":\""
                   << json_escape(capability.name) << "\",\"rx\":"
                   << (capability.rx ? "true" : "false") << ",\"tx\":"
                   << (capability.tx ? "true" : "false") << "}\n";
          } else {
            output << "compiled " << capability.name << " rx="
                   << (capability.rx ? "true" : "false") << " tx="
                   << (capability.tx ? "true" : "false") << '\n';
          }
        }
        if (environment.factory != nullptr) {
          for (const auto& device : environment.factory->enumerate()) {
            if (parsed.jsonl) {
              output << "{\"kind\":\"attached_device\",\"backend\":\""
                     << json_escape(device.backend) << "\",\"serial\":\""
                     << json_escape(device.serial) << "\",\"board\":\""
                     << json_escape(device.board_name)
                     << "\",\"firmware\":\""
                     << json_escape(device.firmware_version)
                     << "\",\"hardware_revision\":\""
                     << json_escape(device.hardware_revision)
                     << "\",\"rx\":"
                     << (device.capabilities.rx ? "true" : "false")
                     << ",\"tx\":"
                     << (device.capabilities.tx ? "true" : "false")
                     << ",\"full_duplex\":"
                     << (device.capabilities.full_duplex ? "true" : "false")
                     << ",\"sample_format\":\""
                     << sample_format_name(device.capabilities.sample_format)
                     << "\",\"minimum_sample_rate\":"
                     << device.capabilities.minimum_sample_rate
                     << ",\"maximum_sample_rate\":"
                     << device.capabilities.maximum_sample_rate << "}\n";
            } else {
              output << "attached " << device.backend << " serial="
                     << device.serial << " board=\"" << device.board_name
                     << "\" firmware=\"" << device.firmware_version
                     << "\" hardware_revision=\""
                     << device.hardware_revision << "\"\n";
            }
          }
        }
        return 0;
      case Command::Fixtures:
        output << "type11-f27-normal-destination\n"
                  "synthetic-join-public\ndsss-dictionary\n";
        return 0;
      case Command::Inspect:
        return inspect_frame(decode_hex(*parsed.hex), parsed.jsonl, output);
      case Command::Build: {
        core::RadioFrame frame;
        frame.psdu = decode_hex(*parsed.psdu_hex);
        output << encode_hex(core::encode_radio_frame(frame)) << '\n';
        return 0;
      }
      case Command::SecureFrame:
        output << encode_hex(build_secure_frame(parsed)) << '\n';
        return 0;
      case Command::JoinFrame:
        output << encode_hex(build_join_frame(parsed)) << '\n';
        return 0;
      case Command::ScanCompleteFrame:
        output << encode_hex(build_scan_complete_frame(parsed)) << '\n';
        return 0;
      case Command::Waveform: {
        const auto frame = decode_hex(*parsed.frame_hex);
        const auto validation = core::parse_radio_frame(frame);
        if (std::holds_alternative<core::ParseError>(validation)) {
          throw std::runtime_error("waveform input is not a valid radio frame");
        }
        const auto waveform =
            parsed.waveform_model == "analytic-half-sine"
                ? dsp::modulate_oqpsk(
                      frame, parsed.sample_rate,
                      dsp::OqpskOrientation::EvenChipsOnI,
                      dsp::ChipPolarity::ZeroIsPositive)
                : dsp::modulate_efr32_custom_oqpsk(frame,
                                                   parsed.sample_rate);
        write_cf32(*parsed.output, waveform.samples);
        output << "output=" << *parsed.output << " samples="
               << waveform.samples.size()
               << " hardware_exact="
               << (waveform.hardware_exact ? "true" : "false")
               << " waveform_model=" << parsed.waveform_model << '\n';
        return 0;
      }
      case Command::Decode:
        return decode_iq(parsed, output, false);
      case Command::Scan:
        return decode_iq(parsed, output, true);
      case Command::Capture: {
        std::optional<radio::FileDevice> replay_device;
        auto* capture_device = selected_device;
        if (capture_device == nullptr && parsed.input.has_value()) {
          const auto replay_samples = read_cf32(*parsed.input);
          replay_device.emplace(replay_samples, parsed.sample_rate,
                                parsed.center_frequency_hz);
          capture_device = &*replay_device;
        }
        if (capture_device == nullptr) {
          throw std::runtime_error(
              "capture requires a selected device or file-device input");
        }
        std::vector<std::complex<float>> samples;
        const auto requested = parsed.sample_count == 0U
                                   ? std::numeric_limits<std::size_t>::max()
                                   : parsed.sample_count;
        while (samples.size() < requested) {
          const auto remaining = requested - samples.size();
          auto block = capture_device->receive(remaining);
          if (block.samples.empty()) break;
          samples.insert(samples.end(), block.samples.begin(),
                         block.samples.end());
        }
        evidence::Writer writer(*parsed.output, parsed.stem);
        writer.write_sigmf(
            samples,
            evidence::CaptureMetadata{
                parsed.sample_rate, parsed.center_frequency_hz,
                parsed.backend.value_or("file"), "CLI selected device",
                parsed.utc, 0U, false});
        const auto reproduction =
            "bh61-radio decode --input " + parsed.stem +
            ".sigmf-data --sample-rate " +
            std::to_string(parsed.sample_rate) + " --jsonl";
        const auto artifacts = writer.finalize(reproduction);
        output << artifacts.meta_path.string() << '\n';
        return 0;
      }
      case Command::Transmit:
        return transmit_iq(parsed, selected_device, output);
      case Command::Simulate: {
        std::ifstream scenario_input(*parsed.input);
        if (!scenario_input) throw std::runtime_error("cannot open sensor scenario");
        const auto scenario = core::parse_sensor_scenario(scenario_input);
        core::SensorModel sensor(scenario.sensor_id);
        const auto path = std::filesystem::path(*parsed.output);
        if (std::filesystem::exists(path)) throw std::runtime_error("sensor event output already exists");
        std::ofstream stream(path, std::ios::binary);
        std::size_t index{};
        for (const auto action : scenario.actions) {
          const auto observation = sensor.apply(action);
          evidence::LabEvent event;
          event.event_id = parsed.stem + "-sensor-" + std::to_string(index++);
          event.utc = parsed.utc;
          event.source = "sdr-framework.sensor-simulator";
          event.sensor_id = scenario.sensor_id;
          event.correlation_id = parsed.stem;
          event.type = observation.type;
          event.direction = "internal";
          auto inner = observation.payload_json;
          if (inner.size() >= 2U) inner = inner.substr(1U, inner.size() - 2U);
          event.payload_json = "{\"counter\":" +
              std::to_string(observation.counter) +
              ",\"retransmission\":" +
              (observation.retransmission ? "true" : "false") +
              (inner.empty() ? "" : "," + inner) + "}";
          stream << evidence::serialize_lab_event(event) << '\n';
        }
        if (!stream) throw std::runtime_error("failed to write sensor events");
        output << path.string() << " events=" << scenario.actions.size()
               << " sensor=" << scenario.sensor_id << '\n';
        return 0;
      }
      case Command::Pcap: {
        const auto frame = decode_hex(*parsed.hex);
        if (std::holds_alternative<core::ParseError>(core::parse_radio_frame(frame))) {
          throw std::runtime_error("PCAP input is not a valid BH61 radio frame");
        }
        write_pcap(*parsed.output, frame);
        output << *parsed.output << " packets=1 linktype=USER0(147)\n";
        return 0;
      }
      case Command::Df: {
        const auto channel_a = read_cf32(*parsed.input);
        const auto channel_b = read_cf32(*parsed.input_b);
        if (channel_a.empty() || channel_a.size() != channel_b.size()) {
          throw std::runtime_error("DF inputs must have the same nonzero sample count");
        }
        std::complex<double> cross{};
        double power_a{}, power_b{};
        for (std::size_t i = 0; i < channel_a.size(); ++i) {
          cross += static_cast<std::complex<double>>(channel_b[i]) *
                   std::conj(static_cast<std::complex<double>>(channel_a[i]));
          power_a += std::norm(channel_a[i]);
          power_b += std::norm(channel_b[i]);
        }
        if (power_a == 0.0 || power_b == 0.0) throw std::runtime_error("DF input has zero power");
        const auto phase = std::arg(cross) - parsed.calibration_phase_rad;
        const auto amplitude_ratio = std::sqrt(power_b / power_a);
        const auto path = std::filesystem::path(*parsed.output);
        if (std::filesystem::exists(path)) throw std::runtime_error("DF output already exists");
        std::ofstream report(path, std::ios::binary);
        report << "{\"schema\":\"bh61.df.observation/v1\",\"sample_count\":"
               << channel_a.size() << ",\"phase_rad\":"
               << std::setprecision(17) << phase
               << ",\"calibration_phase_rad\":" << parsed.calibration_phase_rad
               << ",\"amplitude_ratio_b_over_a\":" << amplitude_ratio
               << ",\"bearing_claimed\":false}\n";
        if (!report) throw std::runtime_error("failed to write DF observation");
        output << path.string() << " bearing_claimed=false\n";
        return 0;
      }
      case Command::DfCapture: {
        if (selected_device == nullptr) {
          throw std::runtime_error(
              "df-capture requires a selected B210/UHD device");
        }
        const auto pair = selected_device->receive_pair(parsed.sample_count);
        if (pair.channel_a.status != radio::SampleBlockStatus::Data ||
            pair.channel_b.status != radio::SampleBlockStatus::Data ||
            pair.channel_a.samples.empty() ||
            pair.channel_a.samples.size() != pair.channel_b.samples.size()) {
          throw std::runtime_error("coherent two-channel capture failed");
        }
        if (pair.channel_a.first_sample_index !=
                pair.channel_b.first_sample_index ||
            pair.channel_a.monotonic_time_ns !=
                pair.channel_b.monotonic_time_ns) {
          throw std::runtime_error(
              "coherent channel metadata is not aligned");
        }
        const auto directory = std::filesystem::path(*parsed.output);
        std::filesystem::create_directories(directory);
        const auto channel_a_path = directory / (parsed.stem + ".ch0.cf32");
        const auto channel_b_path = directory / (parsed.stem + ".ch1.cf32");
        const auto report_path = directory / (parsed.stem + ".df.json");
        const auto manifest_path = directory / (parsed.stem + ".sha256");
        for (const auto& path : {channel_a_path, channel_b_path, report_path,
                                manifest_path}) {
          if (std::filesystem::exists(path)) {
            throw std::runtime_error(
                "DF capture evidence artifact already exists");
          }
        }
        write_cf32(channel_a_path, pair.channel_a.samples);
        write_cf32(channel_b_path, pair.channel_b.samples);
        std::complex<double> cross{};
        double power_a{}, power_b{};
        for (std::size_t i = 0; i < pair.channel_a.samples.size(); ++i) {
          cross += static_cast<std::complex<double>>(
                       pair.channel_b.samples[i]) *
                   std::conj(static_cast<std::complex<double>>(
                       pair.channel_a.samples[i]));
          power_a += std::norm(pair.channel_a.samples[i]);
          power_b += std::norm(pair.channel_b.samples[i]);
        }
        if (power_a == 0.0 || power_b == 0.0) {
          throw std::runtime_error("DF capture has zero channel power");
        }
        std::ofstream report(report_path, std::ios::binary);
        report << "{\"schema\":\"bh61.df.capture/v1\",\"utc\":\""
               << json_escape(parsed.utc) << "\",\"sample_count\":"
               << pair.channel_a.samples.size() << ",\"sample_rate\":"
               << parsed.sample_rate << ",\"center_frequency_hz\":"
               << parsed.center_frequency_hz << ",\"device_time_ns\":"
               << pair.channel_a.monotonic_time_ns
               << ",\"first_sample_index\":"
               << pair.channel_a.first_sample_index << ",\"antenna\":\""
               << json_escape(*parsed.antenna)
               << "\",\"coherent_capture\":true,\"phase_rad\":"
               << std::setprecision(17)
               << (std::arg(cross) - parsed.calibration_phase_rad)
               << ",\"calibration_phase_rad\":"
               << parsed.calibration_phase_rad
               << ",\"amplitude_ratio_b_over_a\":"
               << std::sqrt(power_b / power_a)
               << ",\"bearing_claimed\":false}\n";
        if (!report) {
          throw std::runtime_error("failed to write DF capture report");
        }
        std::ofstream manifest(manifest_path, std::ios::binary);
        for (const auto& path : {channel_a_path, channel_b_path, report_path}) {
          manifest << evidence::sha256_file(path) << "  "
                   << path.filename().string() << '\n';
        }
        if (!manifest) {
          throw std::runtime_error("failed to write DF manifest");
        }
        output << report_path.string()
               << " coherent_capture=true bearing_claimed=false\n";
        return 0;
      }
      case Command::Coverage: {
        const auto path = std::filesystem::path(*parsed.output);
        if (std::filesystem::exists(path)) throw std::runtime_error("coverage output already exists");
        const auto loss = static_cast<double>(parsed.packet_count - parsed.valid_count) /
                          static_cast<double>(parsed.packet_count);
        std::ofstream report(path, std::ios::binary);
        report << "{\"schema\":\"bh61.coverage.observation/v1\",\"utc\":\""
               << json_escape(parsed.utc) << "\",\"location\":\""
               << json_escape(*parsed.location) << "\",\"antenna\":\""
               << json_escape(*parsed.antenna) << "\",\"gain_db\":"
               << std::setprecision(17) << parsed.gain_db
               << ",\"packet_count\":" << parsed.packet_count
               << ",\"valid_frame_count\":" << parsed.valid_count
               << ",\"mean_score\":" << parsed.score
               << ",\"loss_estimate\":" << loss << "}\n";
        if (!report) throw std::runtime_error("failed to write coverage observation");
        output << path.string() << " valid=" << parsed.valid_count << '/'
               << parsed.packet_count << '\n';
        return 0;
      }
      case Command::SensorCreate: {
        const auto session_path = std::filesystem::path(*parsed.session);
        auto keys = core::generate_p256_keypair();
        core::SensorIdentity identity;
        identity.serial = *parsed.serial;
        random_bytes(identity.physical_device_id);
        random_bytes(identity.eui64);
        identity.eui64[0] = static_cast<std::uint8_t>(
            (identity.eui64[0] | 0x02U) & 0xfeU);
        random_bytes(identity.nonce);
        identity.public_xy = keys.public_xy;
        std::array<std::uint8_t, 64> signed_nonce{};
        random_bytes(signed_nonce);
        try {
          auto session = core::SensorSession::create(
              session_path, identity, keys.private_scalar);
          static_cast<void>(session);
        } catch (...) {
          OPENSSL_cleanse(keys.private_scalar.data(), keys.private_scalar.size());
          throw;
        }
        OPENSSL_cleanse(keys.private_scalar.data(), keys.private_scalar.size());
        const auto registration =
            "{\n"
            "  \"serial\": \"" + json_escape(identity.serial) + "\",\n" +
            "  \"physical_id_hex\": \"" +
            encode_hex(identity.physical_device_id) + "\",\n" +
            "  \"eui64_hex\": \"" + encode_hex(identity.eui64) + "\",\n" +
            "  \"model\": \"" + json_escape(*parsed.model) + "\",\n" +
            "  \"public_key_hex\": \"" + encode_hex(identity.public_xy) +
            "\",\n" +
            "  \"signed_nonce_hex\": \"" + encode_hex(signed_nonce) +
            "\",\n"
            "  \"retired\": false\n"
            "}\n";
        write_private_text(session_path / "cloud-registration.json", registration);
        output << "{\"session\":\"" << json_escape(session_path.string())
               << "\",\"serial\":\"" << json_escape(identity.serial)
               << "\",\"eui64_hex\":\"" << encode_hex(identity.eui64)
               << "\",\"registration\":\""
               << json_escape((session_path / "cloud-registration.json").string())
               << "\"}\n";
        return 0;
      }
      case Command::SensorStatus: {
        const auto session_path = std::filesystem::path(*parsed.session);
        const auto session = core::SensorSession::load(session_path);
        output << "{\"serial\":\"" << json_escape(session.identity().serial)
               << "\",\"eui64_hex\":\"" << encode_hex(session.identity().eui64)
               << "\",\"state\":\""
               << core::sensor_session_state_name(session.state())
               << "\",\"retired\":"
               << (session_is_retired(session_path) ? "true" : "false")
               << ",\"send_counter\":" << session.send_counter()
               << ",\"receive_counter\":" << session.receive_counter()
               << ",\"key_fingerprint_sha256\":\""
               << session.key_fingerprint() << "\"}\n";
        return 0;
      }
      case Command::SensorRetire: {
        const auto session_path = std::filesystem::path(*parsed.session);
        const auto session = core::SensorSession::load(session_path);
        const auto evidence_path = std::filesystem::path(*parsed.output);
        if (!std::filesystem::create_directory(evidence_path)) {
          throw std::runtime_error("retirement evidence directory already exists");
        }
        std::filesystem::permissions(evidence_path,
                                     std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        const auto record =
            "{\"schema\":\"bh61.sensor-retirement/v1\",\"serial\":\"" +
            json_escape(session.identity().serial) +
            "\",\"eui64_hex\":\"" + encode_hex(session.identity().eui64) +
            "\",\"cloud_retirement_required\":true}\n";
        write_private_text(evidence_path / "retirement.json", record);
        write_private_text(session_path / "retired.json", record);
        output << "serial=" << session.identity().serial
               << " local_session_retired=true cloud_retirement_required=true\n";
        return 0;
      }
      case Command::SensorJoin:
      case Command::SensorEvent:
      case Command::SensorRpcRead: {
        const auto session_path = std::filesystem::path(*parsed.session);
        if (session_is_retired(session_path)) {
          throw std::runtime_error("synthetic sensor session is retired");
        }
        auto session = core::SensorSession::load(session_path);
        SensorRunnerConfig config;
        config.pan = static_cast<std::uint16_t>(parsed.pan.value_or(0x01ffU));
        config.hub_short_address = static_cast<std::uint16_t>(
            parsed.destination.value_or(1U));
        config.sample_rate = parsed.sample_rate;
        config.receive_timeout = std::chrono::milliseconds(parsed.rx_timeout_ms);
        config.maximum_receive_frames = 8U;
        config.evidence_directory = *parsed.output;
        const auto preflight_path = std::filesystem::path(*parsed.preflight);
        config.oracle = [preflight_path] {
          return isolation_preflight_status(preflight_path);
        };
        config.request_uplink_mac_ack = parsed.request_uplink_mac_ack;
        config.dry_run = parsed.dry_run;

        DryRunSensorFrameRadio dry_radio;
        std::unique_ptr<DeviceSensorFrameRadio> live_radio;
        SensorFrameRadio* frame_radio = &dry_radio;
        if (!parsed.dry_run) {
          if (selected_device == nullptr) {
            throw std::runtime_error("live sensor transaction requires B210");
          }
          live_radio = std::make_unique<DeviceSensorFrameRadio>(
              *selected_device, parsed.sample_rate, parsed.sample_rate / 100U,
              parsed.carrier_offset_hz,
              std::filesystem::path(*parsed.output) / "receive.cf32",
              session.identity().eui64,
              parsed.command == Command::SensorJoin ? 2U : 0U);
          frame_radio = live_radio.get();
        }
        SensorSessionRunner runner(*frame_radio, config);
        if (parsed.command == Command::SensorJoin) {
          runner.join(session);
          output << "session=" << session_path.string()
                 << " operation=join dry_run="
                 << (parsed.dry_run ? "true" : "false") << " state="
                 << core::sensor_session_state_name(session.state()) << '\n';
        } else if (parsed.command == Command::SensorEvent) {
          const auto semantic = core::event_semantic(*parsed.event);
          if (!semantic) throw std::logic_error("validated sensor event is unavailable");
          runner.send_event(session, *semantic, *parsed.event_id, *parsed.event);
          output << "session=" << session_path.string() << " operation="
                 << *parsed.event << " dry_run="
                 << (parsed.dry_run ? "true" : "false") << " state="
                 << core::sensor_session_state_name(session.state()) << '\n';
        } else {
          const auto operation = coordinator_read_operation(*parsed.rpc);
          if (!operation) throw std::logic_error("validated coordinator read is unavailable");
          std::vector<std::uint8_t> echo;
          if (parsed.payload_hex) echo = decode_hex(*parsed.payload_hex);
          std::optional<std::uint16_t> selector;
          if (parsed.config_id) selector = static_cast<std::uint16_t>(*parsed.config_id);
          else if (parsed.stat_group) selector = static_cast<std::uint16_t>(*parsed.stat_group);
          std::optional<std::uint8_t> index;
          if (parsed.index) index = static_cast<std::uint8_t>(*parsed.index);
          const auto request = make_coordinator_read_request(
              *operation, echo, selector, index, 0x45U);
          const auto result = runner.read_coordinator(
              session, request, coordinator_read_operation_name(*operation));
          output << "session=" << session_path.string() << " operation="
                 << coordinator_read_operation_name(*operation)
                 << " dry_run=" << (parsed.dry_run ? "true" : "false");
          if (result) {
            output << " response_id=" << static_cast<unsigned>(result->response_id)
                   << " transaction=" << static_cast<unsigned>(result->transaction)
                   << " counter=" << result->counter
                   << " payload_hex=" << encode_hex(result->payload);
          }
          output << " state=" << core::sensor_session_state_name(session.state()) << '\n';
        }
        return 0;
      }
    }
  } catch (const std::exception& failure) {
    structured_error(error, "operation_failed", failure.what());
    return 1;
  }
  structured_error(error, "internal_error", "unhandled command");
  return 1;
}

}  // namespace bh61::app
