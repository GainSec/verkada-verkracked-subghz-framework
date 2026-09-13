#pragma once

#include "bh61/core/messages.hpp"
#include "bh61/core/p256.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace bh61::core {

enum class SensorSessionState {
  Created,
  JoinSent,
  ResponseReceived,
  KeyDerived,
  Joined,
  EventSent,
  Acknowledged,
};

struct SensorIdentity {
  std::string serial;
  std::array<std::uint8_t, 8> physical_device_id{};
  std::array<std::uint8_t, 8> eui64{};
  std::array<std::uint8_t, 8> nonce{};
  std::array<std::uint8_t, 64> public_xy{};

  auto operator==(const SensorIdentity&) const -> bool = default;
};

auto public_key_hash4(std::span<const std::uint8_t, 64> public_xy)
    -> std::array<std::uint8_t, 4>;
auto session_key_fingerprint(const Aes128Key& key) -> std::string;
auto sensor_session_state_name(SensorSessionState state) -> std::string_view;

class SensorSession {
 public:
  SensorSession(const SensorSession&) = delete;
  auto operator=(const SensorSession&) -> SensorSession& = delete;
  SensorSession(SensorSession&& other) noexcept;
  auto operator=(SensorSession&& other) noexcept -> SensorSession&;
  ~SensorSession();

  static auto create(const std::filesystem::path& output_directory,
                     SensorIdentity identity,
                     std::array<std::uint8_t, 32> private_scalar)
      -> SensorSession;
  static auto load(const std::filesystem::path& output_directory)
      -> SensorSession;

  auto state() const noexcept -> SensorSessionState;
  auto identity() const noexcept -> const SensorIdentity&;
  auto private_scalar() const noexcept
      -> const std::array<std::uint8_t, 32>&;
  auto remote_public_xy() const
      -> const std::array<std::uint8_t, 64>&;
  auto key_fingerprint() const noexcept -> const std::string&;
  auto send_counter() const noexcept -> std::uint16_t;
  auto receive_counter() const noexcept -> std::uint16_t;

  void mark_join_sent();
  void accept_join_response(const JoinResponse& response);
  void install_key_fingerprint(std::string fingerprint);
  void mark_joined(const JoinSignature& signature);
  void mark_protected_request_sent();
  void mark_event_sent(std::uint32_t event_id);
  void accept_received_counter(std::uint16_t received_counter);
  void mark_acknowledged(const SenseAckRequest& acknowledgement,
                         std::uint16_t received_counter);
  void complete_event();

 private:
  SensorSession(std::filesystem::path output_directory,
                SensorIdentity identity,
                std::array<std::uint8_t, 32> private_scalar);
  void persist() const;

  std::filesystem::path output_directory_;
  SensorIdentity identity_;
  std::array<std::uint8_t, 32> private_scalar_{};
  SensorSessionState state_{SensorSessionState::Created};
  bool remote_public_present_{};
  std::array<std::uint8_t, 64> remote_public_xy_{};
  std::string key_fingerprint_;
  std::uint16_t send_counter_{};
  std::uint16_t receive_counter_{};
  std::uint8_t join_format_{};
  std::optional<std::uint32_t> pending_event_id_;
};

}  // namespace bh61::core
