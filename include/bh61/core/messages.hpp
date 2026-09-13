#pragma once

#include "bh61/core/frame.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace bh61::core {

struct ScanInfo {
  std::uint16_t channel{};
  std::uint8_t regulatory_code{};
  std::uint8_t reserved{};
  std::uint8_t sequence{};
};

using ScanInfoResult = std::variant<ScanInfo, ParseError>;
auto parse_scan_info(std::span<const std::uint8_t> body) -> ScanInfoResult;
auto encode_scan_info(const ScanInfo& scan) -> std::vector<std::uint8_t>;

enum class CounterControlSubtype : std::uint8_t {
  UnsupportedType = 0,
  ExpectedCounter = 1,
  MissingKey = 2,
};

struct CounterControl {
  CounterControlSubtype subtype{};
  std::uint16_t value{};
};

using CounterControlResult = std::variant<CounterControl, ParseError>;
auto parse_counter_control(std::span<const std::uint8_t> body)
    -> CounterControlResult;
auto encode_counter_control(const CounterControl& control)
    -> std::vector<std::uint8_t>;

struct RpcMessage {
  std::uint8_t message_id{};
  std::uint8_t transaction{};
  std::uint8_t declared_length{};
  std::uint8_t flags{};
  std::vector<std::uint8_t> payload;
  std::vector<std::uint8_t> trailing;
};

using RpcResult = std::variant<RpcMessage, ParseError>;
auto parse_rpc(std::span<const std::uint8_t> bytes) -> RpcResult;
auto encode_rpc(const RpcMessage& rpc) -> std::vector<std::uint8_t>;

struct AckResponse {
  std::uint32_t status{};
  std::vector<std::uint8_t> message;
};

using AckResponseResult = std::variant<AckResponse, ParseError>;
auto parse_ack_response(std::span<const std::uint8_t> body)
    -> AckResponseResult;
auto encode_ack_response(const AckResponse& response)
    -> std::vector<std::uint8_t>;

struct JoinTlv {
  std::uint8_t tag{};
  std::vector<std::uint8_t> value;
};

struct JoinRequest {
  std::uint8_t version{};
  std::uint8_t bsp{};
  std::uint32_t firmware_version{};
  std::array<std::uint8_t, 8> physical_device_id{};
  std::vector<std::uint8_t> serial;
  std::optional<std::array<std::uint8_t, 8>> nonce;
  std::optional<std::uint8_t> regulatory_code;
  std::optional<std::array<std::uint8_t, 2>> legacy_reg_pair;
  std::vector<JoinTlv> tlvs;
};

using JoinResult = std::variant<JoinRequest, ParseError>;
auto parse_join_request(std::span<const std::uint8_t> body) -> JoinResult;
auto encode_join_request(const JoinRequest& join) -> std::vector<std::uint8_t>;

struct JoinResponse {
  std::uint8_t format{};
  std::array<std::uint8_t, 64> local_public_xy{};
  std::array<std::uint8_t, 16> reserved{};
  std::array<std::uint8_t, 4> remote_public_hash{};
  std::uint16_t local_counter{};
  std::uint16_t remote_counter{};

  auto operator==(const JoinResponse&) const -> bool = default;
};

using JoinResponseResult = std::variant<JoinResponse, ParseError>;
auto parse_join_response(std::span<const std::uint8_t> body)
    -> JoinResponseResult;
auto encode_join_response(const JoinResponse& response)
    -> std::vector<std::uint8_t>;

struct JoinSignature {
  std::vector<std::uint8_t> signature;
};

using JoinSignatureResult = std::variant<JoinSignature, ParseError>;
auto parse_join_signature(std::span<const std::uint8_t> body)
    -> JoinSignatureResult;
auto encode_join_signature(const JoinSignature& signature)
    -> std::vector<std::uint8_t>;

struct EventTlv {
  std::uint8_t type{};
  std::vector<std::uint8_t> value;

  auto operator==(const EventTlv&) const -> bool = default;
};

struct EventRequest {
  std::uint8_t type{};
  bool active{};
  std::uint32_t age_ms{};
  std::vector<EventTlv> tlvs;
};

struct EventSemantic {
  std::uint8_t type{};
  bool active{};

  auto operator==(const EventSemantic&) const -> bool = default;
};

auto event_semantic(std::string_view name) -> std::optional<EventSemantic>;

using EventRequestResult = std::variant<EventRequest, ParseError>;
auto parse_event_request(std::span<const std::uint8_t> body)
    -> EventRequestResult;
auto encode_event_request(const EventRequest& event)
    -> std::vector<std::uint8_t>;

struct SenseAckRequest {
  std::uint32_t event_id{};
};

using SenseAckRequestResult = std::variant<SenseAckRequest, ParseError>;
auto parse_sense_ack_request(std::span<const std::uint8_t> body)
    -> SenseAckRequestResult;
auto encode_sense_ack_request(const SenseAckRequest& acknowledgement)
    -> std::vector<std::uint8_t>;

}  // namespace bh61::core
