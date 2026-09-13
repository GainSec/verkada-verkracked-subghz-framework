#include "bh61/radio/libhackrf_transport.hpp"

#include <array>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <sstream>
#include <utility>

namespace bh61::radio {
namespace {

auto native_message(std::string_view operation, int code,
                    std::string_view name) -> std::string {
  return std::string(operation) + " failed: " + std::string(name) + " (" +
         std::to_string(code) + ")";
}

auto monotonic_now_ns() -> std::uint64_t {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace

NativeHackrfError::NativeHackrfError(std::string operation, int native_code,
                                     std::string native_name)
    : std::runtime_error(native_message(operation, native_code, native_name)),
      operation_(std::move(operation)),
      native_code_(native_code) {}

auto NativeHackrfError::operation() const noexcept -> std::string_view {
  return operation_;
}

auto NativeHackrfError::native_code() const noexcept -> int {
  return native_code_;
}

auto LibhackrfFunctions::native() -> LibhackrfFunctions {
  return {[] { return hackrf_init(); },
          [] { return hackrf_exit(); },
          [] { return hackrf_device_list(); },
          [](hackrf_device_list_t* list) { hackrf_device_list_free(list); },
          [](const char* serial, hackrf_device** device) {
            return hackrf_open_by_serial(serial, device);
          },
          [](hackrf_device* device) { return hackrf_close(device); },
          [](hackrf_device* device, std::uint8_t* value) {
            return hackrf_board_id_read(device, value);
          },
          [](hackrf_device* device, char* value, std::uint8_t length) {
            return hackrf_version_string_read(device, value, length);
          },
          [](hackrf_device* device, std::uint16_t* value) {
            return hackrf_usb_api_version_read(device, value);
          },
          [](hackrf_device* device, std::uint8_t* value) {
            return hackrf_board_rev_read(device, value);
          },
          [](hackrf_board_id value) { return hackrf_board_id_name(value); },
          [](hackrf_board_rev value) { return hackrf_board_rev_name(value); },
          [](hackrf_error value) { return hackrf_error_name(value); },
          [](hackrf_device* device, double value) {
            return hackrf_set_sample_rate(device, value);
          },
          [](std::uint32_t value) {
            return hackrf_compute_baseband_filter_bw(value);
          },
          [](hackrf_device* device, std::uint32_t value) {
            return hackrf_set_baseband_filter_bandwidth(device, value);
          },
          [](hackrf_device* device, std::uint64_t value) {
            return hackrf_set_freq(device, value);
          },
          [](hackrf_device* device, std::uint32_t value) {
            return hackrf_set_lna_gain(device, value);
          },
          [](hackrf_device* device, std::uint32_t value) {
            return hackrf_set_vga_gain(device, value);
          },
          [](hackrf_device* device, std::uint32_t value) {
            return hackrf_set_txvga_gain(device, value);
          },
          [](hackrf_device* device, std::uint8_t value) {
            return hackrf_set_amp_enable(device, value);
          },
          [](hackrf_device* device, std::uint8_t value) {
            return hackrf_set_antenna_enable(device, value);
          },
          [](hackrf_device* device, hackrf_sample_block_cb_fn callback,
             void* context) {
            return hackrf_start_rx(device, callback, context);
          },
          [](hackrf_device* device) { return hackrf_stop_rx(device); },
          [](hackrf_device* device, hackrf_flush_cb_fn callback,
             void* context) {
            return hackrf_enable_tx_flush(device, callback, context);
          },
          [](hackrf_device* device, hackrf_sample_block_cb_fn callback,
             void* context) {
            return hackrf_start_tx(device, callback, context);
          },
          [](hackrf_device* device) { return hackrf_stop_tx(device); }};
}

LibhackrfTransport::LibhackrfTransport(LibhackrfFunctions functions)
    : functions_(std::move(functions)) {
  const auto result = functions_.initialize();
  if (result != HACKRF_SUCCESS) {
    const auto* name = functions_.error_name(static_cast<hackrf_error>(result));
    throw NativeHackrfError("hackrf_init", result,
                            name == nullptr ? "unknown" : name);
  }
  initialized_ = true;
}

LibhackrfTransport::~LibhackrfTransport() {
  if (tx_started_ && device_ != nullptr) {
    static_cast<void>(functions_.stop_tx(device_));
    tx_started_ = false;
  }
  if (rx_started_ && device_ != nullptr) {
    static_cast<void>(functions_.stop_rx(device_));
    rx_started_ = false;
  }
  if (device_ != nullptr) {
    static_cast<void>(functions_.close(device_));
    device_ = nullptr;
  }
  if (initialized_) {
    static_cast<void>(functions_.shutdown());
    initialized_ = false;
  }
}

auto LibhackrfTransport::enumerate() -> std::vector<HackrfIdentity> {
  auto* list = functions_.device_list();
  if (list == nullptr) {
    throw std::runtime_error("hackrf_device_list returned null");
  }
  std::vector<HackrfIdentity> identities;
  try {
    for (int index = 0; index < list->devicecount; ++index) {
      if (list->serial_numbers[index] == nullptr) {
        continue;
      }
      const std::string serial(list->serial_numbers[index]);
      hackrf_device* candidate{};
      require_success(functions_.open_by_serial(serial.c_str(), &candidate),
                      "hackrf_open_by_serial");
      try {
        identities.push_back(read_identity(serial, candidate));
      } catch (...) {
        static_cast<void>(functions_.close(candidate));
        throw;
      }
      require_success(functions_.close(candidate), "hackrf_close");
    }
  } catch (...) {
    functions_.device_list_free(list);
    throw;
  }
  functions_.device_list_free(list);
  return identities;
}

void LibhackrfTransport::open(std::string_view serial) {
  if (device_ != nullptr) {
    throw std::logic_error("a HackRF device is already open");
  }
  const std::string owned_serial(serial);
  require_success(
      functions_.open_by_serial(owned_serial.c_str(), &device_),
      "hackrf_open_by_serial");
}

auto LibhackrfTransport::configure(const HackrfConfiguration& configuration)
    -> HackrfRealizedConfiguration {
  if (device_ == nullptr) {
    throw std::logic_error("HackRF configure requires an open device");
  }
  require_success(
      functions_.set_sample_rate(device_,
                                 static_cast<double>(configuration.sample_rate)),
      "hackrf_set_sample_rate");
  const auto realized_filter =
      functions_.compute_baseband_filter(configuration.baseband_filter_hz);
  require_success(functions_.set_baseband_filter(device_, realized_filter),
                  "hackrf_set_baseband_filter_bandwidth");
  require_success(
      functions_.set_frequency(device_, configuration.center_frequency_hz),
      "hackrf_set_freq");
  require_success(functions_.set_lna_gain(device_, configuration.lna_gain_db),
                  "hackrf_set_lna_gain");
  require_success(functions_.set_vga_gain(device_, configuration.vga_gain_db),
                  "hackrf_set_vga_gain");
  require_success(functions_.set_tx_vga_gain(
                      device_, configuration.tx_vga_gain_db),
                  "hackrf_set_txvga_gain");
  require_success(
      functions_.set_amplifier(device_,
                               configuration.amplifier_enabled ? 1U : 0U),
      "hackrf_set_amp_enable");
  require_success(
      functions_.set_antenna_power(
          device_, configuration.antenna_power_enabled ? 1U : 0U),
      "hackrf_set_antenna_enable");
  return {configuration.sample_rate, configuration.center_frequency_hz,
          realized_filter, configuration.lna_gain_db,
          configuration.vga_gain_db};
}

void LibhackrfTransport::start_rx(HackrfRxCallback callback) {
  if (device_ == nullptr) {
    throw std::logic_error("HackRF RX requires an open device");
  }
  if (rx_started_) {
    return;
  }
  receive_callback_ = std::move(callback);
  try {
    require_success(functions_.start_rx(device_, &LibhackrfTransport::rx_thunk,
                                        this),
                    "hackrf_start_rx");
    rx_started_ = true;
  } catch (...) {
    receive_callback_ = {};
    throw;
  }
}

void LibhackrfTransport::stop_rx() {
  if (!rx_started_) {
    return;
  }
  const auto result = functions_.stop_rx(device_);
  rx_started_ = false;
  receive_callback_ = {};
  require_success(result, "hackrf_stop_rx");
}

void LibhackrfTransport::close() {
  if (device_ == nullptr) {
    return;
  }
  if (rx_started_) {
    stop_rx();
  }
  if (tx_started_) {
    require_success(functions_.stop_tx(device_), "hackrf_stop_tx");
    tx_started_ = false;
  }
  auto* closing = device_;
  device_ = nullptr;
  require_success(functions_.close(closing), "hackrf_close");
}

void LibhackrfTransport::transmit(std::span<const std::int8_t> bytes) {
  if (device_ == nullptr) {
    throw std::logic_error("HackRF TX requires an open device");
  }
  if (rx_started_ || tx_started_) {
    throw std::logic_error("HackRF is already streaming");
  }
  if (bytes.empty() || bytes.size() % 2U != 0U) {
    throw std::invalid_argument("HackRF TX requires nonempty interleaved IQ");
  }
  {
    std::lock_guard lock(tx_mutex_);
    tx_bytes_.assign(bytes.begin(), bytes.end());
    tx_offset_ = 0U;
    tx_flush_result_ = HACKRF_SUCCESS;
    tx_flushed_ = false;
  }
  require_success(functions_.enable_tx_flush(
                      device_, &LibhackrfTransport::tx_flush_thunk, this),
                  "hackrf_enable_tx_flush");
  try {
    require_success(
        functions_.start_tx(device_, &LibhackrfTransport::tx_thunk, this),
        "hackrf_start_tx");
    tx_started_ = true;
  } catch (...) {
    std::lock_guard lock(tx_mutex_);
    tx_bytes_.clear();
    throw;
  }
  {
    std::unique_lock lock(tx_mutex_);
    if (!tx_condition_.wait_for(lock, std::chrono::seconds(30),
                                [this] { return tx_flushed_; })) {
      lock.unlock();
      const auto stop_result = functions_.stop_tx(device_);
      tx_started_ = false;
      require_success(stop_result, "hackrf_stop_tx");
      throw std::runtime_error("HackRF TX flush timed out");
    }
  }
  const auto stop_result = functions_.stop_tx(device_);
  tx_started_ = false;
  require_success(stop_result, "hackrf_stop_tx");
  std::lock_guard lock(tx_mutex_);
  tx_bytes_.clear();
  if (tx_flush_result_ != HACKRF_SUCCESS) {
    throw NativeHackrfError("hackrf_tx_flush", tx_flush_result_,
                            "transmission flush failed");
  }
}

auto LibhackrfTransport::rx_thunk(hackrf_transfer* transfer) -> int {
  if (transfer == nullptr || transfer->rx_ctx == nullptr) {
    return -1;
  }
  return static_cast<LibhackrfTransport*>(transfer->rx_ctx)->on_rx(transfer);
}

auto LibhackrfTransport::tx_thunk(hackrf_transfer* transfer) -> int {
  if (transfer == nullptr || transfer->tx_ctx == nullptr) return -1;
  return static_cast<LibhackrfTransport*>(transfer->tx_ctx)->on_tx(transfer);
}

void LibhackrfTransport::tx_flush_thunk(void* context, int result) {
  if (context == nullptr) return;
  auto* self = static_cast<LibhackrfTransport*>(context);
  {
    std::lock_guard lock(self->tx_mutex_);
    self->tx_flush_result_ = result;
    self->tx_flushed_ = true;
  }
  self->tx_condition_.notify_all();
}

auto LibhackrfTransport::on_tx(hackrf_transfer* transfer) noexcept -> int {
  try {
    std::lock_guard lock(tx_mutex_);
    if (transfer->buffer == nullptr || transfer->buffer_length <= 0 ||
        transfer->buffer_length % 2 != 0 || tx_offset_ >= tx_bytes_.size()) {
      transfer->valid_length = 0;
      return -1;
    }
    const auto remaining = tx_bytes_.size() - tx_offset_;
    const auto count = std::min(
        remaining, static_cast<std::size_t>(transfer->buffer_length));
    std::memcpy(transfer->buffer, tx_bytes_.data() + tx_offset_, count);
    transfer->valid_length = static_cast<int>(count);
    tx_offset_ += count;
    return tx_offset_ == tx_bytes_.size() ? -1 : 0;
  } catch (...) {
    transfer->valid_length = 0;
    return -1;
  }
}

auto LibhackrfTransport::on_rx(hackrf_transfer* transfer) noexcept -> int {
  try {
    if (transfer->buffer == nullptr || transfer->valid_length < 0 ||
        transfer->valid_length > transfer->buffer_length ||
        transfer->valid_length % 2 != 0) {
      receive_callback_(HackrfRxTransfer{
          {}, monotonic_now_ns(), HackrfTransferStatus::Error,
          "invalid_native_transfer_length"});
      return -1;
    }
    const auto* bytes = reinterpret_cast<const std::int8_t*>(transfer->buffer);
    receive_callback_(HackrfRxTransfer{
        std::span<const std::int8_t>(
            bytes, static_cast<std::size_t>(transfer->valid_length)),
        monotonic_now_ns(), HackrfTransferStatus::Samples, {}});
    return 0;
  } catch (...) {
    return -1;
  }
}

void LibhackrfTransport::require_success(int result,
                                         std::string_view operation) const {
  if (result == HACKRF_SUCCESS) {
    return;
  }
  const auto* name = functions_.error_name(static_cast<hackrf_error>(result));
  throw NativeHackrfError(std::string(operation), result,
                          name == nullptr ? "unknown" : name);
}

auto LibhackrfTransport::read_identity(std::string serial,
                                       hackrf_device* device) const
    -> HackrfIdentity {
  std::uint8_t board_id = BOARD_ID_UNDETECTED;
  require_success(functions_.board_id_read(device, &board_id),
                  "hackrf_board_id_read");
  std::array<char, 256> version{};
  require_success(
      functions_.version_string_read(device, version.data(), 255U),
      "hackrf_version_string_read");
  std::uint16_t usb_api{};
  require_success(functions_.usb_api_version_read(device, &usb_api),
                  "hackrf_usb_api_version_read");
  std::uint8_t revision = BOARD_REV_UNDETECTED;
  require_success(functions_.board_rev_read(device, &revision),
                  "hackrf_board_rev_read");
  std::ostringstream firmware;
  firmware << version.data() << " (API:" << std::hex
           << static_cast<unsigned>((usb_api >> 8U) & 0xffU) << '.'
           << std::setw(2) << std::setfill('0')
           << static_cast<unsigned>(usb_api & 0xffU) << ')';
  const auto* board_name =
      functions_.board_id_name(static_cast<hackrf_board_id>(board_id));
  const auto* revision_name =
      functions_.board_rev_name(static_cast<hackrf_board_rev>(revision));
  return {std::move(serial), board_name == nullptr ? "unknown" : board_name,
          firmware.str(),
          revision_name == nullptr ? "unknown" : revision_name};
}

}  // namespace bh61::radio
