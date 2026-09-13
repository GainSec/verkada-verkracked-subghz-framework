#pragma once

#include "bh61/core/session.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace bh61::core {

struct P256KeyPair {
  std::array<std::uint8_t, 32> private_scalar;
  std::array<std::uint8_t, 64> public_xy;
};

auto generate_p256_keypair() -> P256KeyPair;
auto p256_public_from_private(std::span<const std::uint8_t, 32> private_scalar)
    -> std::array<std::uint8_t, 64>;
auto p256_shared_x(std::span<const std::uint8_t, 32> private_scalar,
                   std::span<const std::uint8_t, 64> remote_public_xy)
    -> std::array<std::uint8_t, 32>;
auto derive_peer_key(std::span<const std::uint8_t, 32> shared_x) -> Aes128Key;

}  // namespace bh61::core
