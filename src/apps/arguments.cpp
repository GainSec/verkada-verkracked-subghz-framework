#include "bh61/app/arguments.hpp"

#include "bh61/core/frame.hpp"
#include "bh61/core/vcmp.hpp"
#include "bh61/core/vmac.hpp"
#include "bh61/dsp/demodulator.hpp"
#include "bh61/dsp/modulator.hpp"
#include "bh61/evidence/writer.hpp"
#include "bh61/radio/file_device.hpp"
#include "bh61/version.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <complex>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
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
  if (name == "waveform") return Command::Waveform;
  if (name == "decode") return Command::Decode;
  if (name == "capture") return Command::Capture;
  if (name == "scan") return Command::Scan;
  if (name == "fixtures") return Command::Fixtures;
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
  const auto decoded =
      dsp::decode_first_burst(input.samples, input.sample_rate, {});
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
    const auto value_result = required_value(arguments, index, option);
    if (std::holds_alternative<ArgumentError>(value_result)) {
      return std::get<ArgumentError>(value_result);
    }
    const auto value = std::get<std::string>(value_result);
    if (option == "--hex") parsed.hex = value;
    else if (option == "--psdu-hex") parsed.psdu_hex = value;
    else if (option == "--frame-hex") parsed.frame_hex = value;
    else if (option == "--input") parsed.input = value;
    else if (option == "--output") parsed.output = value;
    else if (option == "--backend") parsed.backend = value;
    else if (option == "--serial") parsed.serial = value;
    else if (option == "--stem") parsed.stem = value;
    else if (option == "--utc") parsed.utc = value;
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
    } else if (option == "--sample-count") {
      const auto number = parse_integer<std::size_t>(value, option);
      if (std::holds_alternative<ArgumentError>(number)) {
        return std::get<ArgumentError>(number);
      }
      parsed.sample_count = std::get<std::size_t>(number);
    } else {
      return ArgumentError{"unknown_option",
                           "unknown option: " + std::string(option)};
    }
  }

  if (parsed.command == Command::Inspect && !parsed.hex.has_value())
    return missing("--hex");
  if (parsed.command == Command::Build && !parsed.psdu_hex.has_value())
    return missing("--psdu-hex");
  if (parsed.command == Command::Waveform && !parsed.frame_hex.has_value())
    return missing("--frame-hex");
  if (parsed.command == Command::Waveform && !parsed.output.has_value())
    return missing("--output");
  if ((parsed.command == Command::Decode || parsed.command == Command::Scan) &&
      !parsed.input.has_value())
    return missing("--input");
  if (parsed.command == Command::Capture && !parsed.output.has_value())
    return missing("--output");
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
    switch (parsed.command) {
      case Command::Help:
        output << "usage: bh61-radio <devices|inspect|build|waveform|decode|"
                  "capture|scan|fixtures> [options]\n";
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
        output << "type11-f27-normal-destination\nreset-req\n"
                  "dsss-dictionary\n";
        return 0;
      case Command::Inspect:
        return inspect_frame(decode_hex(*parsed.hex), parsed.jsonl, output);
      case Command::Build: {
        core::RadioFrame frame;
        frame.psdu = decode_hex(*parsed.psdu_hex);
        output << encode_hex(core::encode_radio_frame(frame)) << '\n';
        return 0;
      }
      case Command::Waveform: {
        const auto frame = decode_hex(*parsed.frame_hex);
        const auto validation = core::parse_radio_frame(frame);
        if (std::holds_alternative<core::ParseError>(validation)) {
          throw std::runtime_error("waveform input is not a valid radio frame");
        }
        const auto waveform = dsp::modulate_oqpsk(
            frame, parsed.sample_rate, dsp::OqpskOrientation::EvenChipsOnI,
            dsp::ChipPolarity::ZeroIsPositive);
        write_cf32(*parsed.output, waveform.samples);
        output << "output=" << *parsed.output << " samples="
               << waveform.samples.size() << " hardware_exact=false\n";
        return 0;
      }
      case Command::Decode:
        return decode_iq(parsed, output, false);
      case Command::Scan:
        return decode_iq(parsed, output, true);
      case Command::Capture: {
        std::optional<radio::FileDevice> replay_device;
        auto* selected_device = environment.device;
        if (selected_device == nullptr && parsed.input.has_value()) {
          const auto replay_samples = read_cf32(*parsed.input);
          replay_device.emplace(replay_samples, parsed.sample_rate,
                                parsed.center_frequency_hz);
          selected_device = &*replay_device;
        }
        if (selected_device == nullptr) {
          throw std::runtime_error(
              "capture requires a selected device or file-device input");
        }
        std::vector<std::complex<float>> samples;
        const auto requested = parsed.sample_count == 0U
                                   ? std::numeric_limits<std::size_t>::max()
                                   : parsed.sample_count;
        while (samples.size() < requested) {
          const auto remaining = requested - samples.size();
          auto block = selected_device->receive(remaining);
          if (block.samples.empty()) break;
          samples.insert(samples.end(), block.samples.begin(),
                         block.samples.end());
        }
        evidence::Writer writer(*parsed.output, parsed.stem);
        writer.write_sigmf(
            samples,
            evidence::CaptureMetadata{
                parsed.sample_rate, parsed.center_frequency_hz, "file",
                "CLI selected device", parsed.utc, 0U, false});
        const auto reproduction =
            "bh61-radio decode --input " + parsed.stem +
            ".sigmf-data --sample-rate " +
            std::to_string(parsed.sample_rate) + " --jsonl";
        const auto artifacts = writer.finalize(reproduction);
        output << artifacts.meta_path.string() << '\n';
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
