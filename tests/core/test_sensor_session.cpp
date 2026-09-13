#include "bh61/core/p256.hpp"
#include "bh61/core/sensor_session.hpp"
#include "test_harness.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

auto session_path(const char* label) -> std::filesystem::path {
  static std::uint64_t sequence = 0;
  const auto value = std::chrono::steady_clock::now().time_since_epoch().count();
  auto path = std::filesystem::temp_directory_path() /
              (std::string("bh61-sensor-session-") + label + "-" +
               std::to_string(value) + "-" + std::to_string(++sequence));
  std::filesystem::remove_all(path);
  return path;
}

auto identity_from(const bh61::core::P256KeyPair& keys)
    -> bh61::core::SensorIdentity {
  return bh61::core::SensorIdentity{
      "SYN-DOOR-0001",
      {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08},
      {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18},
      {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28},
      keys.public_xy};
}

auto join_response_for(const bh61::core::SensorIdentity& identity,
                       std::uint16_t local = 0x1234,
                       std::uint16_t remote = 0x5678)
    -> bh61::core::JoinResponse {
  bh61::core::JoinResponse response{};
  response.format = 5;
  response.local_public_xy = bh61::core::p256_public_from_private(
      std::array<std::uint8_t, 32>{
          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2});
  response.remote_public_hash = bh61::core::public_key_hash4(identity.public_xy);
  response.local_counter = local;
  response.remote_counter = remote;
  return response;
}

template <typename Function>
auto throws_logic(Function&& function) -> bool {
  try {
    function();
  } catch (const std::logic_error&) {
    return true;
  }
  return false;
}

template <typename Function>
auto throws_any(Function&& function) -> bool {
  try {
    function();
  } catch (const std::exception&) {
    return true;
  }
  return false;
}

}  // namespace

BH61_TEST("sensor session rejects serials longer than the coordinator field") {
  const auto path = session_path("serial-too-long");
  const auto keys = bh61::core::generate_p256_keypair();
  auto identity = identity_from(keys);
  identity.serial = std::string(33U, 'A');
  BH61_REQUIRE(throws_any([&] {
    static_cast<void>(bh61::core::SensorSession::create(
        path, identity, keys.private_scalar));
  }));
  std::filesystem::remove_all(path);
}

BH61_TEST("sensor session follows the complete allowed state path and resumes") {
  const auto path = session_path("linear");
  const auto keys = bh61::core::generate_p256_keypair();
  auto session = bh61::core::SensorSession::create(path, identity_from(keys),
                                                   keys.private_scalar);
  BH61_REQUIRE(session.state() == bh61::core::SensorSessionState::Created);
  session.mark_join_sent();
  const auto response = join_response_for(session.identity());
  session.accept_join_response(response);
  const auto shared = bh61::core::p256_shared_x(keys.private_scalar,
                                                response.local_public_xy);
  const auto key = bh61::core::derive_peer_key(shared);
  const auto fingerprint = bh61::core::session_key_fingerprint(key);
  session.install_key_fingerprint(fingerprint);
  session.mark_joined(bh61::core::JoinSignature{{0x30, 0x01, 0x00}});
  session.mark_event_sent(1);
  session.mark_acknowledged(bh61::core::SenseAckRequest{1}, 0x5678);
  session.complete_event();
  BH61_REQUIRE(session.state() == bh61::core::SensorSessionState::Joined);
  BH61_REQUIRE(session.send_counter() == 0x1235);
  BH61_REQUIRE(session.receive_counter() == 0x5679);

  const auto resumed = bh61::core::SensorSession::load(path);
  BH61_REQUIRE(resumed.state() == session.state());
  BH61_REQUIRE(resumed.identity() == session.identity());
  BH61_REQUIRE(resumed.key_fingerprint() == fingerprint);
  BH61_REQUIRE(resumed.private_scalar() == keys.private_scalar);
  std::filesystem::remove_all(path);
}

