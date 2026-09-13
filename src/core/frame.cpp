#include "bh61/core/frame.hpp"

#include "bh61/core/bytes.hpp"
#include "bh61/core/crc16.hpp"

#include <array>
#include <stdexcept>

namespace bh61::core {
namespace {

constexpr std::uint8_t minimum_phr = 5;
constexpr std::uint8_t maximum_phr = 127;

}  // namespace

auto parse_radio_frame(std::span<const std::uint8_t> bytes)
    -> RadioFrameResult {
  if (bytes.empty()) {
    return ParseError{ParseErrorCode::Truncated, "PHR", 0,
                      "missing physical header"};
  }

  const auto phr = bytes[0];
  if (phr < minimum_phr || phr > maximum_phr) {
    return ParseError{ParseErrorCode::InvalidLength, "PHR", 0,
                      "recovered profile permits PHR values 5 through 127"};
  }

  const auto expected_size = static_cast<std::size_t>(phr) + 1U;
  if (bytes.size() != expected_size) {
    return ParseError{ParseErrorCode::LengthMismatch, "PHR", 0,
                      "buffer size does not equal PHR plus one"};
  }

  const auto psdu_size = static_cast<std::size_t>(phr) - 2U;
  const auto psdu = bytes.subspan(1, psdu_size);
  const std::array<std::uint8_t, 2> fcs_bytes{
      bytes[1U + psdu_size], bytes[2U + psdu_size]};
  const auto received_fcs = load_be16(fcs_bytes);
  const auto computed_fcs = radio_fcs16(psdu);
  if (computed_fcs != received_fcs) {
    return ParseError{ParseErrorCode::BadFcs, "radio FCS", 1U + psdu_size,
                      "computed radio FCS does not match received bytes"};
  }

  return RadioFrame{phr, std::vector<std::uint8_t>(psdu.begin(), psdu.end()),
                    received_fcs, computed_fcs};
}

auto encode_radio_frame(const RadioFrame& frame) -> std::vector<std::uint8_t> {
  constexpr auto minimum_psdu = static_cast<std::size_t>(minimum_phr) - 2U;
  constexpr auto maximum_psdu = static_cast<std::size_t>(maximum_phr) - 2U;
  if (frame.psdu.size() < minimum_psdu || frame.psdu.size() > maximum_psdu) {
    throw std::invalid_argument(
        "recovered profile permits PSDU sizes 3 through 125 bytes");
  }

  std::vector<std::uint8_t> encoded;
  encoded.reserve(frame.psdu.size() + 3U);
  encoded.push_back(static_cast<std::uint8_t>(frame.psdu.size() + 2U));
  encoded.insert(encoded.end(), frame.psdu.begin(), frame.psdu.end());
  const auto fcs = store_be16(radio_fcs16(frame.psdu));
  encoded.insert(encoded.end(), fcs.begin(), fcs.end());
  return encoded;
}

auto encode_mac_ack_frame(std::uint8_t sequence) -> std::vector<std::uint8_t> {
  return encode_radio_frame(RadioFrame{0U, {0x02U, 0x00U, sequence}, 0U, 0U});
}

}  // namespace bh61::core
