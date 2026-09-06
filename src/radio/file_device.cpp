#include "bh61/radio/file_device.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace bh61::radio {
namespace {

auto sample_time_ns(std::size_t sample_index, std::uint32_t sample_rate)
    -> std::uint64_t {
  constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000U;
  const auto index = static_cast<std::uint64_t>(sample_index);
  const auto rate = static_cast<std::uint64_t>(sample_rate);
  return (index / rate) * nanoseconds_per_second +
         ((index % rate) * nanoseconds_per_second) / rate;
}

}  // namespace

FileDevice::FileDevice(
    std::span<const std::complex<float>> receive_samples,
    std::uint32_t sample_rate, std::uint64_t center_frequency_hz)
    : receive_samples_(receive_samples.begin(), receive_samples.end()),
      sample_rate_(sample_rate),
      center_frequency_hz_(center_frequency_hz) {
  if (sample_rate == 0U) {
    throw std::invalid_argument("file device sample rate must be nonzero");
  }
}

auto FileDevice::capabilities() const -> DeviceCapabilities {
  return DeviceCapabilities{true,
                            true,
                            false,
                            true,
                            true,
                            SampleFormat::ComplexFloat32,
                            1U,
                            std::numeric_limits<std::uint32_t>::max()};
}

auto FileDevice::receive(std::size_t maximum_samples) -> SampleBlock {
  const auto count =
      std::min(maximum_samples, receive_samples_.size() - receive_offset_);
  const auto first_index = receive_offset_;
  std::vector<std::complex<float>> block(
      receive_samples_.begin() + static_cast<std::ptrdiff_t>(receive_offset_),
      receive_samples_.begin() +
          static_cast<std::ptrdiff_t>(receive_offset_ + count));
  receive_offset_ += count;
  return SampleBlock{std::move(block),
                     static_cast<std::uint64_t>(first_index),
                     sample_time_ns(first_index, sample_rate_), false, {},
                     count == 0U ? SampleBlockStatus::EndOfStream
                                 : SampleBlockStatus::Data};
}

void FileDevice::transmit(std::span<const std::complex<float>> samples,
                          std::uint64_t requested_time_ns) {
  transmit_records_.push_back(TransmitRecord{
      requested_time_ns, samples.size(), transmitted_samples_.size()});
  transmitted_samples_.insert(transmitted_samples_.end(), samples.begin(),
                              samples.end());
}

auto FileDevice::eof() const -> bool {
  return receive_offset_ == receive_samples_.size();
}

auto FileDevice::transmitted_samples() const
    -> const std::vector<std::complex<float>>& {
  return transmitted_samples_;
}

auto FileDevice::transmit_records() const
    -> const std::vector<TransmitRecord>& {
  return transmit_records_;
}

auto FileDevice::sample_rate() const -> std::uint32_t { return sample_rate_; }

auto FileDevice::center_frequency_hz() const -> std::uint64_t {
  return center_frequency_hz_;
}

}  // namespace bh61::radio
