#include "bh61/evidence/passive_capture.hpp"
#include "bh61/evidence/writer.hpp"
#include "bh61/radio/file_device.hpp"
#include "test_harness.hpp"

#include <array>
#include <complex>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

auto directory() -> std::filesystem::path {
  auto path = std::filesystem::temp_directory_path() / "bh61-passive-ingest";
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path);
  return path;
}

constexpr std::string_view frame =
    "1a41c800ff01010000000000000000000b00338b0000010000705c";

class TrackingDevice final : public bh61::radio::Device {
 public:
  std::size_t receive_calls{};
  std::size_t transmit_calls{};
  bool complete{};

  auto capabilities() const -> bh61::radio::DeviceCapabilities override {
    return {true, false, false, false, false,
            bh61::radio::SampleFormat::ComplexFloat32, 1U, 20'000'000U};
  }
  auto receive(std::size_t maximum) -> bh61::radio::SampleBlock override {
    ++receive_calls;
    if (complete || maximum == 0U) {
      return {{}, 2U, 20U, false, "",
              bh61::radio::SampleBlockStatus::EndOfStream};
    }
    complete = true;
    return {{{1.0F, 0.0F}, {0.0F, 1.0F}}, 0U, 10U, false, "",
            bh61::radio::SampleBlockStatus::Data};
  }
  void transmit(std::span<const std::complex<float>>, std::uint64_t) override {
    ++transmit_calls;
  }
};

}  // namespace

BH61_TEST("SigMF and event JSONL ingest into normalized passive records") {
  const auto path = directory();
  constexpr std::array<std::complex<float>, 2> samples{
      std::complex<float>{0.25F, -0.5F},
      std::complex<float>{0.75F, 0.125F}};
  bh61::evidence::Writer writer(path, "capture");
  writer.write_sigmf(samples, {4'000'000U, 915'350'000U, "b210",
                               "rx-only", "2026-08-23T12:00:00Z", 900U,
                               false});
  writer.append_decoded_event({"2026-08-23T12:00:00.001Z", 1000U,
                               std::string(frame), 0.99, 10U, false, 125.0});
  const auto artifacts = writer.finalize("bh61-radio decode --input capture.sigmf-meta");

  const auto result = bh61::evidence::ingest_sigmf(
      artifacts.meta_path, artifacts.data_path, artifacts.events_path);
  BH61_REQUIRE(std::holds_alternative<bh61::evidence::PassiveCapture>(result));
  const auto& capture = std::get<bh61::evidence::PassiveCapture>(result);
  BH61_REQUIRE(capture.sample_rate == 4'000'000U);
  BH61_REQUIRE(capture.center_frequency_hz == 915'350'000U);
  BH61_REQUIRE(capture.sample_count == 2U);
  BH61_REQUIRE(capture.frames.size() == 1U);
  BH61_REQUIRE(capture.frames[0].frame_hex == frame);
  BH61_REQUIRE(capture.frames[0].vcmp_type == 11U);
  BH61_REQUIRE(capture.frames[0].catalog_event == "scan-info-rx");
  BH61_REQUIRE(capture.frames[0].sample_rate == 4'000'000U);
  BH61_REQUIRE(capture.frames[0].center_frequency_hz == 915'350'000U);

  std::error_code error;
  std::filesystem::remove_all(path, error);
}

BH61_TEST("standalone JSONL normalizes RSSI and rejects a later malformed row atomically") {
  const auto path = directory();
  const auto valid = path / "valid.jsonl";
  {
    std::ofstream output(valid);
    output << "{\"kind\":\"decoded_frame\",\"utc\":\"2026-08-23T12:00:00Z\","
              "\"monotonic_ns\":12,\"center_frequency_hz\":915000000,"
              "\"sample_rate\":4000000,\"rssi_dbm\":-71.5,\"frame_hex\":\""
           << frame << "\"}\n";
  }
  const auto accepted = bh61::evidence::ingest_frame_jsonl(valid);
  BH61_REQUIRE(std::holds_alternative<bh61::evidence::PassiveCapture>(accepted));
  const auto& capture = std::get<bh61::evidence::PassiveCapture>(accepted);
  BH61_REQUIRE(capture.frames.size() == 1U);
  BH61_REQUIRE(capture.frames[0].rssi_dbm.has_value());
  BH61_REQUIRE(*capture.frames[0].rssi_dbm == -71.5);

  const auto invalid = path / "invalid.jsonl";
  {
    std::ofstream output(invalid);
    output << std::ifstream(valid).rdbuf();
    output << "{\"kind\":\"decoded_frame\",\"frame_hex\":\"abc\"}\n";
  }
  const auto rejected = bh61::evidence::ingest_frame_jsonl(invalid);
  BH61_REQUIRE(std::holds_alternative<bh61::evidence::PassiveIngestError>(rejected));
  BH61_REQUIRE(std::get<bh61::evidence::PassiveIngestError>(rejected).line == 2U);

  std::error_code error;
  std::filesystem::remove_all(path, error);
}

BH61_TEST("SigMF rejects unsupported datatype and non-whole samples") {
  const auto path = directory();
  const auto meta = path / "bad.sigmf-meta";
  const auto data = path / "bad.sigmf-data";
  const auto events = path / "bad.events.jsonl";
  {
    std::ofstream output(meta);
    output << "{\"global\":{\"core:datatype\":\"ci16_le\","
              "\"core:sample_rate\":4000000},\"captures\":[{"
              "\"core:frequency\":915000000,\"core:datetime\":\"now\"}]}\n";
  }
  { std::ofstream output(data, std::ios::binary); output << "123"; }
  { std::ofstream output(events); }
  const auto rejected = bh61::evidence::ingest_sigmf(meta, data, events);
  BH61_REQUIRE(std::holds_alternative<bh61::evidence::PassiveIngestError>(rejected));

  std::error_code error;
  std::filesystem::remove_all(path, error);
}

BH61_TEST("receive-only collector never invokes device transmit") {
  TrackingDevice device;
  const auto collected = bh61::evidence::collect_receive_only(device, 8U);
  BH61_REQUIRE(collected.size() == 2U);
  BH61_REQUIRE(device.receive_calls >= 1U);
  BH61_REQUIRE(device.transmit_calls == 0U);
}
