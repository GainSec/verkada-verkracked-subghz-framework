#pragma once

#include "bh61/core/frame.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
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

}  // namespace bh61::core
