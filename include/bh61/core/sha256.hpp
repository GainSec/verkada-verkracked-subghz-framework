#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace bh61::core {

auto sha256_hex(std::span<const std::uint8_t> bytes) -> std::string;

}  // namespace bh61::core
