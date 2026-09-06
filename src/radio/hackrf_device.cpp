#include "bh61/radio/hackrf_device.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace bh61::radio {
namespace {

auto sample_offset_ns(std::size_t sample_offset, std::uint32_t sample_rate)
    -> std::uint64_t {
  constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000U;
  const auto offset = static_cast<std::uint64_t>(sample_offset);
  const auto rate = static_cast<std::uint64_t>(sample_rate);
  return (offset / rate) * nanoseconds_per_second +
         ((offset % rate) * nanoseconds_per_second) / rate;
}

}  // namespace

HackrfDevice::HackrfDevice(HackrfTransport& transport,
                           HackrfConfiguration configuration,
                           std::chrono::milliseconds receive_timeout)
    : transport_(transport),
      configuration_(std::move(configuration)),
      receive_timeout_(receive_timeout) {
  if (receive_timeout_ <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("HackRF receive timeout must be positive");
  }
}

HackrfDevice::~HackrfDevice() {
  try {
    stop();
    close();
  } catch (...) {
  }
}

void HackrfDevice::open() {
  if (opened_) {
    return;
  }
  const auto failures = validate_hackrf_configuration(configuration_);
  if (!failures.empty()) {
    throw std::invalid_argument(failures.front().message);
  }
  const auto identities = transport_.enumerate();
  const auto selected = std::find_if(
      identities.begin(), identities.end(), [this](const auto& candidate) {
        return candidate.serial == configuration_.serial;
      });
  if (selected == identities.end()) {
    throw std::runtime_error("requested HackRF serial is not attached");
  }
  transport_.open(configuration_.serial);
  opened_ = true;
  try {
    realized_ = transport_.configure(configuration_);
    identity_ = *selected;
  } catch (...) {
    transport_.close();
    opened_ = false;
    throw;
  }
}

void HackrfDevice::start_receive() {
  {
    std::lock_guard lock(mutex_);
    if (!opened_) {
      throw std::logic_error("HackRF device must be open before receive");
    }
    if (receiving_) {
      return;
    }
    queue_.clear();
    next_sample_index_ = 0U;
    terminal_pending_ = false;
    terminal_reason_.clear();
    cancelled_ = false;
    receiving_ = true;
  }
  try {
    transport_.start_rx(
        [this](const HackrfRxTransfer& transfer) { handle_transfer(transfer); });
  } catch (...) {
    std::lock_guard lock(mutex_);
    receiving_ = false;
    throw;
  }
}

void HackrfDevice::stop() {
  {
    std::lock_guard lock(mutex_);
    if (!receiving_) {
      return;
    }
    receiving_ = false;
  }
  transport_.stop_rx();
  {
    std::lock_guard lock(mutex_);
    cancelled_ = true;
  }
  condition_.notify_all();
}

void HackrfDevice::close() {
  stop();
  if (!opened_) {
    return;
  }
  transport_.close();
  opened_ = false;
}

auto HackrfDevice::identity() const -> const HackrfIdentity& {
  if (!opened_) {
    throw std::logic_error("HackRF identity requested before open");
  }
  return identity_;
}

auto HackrfDevice::realized_configuration() const
    -> const HackrfRealizedConfiguration& {
  if (!opened_) {
    throw std::logic_error("HackRF configuration requested before open");
  }
  return realized_;
}

auto HackrfDevice::capabilities() const -> DeviceCapabilities {
  return DeviceCapabilities{
      true,
      false,
      false, false, false, SampleFormat::SignedInt8, 2'000'000U, 20'000'000U};
}

auto HackrfDevice::receive(std::size_t maximum_samples) -> SampleBlock {
  if (maximum_samples == 0U) {
    throw std::invalid_argument("HackRF receive sample count must be nonzero");
  }
  std::unique_lock lock(mutex_);
  const auto ready = condition_.wait_for(lock, receive_timeout_, [this] {
    return !queue_.empty() || terminal_pending_ || cancelled_;
  });
  if (!ready) {
    return SampleBlock{{}, 0U, 0U, false, {}, SampleBlockStatus::Timeout};
  }
  if (queue_.empty()) {
    return terminal_block();
  }
  auto block = std::move(queue_.front());
  queue_.pop_front();
  const auto total_samples = block.bytes.size() / 2U;
  const auto available = total_samples - block.consumed_samples;
  const auto count = std::min(maximum_samples, available);
  const auto byte_offset = block.consumed_samples * 2U;
  const auto decoded = hackrf_iq_to_cf32(
      std::span<const std::int8_t>(block.bytes).subspan(byte_offset, count * 2U));
  if (!decoded.has_value()) {
    return SampleBlock{{}, block.first_sample_index, block.monotonic_time_ns,
                       true, decoded.error(), SampleBlockStatus::Error};
  }
  const auto consumed_before = block.consumed_samples;
  block.consumed_samples += count;
  const auto first_index = block.first_sample_index + consumed_before;
  const auto monotonic = block.monotonic_time_ns +
                         sample_offset_ns(consumed_before,
                                          configuration_.sample_rate);
  const auto discontinuity = block.discontinuity;
  auto reason = block.discontinuity_reason;
  if (block.consumed_samples < total_samples) {
    block.discontinuity = false;
    block.discontinuity_reason.clear();
    queue_.push_front(std::move(block));
  }
  return SampleBlock{*decoded, first_index, monotonic, discontinuity,
                     std::move(reason), SampleBlockStatus::Data};
}

void HackrfDevice::transmit(std::span<const std::complex<float>>,
                            std::uint64_t) {
  throw std::logic_error("HackRF transmit is unavailable in this build");
}

void HackrfDevice::handle_transfer(const HackrfRxTransfer& transfer) {
  std::lock_guard lock(mutex_);
  if (!receiving_) {
    return;
  }
  if (transfer.status != HackrfTransferStatus::Samples) {
    terminal_pending_ = true;
    terminal_reason_ = transfer.detail;
    switch (transfer.status) {
      case HackrfTransferStatus::EndOfStream:
        terminal_status_ = SampleBlockStatus::EndOfStream;
        break;
      case HackrfTransferStatus::DeviceRemoved:
        terminal_status_ = SampleBlockStatus::DeviceRemoved;
        break;
      case HackrfTransferStatus::Error:
        terminal_status_ = SampleBlockStatus::Error;
        break;
      case HackrfTransferStatus::Samples:
        break;
    }
    receiving_ = false;
    condition_.notify_all();
    return;
  }
  if (transfer.bytes.size() % 2U != 0U) {
    terminal_pending_ = true;
    terminal_status_ = SampleBlockStatus::Error;
    terminal_reason_ = "odd_iq_byte_count";
    receiving_ = false;
    condition_.notify_all();
    return;
  }
  const auto sample_count = transfer.bytes.size() / 2U;
  RawBlock block{std::vector<std::int8_t>(transfer.bytes.begin(),
                                          transfer.bytes.end()),
                 next_sample_index_, transfer.monotonic_time_ns, 0U, false, {}};
  next_sample_index_ += sample_count;
  if (queue_.size() == configuration_.queue_capacity_blocks) {
    queue_.pop_front();
    block.discontinuity = true;
    block.discontinuity_reason = "queue_overflow";
  }
  queue_.push_back(std::move(block));
  condition_.notify_one();
}

auto HackrfDevice::terminal_block() const -> SampleBlock {
  if (cancelled_) {
    return SampleBlock{{}, next_sample_index_, 0U, true, "cancelled",
                       SampleBlockStatus::Cancelled};
  }
  return SampleBlock{{}, next_sample_index_, 0U, true, terminal_reason_,
                     terminal_status_};
}

}  // namespace bh61::radio
