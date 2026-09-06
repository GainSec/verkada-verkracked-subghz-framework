#include "bh61/core/messages.hpp"

#include "bh61/core/bytes.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace bh61::core {

auto parse_scan_info(std::span<const std::uint8_t> body) -> ScanInfoResult {
  if (body.size() != 5U) {
    return ParseError{ParseErrorCode::InvalidField, "scan-info", body.size(),
                      "scan-info body must be exactly five bytes"};
  }
  const std::array<std::uint8_t, 2> channel_bytes{body[0], body[1]};
  if (body[2] < 1U || body[2] > 4U) {
    return ParseError{ParseErrorCode::InvalidField, "scan-info", 2,
                      "regulatory code must be in the recovered range 1..4"};
  }
  return ScanInfo{load_be16(channel_bytes), body[2], body[3], body[4]};
}

auto encode_scan_info(const ScanInfo& scan) -> std::vector<std::uint8_t> {
  if (scan.regulatory_code < 1U || scan.regulatory_code > 4U) {
    throw std::invalid_argument("regulatory code must be in range 1..4");
  }
  const auto channel = store_be16(scan.channel);
  return {channel[0], channel[1], scan.regulatory_code, scan.reserved,
          scan.sequence};
}

auto parse_counter_control(std::span<const std::uint8_t> body)
    -> CounterControlResult {
  if (body.empty()) {
    return ParseError{ParseErrorCode::Truncated, "counter-control", 0,
                      "counter-control subtype is missing"};
  }
  switch (body[0]) {
    case 0:
      if (body.size() != 2U) {
        break;
      }
      return CounterControl{CounterControlSubtype::UnsupportedType, body[1]};
    case 1: {
      if (body.size() != 3U) {
        break;
      }
      const std::array<std::uint8_t, 2> value{body[1], body[2]};
      return CounterControl{CounterControlSubtype::ExpectedCounter,
                            load_be16(value)};
    }
    case 2:
      if (body.size() != 1U) {
        break;
      }
      return CounterControl{CounterControlSubtype::MissingKey, 0};
    default:
      return ParseError{ParseErrorCode::InvalidField, "counter-control", 0,
                        "unknown counter-control subtype"};
  }
  return ParseError{ParseErrorCode::InvalidField, "counter-control", body.size(),
                    "body length does not match counter-control subtype"};
}

auto encode_counter_control(const CounterControl& control)
    -> std::vector<std::uint8_t> {
  switch (control.subtype) {
    case CounterControlSubtype::UnsupportedType:
      return {0, static_cast<std::uint8_t>(control.value)};
    case CounterControlSubtype::ExpectedCounter: {
      const auto value = store_be16(control.value);
      return {1, value[0], value[1]};
    }
    case CounterControlSubtype::MissingKey:
      return {2};
  }
  throw std::invalid_argument("unknown counter-control subtype");
}

auto parse_rpc(std::span<const std::uint8_t> bytes) -> RpcResult {
  if (bytes.size() < 4U) {
    return ParseError{ParseErrorCode::Truncated, "RPC", bytes.size(),
                      "four-byte RPC header is truncated"};
  }
  const auto declared = static_cast<std::size_t>(bytes[2]);
  if (declared > bytes.size() - 4U) {
    return ParseError{ParseErrorCode::InvalidField, "RPC", 2,
                      "declared payload exceeds decrypted bytes"};
  }
  RpcMessage rpc{bytes[0], bytes[1], bytes[2], bytes[3], {}, {}};
  rpc.payload.assign(bytes.begin() + 4,
                     bytes.begin() + static_cast<std::ptrdiff_t>(4U + declared));
  rpc.trailing.assign(bytes.begin() + static_cast<std::ptrdiff_t>(4U + declared),
                      bytes.end());
  return rpc;
}

auto encode_rpc(const RpcMessage& rpc) -> std::vector<std::uint8_t> {
  if (rpc.payload.size() != rpc.declared_length) {
    throw std::invalid_argument("RPC payload does not match declared length");
  }
  std::vector<std::uint8_t> encoded{rpc.message_id, rpc.transaction,
                                    rpc.declared_length, rpc.flags};
  encoded.insert(encoded.end(), rpc.payload.begin(), rpc.payload.end());
  encoded.insert(encoded.end(), rpc.trailing.begin(), rpc.trailing.end());
  return encoded;
}

