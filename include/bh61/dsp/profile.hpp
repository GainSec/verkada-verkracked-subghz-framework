#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace bh61::dsp::phy {

inline constexpr std::uint32_t bit_rate = 80'000;
inline constexpr std::uint32_t symbol_rate = 20'000;
inline constexpr std::uint32_t chip_rate = 640'000;
inline constexpr std::size_t preamble_symbols = 10;
inline constexpr std::array<std::uint8_t, 3> sync_symbols{0x00, 0x07, 0x0a};
inline constexpr std::array<std::uint8_t, 8> custom_oqpsk_taps{
    1, 1, 16, 48, 80, 112, 127, 127};

// The register coefficients are exact. Their absolute I/Q application remains
// a live-capture boundary and is not asserted by the analytical waveform model.
inline constexpr bool custom_tap_application_is_iq_verified = false;

auto fcc_frequency_hz(std::uint16_t channel) -> std::uint32_t;
auto burst_symbols(std::span<const std::uint8_t> frame_bytes)
    -> std::vector<std::uint8_t>;

}  // namespace bh61::dsp::phy