BH61_TEST("join counters are oriented from the coordinator peer record") {
  const auto path = session_path("counter-orientation");
  const auto keys = bh61::core::generate_p256_keypair();
  auto session = bh61::core::SensorSession::create(path, identity_from(keys),
                                                   keys.private_scalar);
  session.mark_join_sent();
  const auto response = join_response_for(session.identity(), 0x1234, 0x5678);
  session.accept_join_response(response);
  BH61_REQUIRE(session.send_counter() == 0x1234);
  BH61_REQUIRE(session.receive_counter() == 0x5678);
  std::filesystem::remove_all(path);
}

BH61_TEST("schema-one sessions migrate historical reversed counters") {
  const auto path = session_path("counter-migration");
  const auto keys = bh61::core::generate_p256_keypair();
  auto session = bh61::core::SensorSession::create(path, identity_from(keys),
                                                   keys.private_scalar);
  session.mark_join_sent();
  session.accept_join_response(
      join_response_for(session.identity(), 0x1234, 0x5678));

  std::ifstream input(path / "session.json", std::ios::binary);
  std::string legacy{std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>()};
  auto replace_once = [&](std::string_view from, std::string_view to) {
    const auto offset = legacy.find(from);
    BH61_REQUIRE(offset != std::string::npos);
    legacy.replace(offset, from.size(), to);
  };
  replace_once("\"schema_version\": 2", "\"schema_version\": 1");
  replace_once("\"send_counter\": 4660", "\"send_counter\": 22136");
  replace_once("\"receive_counter\": 22136", "\"receive_counter\": 4660");
  {
    std::ofstream output(path / "session.json", std::ios::binary | std::ios::trunc);
    output << legacy;
  }

  const auto migrated = bh61::core::SensorSession::load(path);
  BH61_REQUIRE(migrated.send_counter() == 0x1234);
  BH61_REQUIRE(migrated.receive_counter() == 0x5678);
  std::ifstream migrated_input(path / "session.json", std::ios::binary);
  const std::string migrated_text{std::istreambuf_iterator<char>(migrated_input),
                                  std::istreambuf_iterator<char>()};
  BH61_REQUIRE(migrated_text.find("\"schema_version\": 2") !=
               std::string::npos);
  std::filesystem::remove_all(path);
}

BH61_TEST("sensor session rejects every forbidden transition") {
  const auto path = session_path("forbidden");
  const auto keys = bh61::core::generate_p256_keypair();
  auto session = bh61::core::SensorSession::create(path, identity_from(keys),
                                                   keys.private_scalar);
  const auto response = join_response_for(session.identity());
  BH61_REQUIRE(throws_logic([&] { session.accept_join_response(response); }));
  BH61_REQUIRE(throws_logic([&] { session.install_key_fingerprint(std::string(64, '0')); }));
  BH61_REQUIRE(throws_logic([&] { session.mark_joined(bh61::core::JoinSignature{{1}}); }));
  BH61_REQUIRE(throws_logic([&] { session.mark_event_sent(1); }));
  BH61_REQUIRE(throws_logic([&] { session.mark_acknowledged({1}, 0); }));
  BH61_REQUIRE(throws_logic([&] { session.complete_event(); }));
  session.mark_join_sent();
  session.mark_join_sent();
  session.accept_join_response(response);
  BH61_REQUIRE(throws_logic([&] { session.mark_join_sent(); }));
  std::filesystem::remove_all(path);
}

BH61_TEST("sensor session accepts only an exact duplicate join response") {
  const auto path = session_path("duplicate");
  const auto keys = bh61::core::generate_p256_keypair();
  auto session = bh61::core::SensorSession::create(path, identity_from(keys),
                                                   keys.private_scalar);
  session.mark_join_sent();
  auto response = join_response_for(session.identity());
  session.accept_join_response(response);
  session.accept_join_response(response);
  ++response.local_counter;
  BH61_REQUIRE(throws_logic([&] { session.accept_join_response(response); }));
  std::filesystem::remove_all(path);
}

