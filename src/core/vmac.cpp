#include "bh61/core/vmac.hpp"

#include "bh61/core/bytes.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace bh61::core {
namespace {

constexpr std::size_t short_header_size = 15;
constexpr std::size_t extended_header_size = 21;
constexpr std::uint16_t fixed_fcf_mask = 0xf047;
constexpr std::uint16_t fixed_fcf_value = 0xc041;
constexpr std::uint16_t clear_fcf_mask = 0x0198;
constexpr std::uint16_t destination_mode_mask = 0x0c00;
constexpr std::uint16_t destination_short_mode = 0x0800;
constexpr std::uint16_t destination_extended_mode = 0x0c00;

constexpr auto supported_fcf(std::uint16_t frame_control) -> bool {
  return (frame_control & fixed_fcf_mask) == fixed_fcf_value &&
         (frame_control & clear_fcf_mask) == 0 &&
         (frame_control & destination_short_mode) != 0;
}

}  // namespace

auto parse_vmac(std::span<const std::uint8_t> psdu) -> VmacResult {
  if (psdu.size() < 5U) {
    return ParseError{ParseErrorCode::Truncated, "VMAC", psdu.size(),
                      "frame control, sequence, or destination PAN is truncated"};
  }

  const std::array<std::uint8_t, 2> fcf_bytes{psdu[0], psdu[1]};
  const auto frame_control = load_le16(fcf_bytes);
  if (!supported_fcf(frame_control)) {
    return ParseError{ParseErrorCode::UnsupportedAddressing, "VMAC", 0,
                      "frame control does not satisfy the recovered firmware masks"};
  }

  const auto destination_mode = frame_control & destination_mode_mask;
  const auto header_size = destination_mode == destination_short_mode
                               ? short_header_size
                               : extended_header_size;
  if (psdu.size() < header_size) {
    return ParseError{ParseErrorCode::Truncated, "VMAC", psdu.size(),
                      "destination/source address fields are truncated"};
  }

  const std::array<std::uint8_t, 2> pan_bytes{psdu[3], psdu[4]};
  VmacFrame frame;
  frame.frame_control = frame_control;
  frame.sequence = psdu[2];
  frame.destination_pan = load_le16(pan_bytes);

  std::size_t source_offset = 0;
  if (destination_mode == destination_short_mode) {
    const std::array<std::uint8_t, 2> destination_bytes{psdu[5], psdu[6]};
    const auto destination = load_le16(destination_bytes);
    if (destination > 2U) {
      return ParseError{ParseErrorCode::InvalidDestination, "VMAC", 5,
                        "short destination must be address 0, 1, or 2"};
    }
    frame.destination = destination;
    source_offset = 7;
  } else if (destination_mode == destination_extended_mode) {
    std::array<std::uint8_t, 8> destination{};
    std::copy_n(psdu.begin() + 5, destination.size(), destination.begin());
    frame.destination = destination;
    source_offset = 13;
  } else {
    return ParseError{ParseErrorCode::UnsupportedAddressing, "VMAC", 0,
                      "destination mode is neither short nor extended"};
  }

  std::copy_n(psdu.begin() + static_cast<std::ptrdiff_t>(source_offset),
              frame.source_eui.size(), frame.source_eui.begin());
  frame.payload.assign(psdu.begin() + static_cast<std::ptrdiff_t>(header_size),
                       psdu.end());
  return frame;
}

auto encode_vmac(const VmacFrame& frame) -> std::vector<std::uint8_t> {
  if (!supported_fcf(frame.frame_control)) {
    throw std::invalid_argument(
        "frame control does not satisfy the recovered firmware masks");
  }

  const auto destination_mode = frame.frame_control & destination_mode_mask;
  const bool short_destination =
      std::holds_alternative<std::uint16_t>(frame.destination);
  if ((short_destination && destination_mode != destination_short_mode) ||
      (!short_destination && destination_mode != destination_extended_mode)) {
    throw std::invalid_argument(
        "frame-control destination mode does not match destination value");
  }

  const auto header_size = short_destination ? short_header_size
                                             : extended_header_size;
  std::vector<std::uint8_t> encoded;
  encoded.reserve(header_size + frame.payload.size());
  const auto fcf = store_le16(frame.frame_control);
  encoded.insert(encoded.end(), fcf.begin(), fcf.end());
  encoded.push_back(frame.sequence);
  const auto pan = store_le16(frame.destination_pan);
  encoded.insert(encoded.end(), pan.begin(), pan.end());
  if (short_destination) {
    const auto value = std::get<std::uint16_t>(frame.destination);
    if (value > 2U) {
      throw std::invalid_argument(
          "short destination must be address 0, 1, or 2");
    }
    const auto destination = store_le16(value);
    encoded.insert(encoded.end(), destination.begin(), destination.end());
  } else {
    const auto& destination =
        std::get<std::array<std::uint8_t, 8>>(frame.destination);
    encoded.insert(encoded.end(), destination.begin(), destination.end());
  }
  encoded.insert(encoded.end(), frame.source_eui.begin(), frame.source_eui.end());
  encoded.insert(encoded.end(), frame.payload.begin(), frame.payload.end());
  return encoded;
}

auto destination_is_accepted(
    const VmacFrame& frame,
    const std::array<std::uint8_t, 8>& local_eui) noexcept -> bool {
  if (const auto* short_destination =
          std::get_if<std::uint16_t>(&frame.destination)) {
    return *short_destination == 1U || *short_destination == 2U;
  }
  return std::get<std::array<std::uint8_t, 8>>(frame.destination) == local_eui;
}

}  // namespace bh61::core
