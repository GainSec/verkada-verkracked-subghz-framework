#include "bh61/evidence/writer.hpp"
#include "test_harness.hpp"

#include <array>
#include <complex>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

auto read_text(const std::filesystem::path& path) -> std::string {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

auto make_test_directory() -> std::filesystem::path {
  const auto path = std::filesystem::temp_directory_path() /
                    "bh61-evidence-writer-contract";
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path);
  return path;
}

}  // namespace

BH61_TEST("SHA256 file hashing matches the published abc vector") {
  const auto directory = make_test_directory();
  const auto path = directory / "abc.txt";
  {
    std::ofstream output(path, std::ios::binary);
    output << "abc";
  }
  BH61_REQUIRE(
      bh61::evidence::sha256_file(path) ==
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

BH61_TEST("evidence writer emits reopenable SigMF JSONL and SHA256 manifest") {
  const auto directory = make_test_directory();
  bh61::evidence::Writer writer(directory, "capture-001");
  constexpr std::array<std::complex<float>, 3> samples{
      std::complex<float>{1.0F, -1.0F}, std::complex<float>{0.5F, 0.25F},
      std::complex<float>{0.0F, 0.75F}};
  writer.write_sigmf(
      samples,
      bh61::evidence::CaptureMetadata{
          4'000'000U, 915'350'000U, "file", "canonical-fixture",
          "2026-08-22T14:03:00.000000Z", 98'765U, false});
  writer.append_decoded_event(bh61::evidence::DecodedEvent{
      "2026-08-22T14:03:00.001000Z", 99'765U,
      "1a41c800ff01010000000000000000000b00338b0000010000705c", 0.997,
      17U, false, -1'000.0});
  writer.append_discontinuity(bh61::evidence::DiscontinuityEvent{
      "2026-08-22T14:03:00.002000Z", 100'765U, 42U, "source gap"});
  const auto artifacts = writer.finalize(
      "bh61-radio decode --input capture-001.sigmf-meta --jsonl");

  BH61_REQUIRE(std::filesystem::file_size(artifacts.data_path) ==
               samples.size() * sizeof(std::complex<float>));
  const auto metadata = read_text(artifacts.meta_path);
  BH61_REQUIRE(metadata.find("\"core:datatype\":\"cf32_le\"") !=
               std::string::npos);
  BH61_REQUIRE(metadata.find("\"core:sample_rate\":4000000") !=
               std::string::npos);
  BH61_REQUIRE(metadata.find("\"core:frequency\":915350000") !=
               std::string::npos);
  BH61_REQUIRE(metadata.find("\"bh61:backend\":\"file\"") !=
               std::string::npos);
  BH61_REQUIRE(metadata.find("\"bh61:monotonic_start_ns\":98765") !=
               std::string::npos);

  const auto events = read_text(artifacts.events_path);
  BH61_REQUIRE(events.find("\"kind\":\"decoded_frame\"") !=
               std::string::npos);
  BH61_REQUIRE(events.find("\"kind\":\"discontinuity\"") !=
               std::string::npos);
  BH61_REQUIRE(events.find("\"utc\":\"2026-08-22T14:03:00.001000Z\"") !=
               std::string::npos);
  BH61_REQUIRE(events.find("\"monotonic_ns\":99765") !=
               std::string::npos);

  const auto reproduction = read_text(artifacts.reproduction_path);
  BH61_REQUIRE(reproduction ==
               "bh61-radio decode --input capture-001.sigmf-meta --jsonl\n");
  const auto manifest = read_text(artifacts.manifest_path);
  BH61_REQUIRE(manifest.find(bh61::evidence::sha256_file(
                   artifacts.data_path)) != std::string::npos);
  BH61_REQUIRE(manifest.find(bh61::evidence::sha256_file(
                   artifacts.meta_path)) != std::string::npos);
  BH61_REQUIRE(manifest.find("capture-001.events.jsonl") != std::string::npos);

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

BH61_TEST("evidence writer appends blocks and records source discontinuities") {
  const auto directory = make_test_directory();
  bh61::evidence::Writer writer(directory, "streamed");
  writer.begin_capture(bh61::evidence::CaptureMetadata{
      4'000'000U, 915'350'000U, "hackrf", "serial=target",
      "2026-08-23T01:00:00.000000Z", 1'000U, false});
  constexpr std::array<std::complex<float>, 2> first{
      std::complex<float>{1.0F, 0.0F}, std::complex<float>{0.0F, 1.0F}};
  constexpr std::array<std::complex<float>, 2> second{
      std::complex<float>{-1.0F, 0.0F}, std::complex<float>{0.0F, -1.0F}};
  writer.append_samples(first, 0U, 1'000U, false, {});
  writer.append_samples(second, 5U, 2'250U, true, "queue_overflow");
  writer.append_discontinuity(bh61::evidence::DiscontinuityEvent{
      "2026-08-23T01:00:00.000001Z", 2'250U, 3U, "queue_overflow"});
  const auto artifacts = writer.finalize(
      "bh61-radio decode --input streamed.sigmf-meta --jsonl");

  BH61_REQUIRE(std::filesystem::file_size(artifacts.data_path) ==
               4U * sizeof(std::complex<float>));
  BH61_REQUIRE(!std::filesystem::exists(artifacts.partial_path));
  const auto metadata = read_text(artifacts.meta_path);
  BH61_REQUIRE(metadata.find("\"bh61:stored_sample_count\":4") !=
               std::string::npos);
  BH61_REQUIRE(metadata.find("\"bh61:source_sample_start\":0") !=
               std::string::npos);
  BH61_REQUIRE(metadata.find("\"bh61:source_sample_start\":5") !=
               std::string::npos);
  BH61_REQUIRE(metadata.find("\"core:sample_start\":2") !=
               std::string::npos);
  BH61_REQUIRE(metadata.find("\"bh61:discontinuity\":true") !=
               std::string::npos);
  BH61_REQUIRE(metadata.find("\"bh61:discontinuity_reason\":\"queue_overflow\"") !=
               std::string::npos);
  const auto events = read_text(artifacts.events_path);
  BH61_REQUIRE(events.find("\"kind\":\"discontinuity\"") !=
               std::string::npos);
  BH61_REQUIRE(read_text(artifacts.manifest_path)
                   .find("streamed.sigmf-data") != std::string::npos);

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

BH61_TEST("unfinalized evidence remains explicitly partial and recoverable") {
  const auto directory = make_test_directory();
  std::filesystem::path partial_path;
  {
    bh61::evidence::Writer writer(directory, "interrupted");
    writer.begin_capture(bh61::evidence::CaptureMetadata{
        4'000'000U, 915'350'000U, "hackrf", "serial=target",
        "2026-08-23T01:00:00.000000Z", 1'000U, false});
    constexpr std::array<std::complex<float>, 1> sample{
        std::complex<float>{0.25F, -0.25F}};
    writer.append_samples(sample, 0U, 1'000U, false, {});
    partial_path = writer.artifacts().partial_path;
    BH61_REQUIRE(std::filesystem::exists(partial_path));
  }
  BH61_REQUIRE(std::filesystem::exists(partial_path));
  BH61_REQUIRE(read_text(partial_path).find("\"complete\":false") !=
               std::string::npos);
  BH61_REQUIRE(read_text(partial_path).find("\"stored_sample_count\":1") !=
               std::string::npos);
  BH61_REQUIRE(!std::filesystem::exists(directory / "interrupted.sha256"));
  BH61_REQUIRE(!std::filesystem::exists(
      directory / "interrupted.sigmf-meta"));
  BH61_REQUIRE(std::filesystem::exists(
      directory / "interrupted.sigmf-data.partial"));

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}