BH61_TEST("sensor session counters wrap at sixteen bits") {
  const auto path = session_path("wrap");
  const auto keys = bh61::core::generate_p256_keypair();
  auto session = bh61::core::SensorSession::create(path, identity_from(keys),
                                                   keys.private_scalar);
  session.mark_join_sent();
  const auto response = join_response_for(session.identity(), 0xffff, 0xffff);
  session.accept_join_response(response);
  const auto key = bh61::core::derive_peer_key(
      bh61::core::p256_shared_x(keys.private_scalar, response.local_public_xy));
  session.install_key_fingerprint(bh61::core::session_key_fingerprint(key));
  session.mark_joined({{0x30}});
  session.mark_event_sent(7);
  session.mark_acknowledged({7}, 0xffff);
  BH61_REQUIRE(session.send_counter() == 0U);
  BH61_REQUIRE(session.receive_counter() == 0U);
  std::filesystem::remove_all(path);
}

BH61_TEST("sensor session advances every authenticated downlink counter") {
  const auto path = session_path("downlink-counters");
  const auto keys = bh61::core::generate_p256_keypair();
  auto session = bh61::core::SensorSession::create(path, identity_from(keys),
                                                   keys.private_scalar);
  session.mark_join_sent();
  const auto response = join_response_for(session.identity(), 0x1234, 0x5678);
  session.accept_join_response(response);
  const auto key = bh61::core::derive_peer_key(
      bh61::core::p256_shared_x(keys.private_scalar, response.local_public_xy));
  session.install_key_fingerprint(bh61::core::session_key_fingerprint(key));
  session.mark_joined({{0x30}});
  session.mark_event_sent(9);
  session.accept_received_counter(0x5678);
  session.accept_received_counter(0x5679);
  BH61_REQUIRE(session.receive_counter() == 0x567a);
  BH61_REQUIRE(throws_any([&] { session.accept_received_counter(0x5679); }));
  BH61_REQUIRE(bh61::core::SensorSession::load(path).receive_counter() ==
               0x567a);
  std::filesystem::remove_all(path);
}

BH61_TEST("sensor session refuses overwrite and keeps private material separate") {
  const auto path = session_path("private");
  const auto keys = bh61::core::generate_p256_keypair();
  const auto identity = identity_from(keys);
  auto session = bh61::core::SensorSession::create(path, identity,
                                                   keys.private_scalar);
  BH61_REQUIRE(throws_any([&] {
    (void)bh61::core::SensorSession::create(path, identity, keys.private_scalar);
  }));
  std::ifstream state(path / "session.json", std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(state)),
                         std::istreambuf_iterator<char>());
  BH61_REQUIRE(text.find("private") == std::string::npos);
  const auto permissions = std::filesystem::status(path / "sensor-private.key").permissions();
  using P = std::filesystem::perms;
  BH61_REQUIRE((permissions & (P::group_all | P::others_all)) == P::none);
  std::filesystem::remove_all(path);
}

BH61_TEST("sensor session rejects corrupt truncated and identity-mismatched state") {
  const auto keys = bh61::core::generate_p256_keypair();
  const auto identity = identity_from(keys);
  const auto corrupt = session_path("corrupt");
  (void)bh61::core::SensorSession::create(corrupt, identity, keys.private_scalar);
  {
    std::ofstream output(corrupt / "session.json", std::ios::trunc);
    output << "{\"schema_version\":";
  }
  BH61_REQUIRE(throws_any([&] { (void)bh61::core::SensorSession::load(corrupt); }));
  std::filesystem::remove_all(corrupt);

  const auto mismatch = session_path("mismatch");
  (void)bh61::core::SensorSession::create(mismatch, identity, keys.private_scalar);
  const auto other = bh61::core::generate_p256_keypair();
  {
    std::ofstream output(mismatch / "sensor-private.key", std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(other.private_scalar.data()),
                 static_cast<std::streamsize>(other.private_scalar.size()));
  }
  std::filesystem::permissions(mismatch / "sensor-private.key",
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);
  BH61_REQUIRE(throws_any([&] { (void)bh61::core::SensorSession::load(mismatch); }));
  std::filesystem::remove_all(mismatch);
}
