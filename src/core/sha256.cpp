#include "bh61/core/sha256.hpp"

#include <openssl/evp.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace bh61::core {

auto sha256_hex(std::span<const std::uint8_t> bytes) -> std::string {
  std::array<std::uint8_t, 32> digest{};
  unsigned int digest_length = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &digest_length,
                 EVP_sha256(), nullptr) != 1 || digest_length != digest.size()) {
    throw std::runtime_error("OpenSSL SHA-256 failed");
  }
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
}

}  // namespace bh61::core