namespace {

auto parse_fixed_join(std::span<const std::uint8_t> body) -> JoinResult {
  if (body.size() < 25U) {
    return ParseError{ParseErrorCode::Truncated, "join-v4/v5", body.size(),
                      "fixed join body is truncated"};
  }
  const auto serial_length = static_cast<std::size_t>(body[14]);
  const auto expected_size = 25U + serial_length;
  if (serial_length == 0U || body.size() != expected_size) {
    return ParseError{ParseErrorCode::InvalidField, "join-v4/v5", 14,
                      "serial length is empty or inconsistent"};
  }

  JoinRequest join;
  join.version = body[0];
  join.bsp = body[1];
  const std::array<std::uint8_t, 4> firmware{body[2], body[3], body[4], body[5]};
  join.firmware_version = load_be32(firmware);
  std::copy_n(body.begin() + 6, join.physical_device_id.size(),
              join.physical_device_id.begin());
  join.serial.assign(body.begin() + 15,
                     body.begin() + static_cast<std::ptrdiff_t>(15U + serial_length));
  std::array<std::uint8_t, 8> nonce{};
  std::copy_n(body.begin() + static_cast<std::ptrdiff_t>(15U + serial_length),
              nonce.size(), nonce.begin());
  join.nonce = nonce;
  const auto pair_offset = 23U + serial_length;
  join.legacy_reg_pair =
      std::array<std::uint8_t, 2>{body[pair_offset], body[pair_offset + 1U]};
  join.regulatory_code = body[pair_offset + 1U];
  return join;
}

auto parse_v6_join(std::span<const std::uint8_t> body) -> JoinResult {
  if (body.size() < 14U) {
    return ParseError{ParseErrorCode::Truncated, "join-v6", body.size(),
                      "version-6 fixed prefix is truncated"};
  }
  JoinRequest join;
  join.version = 6;
  join.bsp = body[1];
  const std::array<std::uint8_t, 4> firmware{body[2], body[3], body[4], body[5]};
  join.firmware_version = load_be32(firmware);
  std::copy_n(body.begin() + 6, join.physical_device_id.size(),
              join.physical_device_id.begin());

  std::size_t offset = 14;
  while (offset < body.size()) {
    if (body.size() - offset < 2U) {
      return ParseError{ParseErrorCode::Truncated, "join-v6 TLV", offset,
                        "TLV header is truncated"};
    }
    const auto tag = body[offset];
    const auto length = static_cast<std::size_t>(body[offset + 1U]);
    offset += 2U;
    if (length > body.size() - offset) {
      return ParseError{ParseErrorCode::Truncated, "join-v6 TLV", offset,
                        "TLV value is truncated"};
    }
    JoinTlv tlv{tag, std::vector<std::uint8_t>(
                         body.begin() + static_cast<std::ptrdiff_t>(offset),
                         body.begin() + static_cast<std::ptrdiff_t>(offset + length))};
    if (tag == 0U) {
      join.serial = tlv.value;
    } else if (tag == 1U && length >= 8U) {
      std::array<std::uint8_t, 8> nonce{};
      std::copy_n(tlv.value.begin(), nonce.size(), nonce.begin());
      join.nonce = nonce;
    } else if (tag == 3U && length >= 1U) {
      join.regulatory_code = tlv.value[0];
    }
    join.tlvs.push_back(std::move(tlv));
    offset += length;
  }
  if (join.serial.empty()) {
    return ParseError{ParseErrorCode::InvalidField, "join-v6", 14,
                      "nonempty serial TLV is required"};
  }
  return join;
}

}  // namespace

auto parse_join_request(std::span<const std::uint8_t> body) -> JoinResult {
  if (body.empty()) {
    return ParseError{ParseErrorCode::Truncated, "join", 0,
                      "join version is missing"};
  }
  if (body[0] == 4U || body[0] == 5U) {
    return parse_fixed_join(body);
  }
  if (body[0] == 6U) {
    return parse_v6_join(body);
  }
  return ParseError{ParseErrorCode::UnsupportedVersion, "join", 0,
                    "supported join versions are 4, 5, and 6"};
}

auto encode_join_request(const JoinRequest& join) -> std::vector<std::uint8_t> {
  if (join.version == 4U || join.version == 5U) {
    if (join.serial.empty() ||
        join.serial.size() >
            static_cast<std::size_t>(std::numeric_limits<std::uint8_t>::max()) ||
        !join.nonce || !join.legacy_reg_pair) {
      throw std::invalid_argument("incomplete fixed-layout join request");
    }
    std::vector<std::uint8_t> encoded{join.version, join.bsp};
    const auto firmware = store_be32(join.firmware_version);
    encoded.insert(encoded.end(), firmware.begin(), firmware.end());
    encoded.insert(encoded.end(), join.physical_device_id.begin(),
                   join.physical_device_id.end());
    encoded.push_back(static_cast<std::uint8_t>(join.serial.size()));
    encoded.insert(encoded.end(), join.serial.begin(), join.serial.end());
    encoded.insert(encoded.end(), join.nonce->begin(), join.nonce->end());
    encoded.insert(encoded.end(), join.legacy_reg_pair->begin(),
                   join.legacy_reg_pair->end());
    return encoded;
  }
  if (join.version == 6U) {
    std::vector<std::uint8_t> encoded{join.version, join.bsp};
    const auto firmware = store_be32(join.firmware_version);
    encoded.insert(encoded.end(), firmware.begin(), firmware.end());
    encoded.insert(encoded.end(), join.physical_device_id.begin(),
                   join.physical_device_id.end());
    for (const auto& tlv : join.tlvs) {
      if (tlv.value.size() >
          static_cast<std::size_t>(std::numeric_limits<std::uint8_t>::max())) {
        throw std::invalid_argument("join TLV exceeds one-byte length");
      }
      encoded.push_back(tlv.tag);
      encoded.push_back(static_cast<std::uint8_t>(tlv.value.size()));
      encoded.insert(encoded.end(), tlv.value.begin(), tlv.value.end());
    }
    return encoded;
  }
  throw std::invalid_argument("supported join versions are 4, 5, and 6");
}

}  // namespace bh61::core
