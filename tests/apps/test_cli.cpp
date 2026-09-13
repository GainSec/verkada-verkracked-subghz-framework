#include "bh61/app/arguments.hpp"
#include "bh61/core/sensor_session.hpp"
#include "bh61/radio/device_factory.hpp"
#include "bh61/radio/file_device.hpp"
#include "test_harness.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <optional>
#include <numbers>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view exact_frame =
    "1a41c800ff01010000000000000000000b00338b0000010000705c";
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
  std::size_t select_calls{};
  std::optional<bh61::radio::DeviceRequest> selected_request;
  bh61::radio::FileDevice selected{{}, 4'000'000U, 915'350'000U};

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

  auto select(const bh61::radio::DeviceRequest& request)
      -> bh61::radio::Device* override {
    ++select_calls;
    selected_request = request;
    BH61_REQUIRE(request.backend == "hackrf");
    BH61_REQUIRE(request.serial == "target");
    BH61_REQUIRE(request.sample_rate == 4'000'000U);
    BH61_REQUIRE(request.center_frequency_hz == 915'350'000U);
    return &selected;
  }
};

class PairDevice final : public bh61::radio::Device {
 public:
  auto capabilities() const -> bh61::radio::DeviceCapabilities override {
    return {true, false, true, true, false,
            bh61::radio::SampleFormat::ComplexFloat32, 1'000'000U,
            61'440'000U};
  }
  auto receive(std::size_t) -> bh61::radio::SampleBlock override {
    return {};
  }
  auto receive_pair(std::size_t count)
      -> bh61::radio::CoherentSamplePair override {
    std::vector<std::complex<float>> a(count, {1.0F, 0.0F});
    std::vector<std::complex<float>> b(count, {0.0F, 1.0F});
    return {{std::move(a), 0U, 42U, false, {},
             bh61::radio::SampleBlockStatus::Data},
            {std::move(b), 0U, 42U, false, {},
             bh61::radio::SampleBlockStatus::Data}};
  }
  void transmit(std::span<const std::complex<float>>, std::uint64_t) override {
    throw std::logic_error("not used");
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

BH61_TEST("live capture requests only a single receive stream") {
  const auto directory = fresh_directory();
  TrackingFactory factory;
  const auto [status, output, error] = run(
      {"capture", "--output", directory.string(), "--stem", "live-rx",
       "--sample-count", "1", "--sample-rate", "4000000",
       "--center-frequency", "915350000", "--backend", "hackrf",
       "--serial", "target"},
      bh61::app::CliEnvironment{nullptr, &factory});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  BH61_REQUIRE(factory.selected_request.has_value());
  BH61_REQUIRE(factory.selected_request->receive);
  BH61_REQUIRE(!factory.selected_request->coherent_receive);
  BH61_REQUIRE(!factory.selected_request->transmit);
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

BH61_TEST("public stateful sensor commands enforce the owner-lab boundary") {
  const auto base = [](std::string_view command,
                       std::initializer_list<std::string_view> tail = {}) {
    std::vector<std::string_view> values{
        command, "--session", "synthetic-session", "--output", "evidence",
        "--preflight", "preflight.json", "--backend", "b210", "--serial",
        "B210-SYNTHETIC", "--fpga", "usrp_b210_fpga.bin", "--sample-rate",
        "4000000", "--center-frequency", "915350000", "--rx-timeout-ms",
        "100", "--dry-run"};
    values.insert(values.end(), tail.begin(), tail.end());
    return bh61::app::parse_arguments(values);
  };

  BH61_REQUIRE(std::holds_alternative<bh61::app::Arguments>(
      base("sensor-join")));
  BH61_REQUIRE(std::holds_alternative<bh61::app::Arguments>(
      base("sensor-event", {"--event", "heartbeat", "--event-id", "1"})));
  BH61_REQUIRE(std::holds_alternative<bh61::app::Arguments>(
      base("sensor-rpc-read", {"--rpc", "image-state"})));

  for (const auto command : {"sensor-counter-resync-test", "replay",
                             "mutate", "mutate-transmit"}) {
    const std::array<std::string_view, 1> values{command};
    BH61_REQUIRE(std::holds_alternative<bh61::app::ArgumentError>(
        bh61::app::parse_arguments(values)));
  }
  BH61_REQUIRE(std::holds_alternative<bh61::app::ArgumentError>(
      base("sensor-event", {"--event", "heartbeat", "--event-id", "1",
                            "--downlink-mac-ack-mode", "suppress"})));
}

BH61_TEST("stateful sensor radio options are explicit and bounded") {
  const auto parse_join = [](std::string_view option,
                             std::string_view value) {
    const auto backend = option == "--backend" ? value : "b210";
    const auto sample_rate = option == "--sample-rate" ? value : "4000000";
    const auto frequency = option == "--center-frequency" ? value : "915350000";
    const auto timeout = option == "--rx-timeout-ms" ? value : "100";
    const std::vector<std::string_view> values{
        "sensor-join", "--session", "synthetic-session", "--output",
        "evidence", "--preflight", "preflight.json", "--backend", backend,
        "--serial", "B210-SYNTHETIC", "--fpga", "usrp_b210_fpga.bin",
        "--sample-rate", sample_rate, "--center-frequency", frequency,
        "--rx-timeout-ms", timeout, "--dry-run"};
    if (option == "--tx-gain") {
      auto with_gain = values;
      with_gain.insert(with_gain.end(), {"--tx-gain", value});
      return bh61::app::parse_arguments(with_gain);
    }
    return bh61::app::parse_arguments(values);
  };
  BH61_REQUIRE(std::holds_alternative<bh61::app::ArgumentError>(
      parse_join("--backend", "hackrf")));
  BH61_REQUIRE(std::holds_alternative<bh61::app::ArgumentError>(
      parse_join("--center-frequency", "928000001")));
  BH61_REQUIRE(std::holds_alternative<bh61::app::ArgumentError>(
      parse_join("--sample-rate", "3999999")));
  BH61_REQUIRE(std::holds_alternative<bh61::app::ArgumentError>(
      parse_join("--rx-timeout-ms", "99")));
  BH61_REQUIRE(std::holds_alternative<bh61::app::ArgumentError>(
      parse_join("--tx-gain", "86")));
}

BH61_TEST("sensor create status and retire do not disclose private material") {
  const auto directory = fresh_directory();
  const auto session = directory / "synthetic-session";
  const auto retirement = directory / "retirement";
  const auto [create_status, created, create_error] = run({
      "sensor-create", "--session", session.string(), "--serial",
      "SYN-DOOR-PUBLIC-0001", "--model", "door-contact"});
  BH61_REQUIRE(create_status == 0);
  BH61_REQUIRE(create_error.empty());
  BH61_REQUIRE(std::filesystem::exists(session / "sensor-private.key"));
  BH61_REQUIRE(std::filesystem::exists(session / "cloud-registration.json"));
  BH61_REQUIRE(created.find("private") == std::string::npos);
  const auto private_mode = std::filesystem::status(
      session / "sensor-private.key").permissions();
  BH61_REQUIRE((private_mode & (std::filesystem::perms::group_all |
                               std::filesystem::perms::others_all)) ==
               std::filesystem::perms::none);
  std::ifstream registration_stream(session / "cloud-registration.json");
  const std::string registration{std::istreambuf_iterator<char>(registration_stream),
                                 std::istreambuf_iterator<char>()};
  BH61_REQUIRE(registration.find("private") == std::string::npos);
  BH61_REQUIRE(registration.find("session_key") == std::string::npos);

  const auto [status_code, status, status_error] =
      run({"sensor-status", "--session", session.string(), "--jsonl"});
  BH61_REQUIRE(status_code == 0);
  BH61_REQUIRE(status_error.empty());
  BH61_REQUIRE(status.find("\"state\":\"created\"") != std::string::npos);
  BH61_REQUIRE(status.find("private") == std::string::npos);

  const auto [retire_status, retired, retire_error] = run({
      "sensor-retire", "--session", session.string(), "--output",
      retirement.string()});
  BH61_REQUIRE(retire_status == 0);
  BH61_REQUIRE(retire_error.empty());
  BH61_REQUIRE(std::filesystem::exists(retirement / "retirement.json"));
  BH61_REQUIRE(retired.find("SYN-DOOR-PUBLIC-0001") != std::string::npos);
}

BH61_TEST("sensor join dry-run creates evidence and does not advance state") {
  const auto directory = fresh_directory();
  const auto session = directory / "dry-session";
  const auto evidence = directory / "dry-evidence";
  const auto preflight = directory / "preflight.json";
  std::ofstream(preflight) << "{\"schema\":\"bh61.isolation-preflight/v1\","
                              "\"overall\":\"pass\"}\n";
  BH61_REQUIRE(std::get<0>(run({"sensor-create", "--session",
                                session.string(), "--serial",
                                "SYN-DRY-PUBLIC-0001", "--model",
                                "door-contact"})) == 0);
  const auto [status, output, error] = run({
      "sensor-join", "--session", session.string(), "--output",
      evidence.string(), "--preflight", preflight.string(), "--backend",
      "b210", "--serial", "B210-SYNTHETIC", "--fpga",
      "usrp_b210_fpga.bin", "--sample-rate", "4000000",
      "--center-frequency", "915350000", "--rx-timeout-ms", "100",
      "--dry-run"});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  BH61_REQUIRE(output.find("dry_run=true") != std::string::npos);
  BH61_REQUIRE(std::filesystem::exists(evidence / "join.frame.bin"));
  BH61_REQUIRE(std::filesystem::exists(evidence / "join.waveform.cf32"));
  const auto loaded = bh61::core::SensorSession::load(session);
  BH61_REQUIRE(loaded.state() == bh61::core::SensorSessionState::Created);
}

BH61_TEST("CLI accepts the B210 transmit gain and alternate FPGA") {
  const std::array<std::string_view, 16> values{
      "transmit", "--frame-hex", exact_frame, "--output", "evidence",
      "--backend", "b210", "--serial", "B210_SYNTHETIC", "--tx-gain", "80",
      "--sample-rate", "4000000", "--fpga", "/lab/usrp_b210_fpga.bin",
      "--enable-tx"};
  const auto parsed = bh61::app::parse_arguments(values);
  BH61_REQUIRE(std::holds_alternative<bh61::app::Arguments>(parsed));
  BH61_REQUIRE(std::get<bh61::app::Arguments>(parsed).tx_gain_db == 80U);
  BH61_REQUIRE(std::get<bh61::app::Arguments>(parsed).fpga ==
               "/lab/usrp_b210_fpga.bin");
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

BH61_TEST("secure-frame builds a complete decryptable protected radio frame") {
  const auto [status, output, error] = run({
      "secure-frame", "--source-eui", "3132333435363738",
      "--destination", "1", "--pan", "65535", "--sequence", "234",
      "--type", "1", "--flags", "1", "--counter", "4660",
      "--payload-hex", "01020304",
      "--key-hex", "30313233343536373839414243444546",
      "--iv-hex", "000102030405060708090a0b0c0d0e0f"});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());

  auto encoded = output;
  BH61_REQUIRE(!encoded.empty() && encoded.back() == '\n');
  encoded.pop_back();
  const auto [inspect_status, inspection, inspect_error] =
      run({"inspect", "--hex", encoded});
  BH61_REQUIRE(inspect_status == 0);
  BH61_REQUIRE(inspect_error.empty());
  BH61_REQUIRE(inspection.find("radio_fcs=valid") != std::string::npos);
  BH61_REQUIRE(inspection.find("vcmp.type=1") != std::string::npos);
}

BH61_TEST("join-frame builds the recovered type-7 join layout and checksums") {
  const auto [status, output, error] = run({
      "join-frame", "--source-eui", "4142434445464748",
      "--destination", "1", "--pan", "511", "--sequence", "235",
      "--serial-hex", "5a"});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  auto encoded = output;
  BH61_REQUIRE(!encoded.empty() && encoded.back() == '\n');
  encoded.pop_back();
  const auto [inspect_status, inspection, inspect_error] =
      run({"inspect", "--hex", encoded});
  BH61_REQUIRE(inspect_status == 0);
  BH61_REQUIRE(inspect_error.empty());
  BH61_REQUIRE(inspection.find("radio_fcs=valid") != std::string::npos);
  BH61_REQUIRE(inspection.find("vcmp.type=7") != std::string::npos);
}

BH61_TEST("scan-complete-frame builds the recovered plaintext type-11 body") {
  const auto [status, output, error] = run({
      "scan-complete-frame", "--source-eui", "5152535455565758",
      "--destination", "1", "--pan", "511", "--sequence", "236",
      "--channel", "1", "--regulatory-code", "1", "--scan-sequence", "9"});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  auto encoded = output;
  BH61_REQUIRE(!encoded.empty() && encoded.back() == '\n');
  encoded.pop_back();
  const auto [inspect_status, inspection, inspect_error] =
      run({"inspect", "--hex", encoded});
  BH61_REQUIRE(inspect_status == 0);
  BH61_REQUIRE(inspect_error.empty());
  BH61_REQUIRE(inspection.find("radio_fcs=valid") != std::string::npos);
  BH61_REQUIRE(inspection.find("vcmp.type=11") != std::string::npos);
}

BH61_TEST("waveform decode and scan execute the stored IQ path") {
  const auto directory = fresh_directory();
  const auto iq_path = directory / "scan.cf32";
  const auto iq = iq_path.string();
  const auto [waveform_status, waveform_output, waveform_error] =
      run({"waveform", "--frame-hex", exact_frame, "--output", iq,
           "--sample-rate", "4000000", "--waveform-model",
           "analytic-half-sine"});
  BH61_REQUIRE(waveform_status == 0);
  BH61_REQUIRE(waveform_error.empty());
  BH61_REQUIRE(waveform_output.find("samples=13400") != std::string::npos);
  BH61_REQUIRE(waveform_output.find("hardware_exact=false") !=
               std::string::npos);
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

BH61_TEST("waveform defaults to the live-validated EFR32 mode 2 model") {
  const auto directory = fresh_directory();
  const auto iq_path = directory / "efr32.cf32";
  const auto [status, output, error] =
      run({"waveform", "--frame-hex", exact_frame, "--output",
           iq_path.string(), "--sample-rate", "4000000"});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  BH61_REQUIRE(output.find("hardware_exact=true") != std::string::npos);
  BH61_REQUIRE(output.find("waveform_model=efr32-custom-oqpsk-mode2") !=
               std::string::npos);
  BH61_REQUIRE(std::filesystem::file_size(iq_path) == 13'400U * 8U);
}

BH61_TEST("decode applies an explicit carrier correction to captured IQ") {
  const auto directory = fresh_directory();
  const auto iq_path = directory / "offset.cf32";
  const auto iq = iq_path.string();
  const auto [waveform_status, waveform_output, waveform_error] =
      run({"waveform", "--frame-hex", exact_frame, "--output", iq,
           "--sample-rate", "4000000", "--waveform-model",
           "analytic-half-sine"});
  BH61_REQUIRE(waveform_status == 0);
  BH61_REQUIRE(waveform_error.empty());

  std::fstream samples(iq_path, std::ios::in | std::ios::out |
                                    std::ios::binary);
  BH61_REQUIRE(samples.good());
  std::vector<std::complex<float>> values(13'400U);
  samples.read(reinterpret_cast<char*>(values.data()),
               static_cast<std::streamsize>(values.size() * sizeof(values[0])));
  for (std::size_t index = 0; index < values.size(); ++index) {
    const auto angle = 2.0 * std::numbers::pi * 1'000.0 *
                       static_cast<double>(index) / 4'000'000.0;
    values[index] *= std::complex<float>{static_cast<float>(std::cos(angle)),
                                         static_cast<float>(std::sin(angle))};
  }
  samples.seekp(0);
  samples.write(reinterpret_cast<const char*>(values.data()),
                static_cast<std::streamsize>(values.size() * sizeof(values[0])));
  samples.close();

  const auto [status, output, error] =
      run({"decode", "--input", iq, "--sample-rate", "4000000",
           "--carrier-offset-hz", "-1000", "--jsonl"});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  BH61_REQUIRE(output.find("\"carrier_offset_hz\":-1000") !=
               std::string::npos);

  std::error_code cleanup_error;
  std::filesystem::remove_all(directory, cleanup_error);
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
           "--sample-rate", "4000000", "--waveform-model",
           "analytic-half-sine"});
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
  BH61_REQUIRE(error.find("missing_option") != std::string::npos);
}

BH61_TEST("transmit requires explicit acknowledgement before device use") {
  const auto directory = fresh_directory();
  const auto input_path = directory / "tx.cf32";
  constexpr std::array<std::complex<float>, 2> samples{
      std::complex<float>{0.25F, -0.5F},
      std::complex<float>{-0.75F, 0.125F}};
  {
    std::ofstream input(input_path, std::ios::binary);
    input.write(reinterpret_cast<const char*>(samples.data()),
                static_cast<std::streamsize>(sizeof(samples)));
  }
  bh61::radio::FileDevice device({}, 4'000'000U, 915'350'000U);
  const auto input = input_path.string();
  const auto output_directory = directory.string();
  const auto [status, output, error] = run(
      {"transmit", "--input", input, "--output", output_directory,
       "--stem", "denied", "--sample-rate", "4000000",
       "--center-frequency", "915350000"},
      bh61::app::CliEnvironment{&device});
  BH61_REQUIRE(status != 0);
  BH61_REQUIRE(output.empty());
  BH61_REQUIRE(error.find("tx_acknowledgement_required") !=
               std::string::npos);
  BH61_REQUIRE(device.transmit_records().empty());
}

BH61_TEST("transmit sends file samples and writes an evidence record") {
  const auto directory = fresh_directory();
  const auto input_path = directory / "tx.cf32";
  constexpr std::array<std::complex<float>, 3> samples{
      std::complex<float>{0.25F, -0.5F},
      std::complex<float>{-0.75F, 0.125F},
      std::complex<float>{0.0F, 0.0F}};
  {
    std::ofstream input(input_path, std::ios::binary);
    input.write(reinterpret_cast<const char*>(samples.data()),
                static_cast<std::streamsize>(sizeof(samples)));
  }
  bh61::radio::FileDevice device({}, 4'000'000U, 915'350'000U);
  const auto input = input_path.string();
  const auto output_directory = directory.string();
  const auto [status, output, error] = run(
      {"transmit", "--input", input, "--output", output_directory,
       "--stem", "allowed", "--sample-rate", "4000000",
       "--center-frequency", "915350000", "--tx-gain", "7", "--enable-tx"},
      bh61::app::CliEnvironment{&device});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  BH61_REQUIRE(device.transmit_records().size() == 1U);
  BH61_REQUIRE(device.transmitted_samples().size() == samples.size());
  BH61_REQUIRE(std::filesystem::exists(directory / "allowed.tx.json"));
  BH61_REQUIRE(std::filesystem::exists(directory / "allowed.tx.sha256"));
  BH61_REQUIRE(std::filesystem::exists(directory / "allowed.events.jsonl"));
  BH61_REQUIRE(output.find("allowed.tx.json") != std::string::npos);
  std::ifstream record(directory / "allowed.tx.json");
  const std::string record_text((std::istreambuf_iterator<char>(record)),
                                std::istreambuf_iterator<char>());
  BH61_REQUIRE(record_text.find(R"("tx_gain_db":7)") != std::string::npos);
}

BH61_TEST("timed transmit passes an absolute device time and records it") {
  const auto directory = fresh_directory();
  const auto input_path = directory / "timed.cf32";
  constexpr std::array<std::complex<float>, 1> samples{
      std::complex<float>{0.25F, -0.5F}};
  {
    std::ofstream input(input_path, std::ios::binary);
    input.write(reinterpret_cast<const char*>(samples.data()),
                static_cast<std::streamsize>(sizeof(samples)));
  }
  bh61::radio::FileDevice device({}, 4'000'000U, 915'350'000U);
  const auto [status, output, error] = run(
      {"transmit", "--input", input_path.string(), "--output",
       directory.string(), "--stem", "timed", "--sample-rate", "4000000",
       "--at-ns", "987654321", "--enable-tx"},
      bh61::app::CliEnvironment{&device});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  BH61_REQUIRE(device.transmit_records().size() == 1U);
  BH61_REQUIRE(device.transmit_records()[0].requested_time_ns == 987654321U);
  std::ifstream record(directory / "timed.tx.json");
  const std::string text((std::istreambuf_iterator<char>(record)),
                         std::istreambuf_iterator<char>());
  BH61_REQUIRE(text.find(R"("requested_time_ns":987654321)") !=
               std::string::npos);
}

BH61_TEST("dry-run validates TX without invoking the device") {
  const auto directory = fresh_directory();
  const auto input_path = directory / "tx.cf32";
  constexpr std::array<std::complex<float>, 1> samples{
      std::complex<float>{0.0F, 0.0F}};
  {
    std::ofstream input(input_path, std::ios::binary);
    input.write(reinterpret_cast<const char*>(samples.data()),
                static_cast<std::streamsize>(sizeof(samples)));
  }
  bh61::radio::FileDevice device({}, 4'000'000U, 915'350'000U);
  const auto input = input_path.string();
  const auto output_directory = directory.string();
  const auto [status, output, error] = run(
      {"transmit", "--input", input, "--output", output_directory,
       "--stem", "dry", "--sample-rate", "4000000", "--dry-run"},
      bh61::app::CliEnvironment{&device});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  BH61_REQUIRE(device.transmit_records().empty());
  BH61_REQUIRE(std::filesystem::exists(directory / "dry.tx.json"));
  BH61_REQUIRE(output.find("dry_run=true") != std::string::npos);
}

BH61_TEST("transmit selects an attached backend when no device is injected") {
  const auto directory = fresh_directory();
  const auto input_path = directory / "tx.cf32";
  constexpr std::array<std::complex<float>, 1> samples{
      std::complex<float>{0.0F, 0.0F}};
  {
    std::ofstream input(input_path, std::ios::binary);
    input.write(reinterpret_cast<const char*>(samples.data()),
                static_cast<std::streamsize>(sizeof(samples)));
  }
  TrackingFactory factory;
  const auto [status, output, error] = run(
      {"transmit", "--input", input_path.string(), "--output",
       directory.string(), "--backend", "hackrf", "--serial", "target",
       "--sample-rate", "4000000", "--enable-tx"},
      bh61::app::CliEnvironment{nullptr, &factory});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  BH61_REQUIRE(factory.select_calls == 1U);
  BH61_REQUIRE(factory.selected.transmit_records().size() == 1U);
}

BH61_TEST("live IQ transmit requires explicit sample rate before device open") {
  const auto parsed = bh61::app::parse_arguments(
      std::array<std::string_view, 8>{"transmit", "--input", "capture.sigmf-meta",
                                      "--output", "evidence", "--backend",
                                      "hackrf", "--enable-tx"});
  BH61_REQUIRE(std::holds_alternative<bh61::app::ArgumentError>(parsed));
  BH61_REQUIRE(std::get<bh61::app::ArgumentError>(parsed).code ==
               "missing_option");
}

BH61_TEST("transmit builds a validated BH61 frame waveform directly") {
  const auto directory = fresh_directory();
  bh61::radio::FileDevice device({}, 4'000'000U, 915'350'000U);
  const auto [status, output, error] = run(
      {"transmit", "--frame-hex", exact_frame, "--output",
       directory.string(), "--stem", "direct-frame", "--sample-rate",
       "4000000", "--enable-tx"},
      bh61::app::CliEnvironment{&device});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  BH61_REQUIRE(device.transmit_records().size() == 1U);
  BH61_REQUIRE(device.transmitted_samples().size() == 13'400U);
  const auto peak = std::max_element(
      device.transmitted_samples().begin(), device.transmitted_samples().end(),
      [](const auto left, const auto right) {
        return std::abs(left) < std::abs(right);
      });
  BH61_REQUIRE(peak != device.transmitted_samples().end());
  BH61_REQUIRE(std::abs(*peak) < 0.53F);
  BH61_REQUIRE(std::filesystem::exists(directory /
                                      "direct-frame.waveform.cf32"));
  BH61_REQUIRE(std::filesystem::exists(directory /
                                      "direct-frame.events.jsonl"));
  std::ifstream record(directory / "direct-frame.tx.json");
  const std::string record_text((std::istreambuf_iterator<char>(record)),
                                std::istreambuf_iterator<char>());
  BH61_REQUIRE(record_text.find("\"hardware_exact\":true") !=
               std::string::npos);
  BH61_REQUIRE(record_text.find(
                   "\"amplitude\":0.5,\"waveform_sha256\":") !=
               std::string::npos);
  BH61_REQUIRE(record_text.find(
                   "\"waveform_model\":\"efr32-custom-oqpsk-mode2\"") !=
               std::string::npos);
}

BH61_TEST("transmit accepts a comma-separated frame sequence in one device burst") {
  const auto directory = fresh_directory();
  bh61::radio::FileDevice device({}, 4'000'000U, 915'350'000U);
  const auto frames = std::string(exact_frame) + "," + std::string(exact_frame);
  const auto [status, output, error] = run(
      {"transmit", "--frame-hex", frames, "--output", directory.string(),
       "--stem", "frame-sequence", "--sample-rate", "4000000",
       "--interval-ms", "1", "--enable-tx"},
      bh61::app::CliEnvironment{&device});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  BH61_REQUIRE(device.transmit_records().size() == 1U);
  BH61_REQUIRE(device.transmitted_samples().size() == 30'800U);
  std::ifstream record(directory / "frame-sequence.tx.json");
  const std::string record_text((std::istreambuf_iterator<char>(record)),
                                std::istreambuf_iterator<char>());
  BH61_REQUIRE(record_text.find("\"frame_count\":2") != std::string::npos);
}

BH61_TEST("public CLI rejects replay and mutation commands") {
  for (const auto command : {"replay", "mutate", "mutate-transmit"}) {
    const auto [status, output, error] = run({command});
    BH61_REQUIRE(status == 2);
    BH61_REQUIRE(output.empty());
    BH61_REQUIRE(error.find("unknown_command") != std::string::npos);
  }
}

BH61_TEST("simulate executes a sensor scenario into correlated lab events") {
  const auto directory = fresh_directory();
  const auto scenario = directory / "door.scenario";
  {
    std::ofstream out(scenario);
    out << "sensor door-7\nenroll\ncontact open\nsupervision\nretransmit\n";
  }
  const auto events = (directory / "sensor.events.jsonl").string();
  const auto [status, output, error] =
      run({"simulate", "--input", scenario.string(), "--output", events,
           "--stem", "walk-test", "--utc", "2026-09-06T15:04:05Z"});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  std::ifstream input(events);
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  BH61_REQUIRE(text.find("sensor.contact.open") != std::string::npos);
  BH61_REQUIRE(text.find(R"("sensor_id":"door-7")") != std::string::npos);
  BH61_REQUIRE(text.find(R"("retransmission":true)") != std::string::npos);
  BH61_REQUIRE(output.find("events=4") != std::string::npos);
}

BH61_TEST("pcap exports a BH61 frame with a Wireshark user link type") {
  const auto directory = fresh_directory();
  const auto path = directory / "frame.pcap";
  const auto [status, output, error] =
      run({"pcap", "--hex", exact_frame, "--output", path.string()});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  std::ifstream input(path, std::ios::binary);
  std::array<unsigned char, 24> header{};
  input.read(reinterpret_cast<char*>(header.data()), header.size());
  BH61_REQUIRE(header[0] == 0xd4U && header[1] == 0xc3U &&
               header[2] == 0xb2U && header[3] == 0xa1U);
  BH61_REQUIRE(header[20] == 147U && header[21] == 0U);
  BH61_REQUIRE(std::filesystem::file_size(path) == 24U + 16U + 27U);
  BH61_REQUIRE(output.find("packets=1") != std::string::npos);
}

BH61_TEST("df computes calibrated phase and amplitude from two CF32 channels") {
  const auto directory = fresh_directory();
  const auto a = directory / "a.cf32";
  const auto b = directory / "b.cf32";
  constexpr std::array<std::complex<float>, 2> channel_a{{{1.0F, 0.0F}, {1.0F, 0.0F}}};
  constexpr std::array<std::complex<float>, 2> channel_b{{{0.0F, 1.0F}, {0.0F, 1.0F}}};
  for (const auto& pair : {std::pair{a, channel_a}, std::pair{b, channel_b}}) {
    std::ofstream out(pair.first, std::ios::binary);
    out.write(reinterpret_cast<const char*>(pair.second.data()), sizeof(pair.second));
  }
  const auto report = (directory / "df.json").string();
  const auto [status, output, error] =
      run({"df", "--input", a.string(), "--input-b", b.string(),
           "--output", report, "--calibration-phase-rad", "0.0"});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  std::ifstream in(report);
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  BH61_REQUIRE(text.find(R"("schema":"bh61.df.observation/v1")") != std::string::npos);
  BH61_REQUIRE(text.find(R"("sample_count":2)") != std::string::npos);
  BH61_REQUIRE(text.find("1.570") != std::string::npos);
  BH61_REQUIRE(output.find("bearing_claimed=false") != std::string::npos);
}

BH61_TEST("df-capture preserves coherent channels and calibration evidence") {
  const auto directory = fresh_directory();
  PairDevice device;
  const auto [status, output, error] = run(
      {"df-capture", "--output", directory.string(), "--stem", "array-a",
       "--sample-count", "8", "--sample-rate", "4000000",
       "--center-frequency", "915350000", "--calibration-phase-rad", "0.5",
       "--antenna", "two-element-linear"},
      bh61::app::CliEnvironment{&device});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  BH61_REQUIRE(std::filesystem::file_size(directory / "array-a.ch0.cf32") ==
               8U * sizeof(std::complex<float>));
  BH61_REQUIRE(std::filesystem::file_size(directory / "array-a.ch1.cf32") ==
               8U * sizeof(std::complex<float>));
  std::ifstream report(directory / "array-a.df.json");
  const std::string text((std::istreambuf_iterator<char>(report)),
                         std::istreambuf_iterator<char>());
  BH61_REQUIRE(text.find(R"("coherent_capture":true)") != std::string::npos);
  BH61_REQUIRE(text.find(R"("bearing_claimed":false)") != std::string::npos);
  BH61_REQUIRE(text.find(R"("device_time_ns":42)") != std::string::npos);
  BH61_REQUIRE(std::filesystem::exists(directory / "array-a.sha256"));
}

BH61_TEST("coverage records location antenna gain and loss estimate") {
  const auto directory = fresh_directory();
  const auto report = (directory / "coverage.json").string();
  const auto [status, output, error] = run(
      {"coverage", "--output", report, "--location", "north-door",
       "--antenna", "915-whip", "--gain-db", "12.5", "--packets", "100",
       "--valid", "95", "--score", "0.88"});
  BH61_REQUIRE(status == 0);
  BH61_REQUIRE(error.empty());
  std::ifstream in(report);
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  BH61_REQUIRE(text.find(R"("schema":"bh61.coverage.observation/v1")") != std::string::npos);
  BH61_REQUIRE(text.find(R"("loss_estimate":0.05)") != std::string::npos);
  BH61_REQUIRE(text.find("north-door") != std::string::npos);
  BH61_REQUIRE(output.find("valid=95/100") != std::string::npos);
}
