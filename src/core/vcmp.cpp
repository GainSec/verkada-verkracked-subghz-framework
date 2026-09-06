#include "bh61/core/vcmp.hpp"

#include "bh61/core/bytes.hpp"
#include "bh61/core/crc16.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace bh61::core {
namespace {

constexpr std::uint8_t encrypted_flag = 0x01;

auto crc_input(std::uint8_t type, std::uint8_t flags,
               std::span<const std::uint8_t> plaintext)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> bytes{type, flags, 0x00, 0x00};
  bytes.insert(bytes.end(), plaintext.begin(), plaintext.end());
  return bytes;
}

constexpr auto padded_size(std::uint8_t plaintext_length) -> std::size_t {
  return ((static_cast<std::size_t>(plaintext_length) + 15U) / 16U) * 16U;
}

}  // namespace

auto parse_vcmp(std::span<const std::uint8_t> bytes) -> VcmpResult {
  if (bytes.size() < 4U) {
    return ParseError{ParseErrorCode::Truncated, "VCMP", bytes.size(),
                      "four-byte VCMP header is truncated"};
  }

  const std::array<std::uint8_t, 2> crc_bytes{bytes[2], bytes[3]};
  VcmpFrame frame;
  frame.type = bytes[0];
  frame.flags = bytes[1];
  frame.received_crc = load_be16(crc_bytes);

  if ((frame.flags & encrypted_flag) == 0) {
    VcmpPlaintext plaintext;
    plaintext.bytes.assign(bytes.begin() + 4, bytes.end());
    frame.computed_crc =
        vcmp_crc16(crc_input(frame.type, frame.flags, plaintext.bytes));
    if (*frame.computed_crc != frame.received_crc) {
      return ParseError{ParseErrorCode::BadChecksum, "VCMP CRC", 2,
                        "plaintext VCMP CRC does not match"};
    }
    frame.body = std::move(plaintext);
    return frame;
  }

  constexpr std::size_t encrypted_prefix_size = 4U + 16U + 1U;
  if (bytes.size() < encrypted_prefix_size + 16U) {
    return ParseError{ParseErrorCode::InvalidEncoding, "VCMP encrypted", 4,
                      "encrypted body lacks a complete AES block"};
  }

  VcmpEncrypted encrypted;
  std::copy_n(bytes.begin() + 4, encrypted.iv.size(), encrypted.iv.begin());
  encrypted.plaintext_length = bytes[20];
  encrypted.ciphertext.assign(bytes.begin() + 21, bytes.end());
  if (encrypted.plaintext_length < 2U ||
      encrypted.ciphertext.size() % 16U != 0U ||
      padded_size(encrypted.plaintext_length) != encrypted.ciphertext.size()) {
    return ParseError{ParseErrorCode::InvalidEncoding, "VCMP encrypted", 20,
                      "plaintext length and CBC block count are inconsistent"};
  }

  frame.body = std::move(encrypted);
  return frame;
}

auto encode_vcmp(const VcmpFrame& frame) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> encoded{frame.type, frame.flags, 0x00, 0x00};
  if (const auto* plaintext = std::get_if<VcmpPlaintext>(&frame.body)) {
    if ((frame.flags & encrypted_flag) != 0) {
      throw std::invalid_argument("encrypted flag set on plaintext VCMP body");
    }
    const auto checksum =
        store_be16(vcmp_crc16(crc_input(frame.type, frame.flags,
                                        plaintext->bytes)));
    encoded[2] = checksum[0];
    encoded[3] = checksum[1];
    encoded.insert(encoded.end(), plaintext->bytes.begin(),
                   plaintext->bytes.end());
    return encoded;
  }

  if ((frame.flags & encrypted_flag) == 0) {
    throw std::invalid_argument("encrypted VCMP body lacks encrypted flag");
  }
  const auto& encrypted = std::get<VcmpEncrypted>(frame.body);
  if (encrypted.plaintext_length < 2U ||
      encrypted.ciphertext.size() % 16U != 0U ||
      padded_size(encrypted.plaintext_length) != encrypted.ciphertext.size()) {
    throw std::invalid_argument(
        "plaintext length and CBC block count are inconsistent");
  }
  const auto checksum = store_be16(frame.received_crc);
  encoded[2] = checksum[0];
  encoded[3] = checksum[1];
  encoded.insert(encoded.end(), encrypted.iv.begin(), encrypted.iv.end());
  encoded.push_back(encrypted.plaintext_length);
  encoded.insert(encoded.end(), encrypted.ciphertext.begin(),
                 encrypted.ciphertext.end());
  return encoded;
}

}  // namespace bh61::core
