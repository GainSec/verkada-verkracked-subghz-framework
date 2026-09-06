#include "bh61/dsp/profile.hpp"

#include "bh61/dsp/dsss.hpp"

#include <stdexcept>

namespace bh61::dsp::phy {

auto fcc_frequency_hz(std::uint16_t channel) -> std::uint32_t {
  if (channel > 20U) {
    throw std::invalid_argument("FCC channel must be in range 0..20");
  }
  return 915'000'000U + static_cast<std::uint32_t>(channel) * 35'000U;
}

auto burst_symbols(std::span<const std::uint8_t> frame_bytes)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> symbols(preamble_symbols, 0);
  symbols.insert(symbols.end(), sync_symbols.begin(), sync_symbols.end());
  const auto frame_symbols = bytes_to_symbols(frame_bytes);
  symbols.insert(symbols.end(), frame_symbols.begin(), frame_symbols.end());
  return symbols;
}

}  // namespace bh61::dsp::phy
