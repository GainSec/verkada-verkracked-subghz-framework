#pragma once

#include "bh61/core/frame.hpp"
#include "bh61/core/vcmp.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace bh61::core {

using Aes128Key = std::array<std::uint8_t, 16>;
using AesBlock = std::array<std::uint8_t, 16>;

auto aes128_cbc_encrypt(std::span<const std::uint8_t> plaintext,
                        const Aes128Key& key, const AesBlock& iv)
    -> std::vector<std::uint8_t>;
auto aes128_cbc_decrypt(std::span<const std::uint8_t> ciphertext,
                        const Aes128Key& key, const AesBlock& iv)
    -> std::vector<std::uint8_t>;

struct OpenedVcmp {
  std::uint16_t counter{};
  std::vector<std::uint8_t> payload;
};

using OpenVcmpResult = std::variant<OpenedVcmp, ParseError>;

auto open_vcmp(const VcmpFrame& frame, const Aes128Key& key) -> OpenVcmpResult;
auto seal_vcmp(std::uint8_t type, std::uint8_t flags, std::uint16_t counter,
               std::span<const std::uint8_t> payload, const Aes128Key& key,
               const AesBlock& iv) -> VcmpFrame;

auto counter_is_acceptable(std::uint16_t expected,
                           std::uint16_t received) noexcept -> bool;
auto next_counter(std::uint16_t counter) noexcept -> std::uint16_t;

}  // namespace bh61::core
