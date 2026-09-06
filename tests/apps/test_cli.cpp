#include "bh61/app/arguments.hpp"
#include "bh61/radio/device_factory.hpp"
#include "bh61/radio/file_device.hpp"
#include "test_harness.hpp"

#include <array>
#include <complex>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view exact_frame =
    "1a41c800ff01010000000000000000000b00338b00000100000e3a";
constexpr std::string_view exact_psdu =
    "41c800ff01010000000000000000000b00338b0000010000";

auto run(std::initializer_list<std::string_view> arguments,
         bh61::app::CliEnvironment environment = {})
    -> std::tuple<int, std::string, std::string> {
  const std::vector<std::string_view> values(arguments);
  std::ostringstream output;
  std::ostringstream error;
  const auto status = bh61::app::run_cli(values, output, error, environment);
  return {status, output.str(), error.str()};
}

auto fresh_directory() -> std::filesystem::path {
  const auto path = std::filesystem::temp_directory_path() /
                    "bh61-cli-contract";
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path);
  return path;
}

}  // namespace

namespace {

class TrackingFactory final : public bh61::radio::DeviceFactory {
 public:
  std::size_t enumerate_calls{};

  auto enumerate() -> std::vector<bh61::radio::DeviceDescriptor> override {
    ++enumerate_calls;
    return {{"hackrf",
             "00000000000000000000000000000001",
             "HackRF Pro",
             "git-9039eb06 (API:1.09)",
             "r1.2",
             {true, false, false, false, false,
              bh61::radio::SampleFormat::SignedInt8, 2'000'000U,
              20'000'000U}}};
  }
};

}  // namespace

BH61_TEST("devices and fixtures enumerate offline capabilities") {
  const auto [device_status, devices, device_error] = run({"devices"});
  BH61_REQUIRE(device_status == 0);
  BH61_REQUIRE(device_error.empty());
  BH61_REQUIRE(devices.find("file") != std::string::npos);

  const auto [fixture_status, fixtures, fixture_error] = run({"fixtures"});
  BH61_REQUIRE(fixture_status == 0);
  BH61_REQUIRE(fixture_error.empty());
  BH61_REQUIRE(fixtures.find("type11-f27-normal-destination") !=
               std::string::npos);
}

BH61_TEST("devices JSON distinguishes compiled and attached HackRF records") {
  TrackingFactory factory;
  const auto [status, output, error] =
      run({"devices", "--jsonl"}, bh61::app::CliEnvironment{nullptr, &factory});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  BH61_REQUIRE(factory.enumerate_calls == 1U);
  BH61_REQUIRE(output.find("\"kind\":\"compiled_backend\"") !=
               std::string::npos);
  BH61_REQUIRE(output.find("\"kind\":\"attached_device\"") !=
               std::string::npos);
  BH61_REQUIRE(output.find("\"serial\":\"00000000000000000000000000000001\"") !=
               std::string::npos);
  BH61_REQUIRE(output.find("\"board\":\"HackRF Pro\"") !=
               std::string::npos);
  BH61_REQUIRE(output.find("\"firmware\":\"git-9039eb06 (API:1.09)\"") !=
               std::string::npos);
  BH61_REQUIRE(output.find("\"tx\":false") != std::string::npos);
}

BH61_TEST("backend and serial options parse for later live commands") {
  const std::array<std::string_view, 5> values{
      "devices", "--backend", "hackrf", "--serial", "target"};
  const auto parsed = bh61::app::parse_arguments(values);
  BH61_REQUIRE(std::holds_alternative<bh61::app::Arguments>(parsed));
  const auto& arguments = std::get<bh61::app::Arguments>(parsed);
  BH61_REQUIRE(arguments.backend == "hackrf");
  BH61_REQUIRE(arguments.serial == "target");
}

BH61_TEST("inspect and build preserve exact recovered frame bytes") {
  const auto [inspect_status, inspection, inspect_error] =
      run({"inspect", "--hex", exact_frame});
  BH61_REQUIRE(inspect_status == 0);
  BH61_REQUIRE(inspect_error.empty());
  BH61_REQUIRE(inspection.find("phr=26") != std::string::npos);
  BH61_REQUIRE(inspection.find("radio_fcs=valid") != std::string::npos);
  BH61_REQUIRE(inspection.find("vmac.sequence=0") != std::string::npos);
  BH61_REQUIRE(inspection.find("vcmp.type=11") != std::string::npos);

  const auto [build_status, built, build_error] =
      run({"build", "--psdu-hex", exact_psdu});
  BH61_REQUIRE(build_status == 0);
  BH61_REQUIRE(build_error.empty());
  BH61_REQUIRE(built == std::string(exact_frame) + "\n");
}

BH61_TEST("waveform decode and scan execute the stored IQ path") {
  const auto directory = fresh_directory();
  const auto iq_path = directory / "scan.cf32";
  const auto iq = iq_path.string();
  const auto [waveform_status, waveform_output, waveform_error] =
      run({"waveform", "--frame-hex", exact_frame, "--output", iq,
           "--sample-rate", "4000000"});
  BH61_REQUIRE(waveform_status == 0);
  BH61_REQUIRE(waveform_error.empty());
  BH61_REQUIRE(waveform_output.find("samples=13400") != std::string::npos);
  BH61_REQUIRE(std::filesystem::file_size(iq_path) == 13'400U * 8U);

  const auto [decode_status, decoded, decode_error] =
      run({"decode", "--input", iq, "--sample-rate", "4000000",
           "--jsonl"});
  BH61_REQUIRE(decode_status == 0);
  BH61_REQUIRE(decode_error.empty());
  BH61_REQUIRE(decoded.find("\"frame_hex\":\"" +
                                std::string(exact_frame) + "\"") !=
               std::string::npos);
  BH61_REQUIRE(decoded.find("\"radio_fcs_valid\":true") !=
               std::string::npos);

  const auto [scan_status, scan, scan_error] =
      run({"scan", "--input", iq, "--sample-rate", "4000000"});
  BH61_REQUIRE(scan_status == 0);
  BH61_REQUIRE(scan_error.empty());
  BH61_REQUIRE(scan.find(std::string(exact_frame)) != std::string::npos);

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

BH61_TEST("capture writes evidence and never invokes transmit") {
  const auto directory = fresh_directory();
  constexpr std::array<std::complex<float>, 3> samples{
      std::complex<float>{1.0F, 0.0F}, std::complex<float>{0.0F, 1.0F},
      std::complex<float>{-1.0F, 0.0F}};
  bh61::radio::FileDevice device(samples, 4'000'000U, 915'350'000U);
  const auto output_directory = directory.string();
  const auto [status, output, error] = run(
      {"capture", "--output", output_directory, "--stem", "offline",
       "--sample-count", "3",
       "--utc", "2026-08-22T14:03:00.000000Z"},
      bh61::app::CliEnvironment{&device});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  BH61_REQUIRE(output.find("offline.sigmf-meta") != std::string::npos);
  BH61_REQUIRE(std::filesystem::exists(directory / "offline.sigmf-data"));
  BH61_REQUIRE(std::filesystem::exists(directory / "offline.sha256"));
  BH61_REQUIRE(device.transmit_records().empty());

  std::error_code remove_error;
  std::filesystem::remove_all(directory, remove_error);
}

BH61_TEST("capture replays raw IQ without an attached device") {
  const auto directory = fresh_directory();
  const auto input_path = directory / "input.cf32";
  constexpr std::array<std::complex<float>, 2> samples{
      std::complex<float>{0.125F, -0.25F},
      std::complex<float>{0.5F, 0.75F}};
  {
    std::ofstream input(input_path, std::ios::binary);
    input.write(reinterpret_cast<const char*>(samples.data()),
                static_cast<std::streamsize>(sizeof(samples)));
  }
  const auto input = input_path.string();
  const auto output_directory = directory.string();
  const auto [status, output, error] =
      run({"capture", "--input", input, "--output", output_directory,
           "--stem", "replayed", "--sample-rate", "4000000"});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  BH61_REQUIRE(output.find("replayed.sigmf-meta") != std::string::npos);
  BH61_REQUIRE(std::filesystem::file_size(directory /
                                         "replayed.sigmf-data") ==
               sizeof(samples));

  std::error_code remove_error;
  std::filesystem::remove_all(directory, remove_error);
}

BH61_TEST("decode resolves a SigMF metadata path to its data and sample rate") {
  const auto directory = fresh_directory();
  const auto raw_path = directory / "source.cf32";
  const auto raw = raw_path.string();
  const auto output_directory = directory.string();
  const auto [waveform_status, waveform_output, waveform_error] =
      run({"waveform", "--frame-hex", exact_frame, "--output", raw,
           "--sample-rate", "4000000"});
  BH61_REQUIRE(waveform_status == 0);
  BH61_REQUIRE(!waveform_output.empty());
  BH61_REQUIRE(waveform_error.empty());
  const auto [capture_status, capture_output, capture_error] =
      run({"capture", "--input", raw, "--output", output_directory,
           "--stem", "pair", "--sample-rate", "4000000"});
  BH61_REQUIRE(capture_status == 0);
  BH61_REQUIRE(!capture_output.empty());
  BH61_REQUIRE(capture_error.empty());

  const auto meta = (directory / "pair.sigmf-meta").string();
  const auto [decode_status, decoded, decode_error] =
      run({"decode", "--input", meta, "--jsonl"});
  BH61_REQUIRE(decode_status == 0);
  BH61_REQUIRE(decode_error.empty());
  BH61_REQUIRE(decoded.find(std::string(exact_frame)) != std::string::npos);

  std::error_code remove_error;
  std::filesystem::remove_all(directory, remove_error);
}

BH61_TEST("invalid CLI values emit one structured error and nonzero status") {
  const auto [status, output, error] =
      run({"decode", "--input", "missing.cf32", "--sample-rate", "zero"});
  BH61_REQUIRE(status != 0);
  BH61_REQUIRE(output.empty());
  BH61_REQUIRE(error.starts_with("{\"error\":{\"code\":"));
  BH61_REQUIRE(error.find('\n') == error.size() - 1U);
}

BH61_TEST("CLI exposes no transmit command in the receive-only build") {
  const auto [status, output, error] = run({"transmit"});
  BH61_REQUIRE(status == 2);
  BH61_REQUIRE(output.empty());
  BH61_REQUIRE(error.find("unknown_command") != std::string::npos);
}
