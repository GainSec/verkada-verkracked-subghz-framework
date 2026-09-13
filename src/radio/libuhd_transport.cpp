#include "bh61/radio/libuhd_transport.hpp"

#include <uhd/device.hpp>
#include <uhd/stream.hpp>
#include <uhd/types/metadata.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <uhd/usrp/multi_usrp.hpp>

#include <chrono>
#include <stdexcept>

namespace bh61::radio {

struct LibuhdTransport::Implementation {
  uhd::usrp::multi_usrp::sptr usrp;
  uhd::rx_streamer::sptr rx;
  uhd::rx_streamer::sptr rx_pair;
  uhd::tx_streamer::sptr tx;
  UhdConfiguration configuration;
  std::uint64_t sample_index{};
  bool continuous_receive{};
};

namespace {

void throw_on_tx_async_error(
    const uhd::tx_streamer::sptr& tx) {
  if (!tx) return;
  uhd::async_metadata_t metadata;
  while (tx->recv_async_msg(metadata, 0.0)) {
    if (metadata.event_code == uhd::async_metadata_t::EVENT_CODE_OK ||
        metadata.event_code ==
            uhd::async_metadata_t::EVENT_CODE_BURST_ACK) {
      continue;
    }
    throw std::runtime_error("UHD TX asynchronous error: " +
                             metadata.strevent());
  }
}

}  // namespace

LibuhdTransport::LibuhdTransport()
    : implementation_(std::make_unique<Implementation>()) {}
LibuhdTransport::~LibuhdTransport() = default;

auto LibuhdTransport::enumerate() -> std::vector<UhdIdentity> {
  std::vector<UhdIdentity> result;
  for (const auto& address : uhd::device::find(uhd::device_addr_t("type=b200"))) {
    result.push_back({address.get("serial", ""), address.get("product", "USRP"),
                      address.get("name", "")});
  }
  return result;
}

void LibuhdTransport::open(const UhdConfiguration& configuration) {
  if (implementation_->usrp) throw std::logic_error("a UHD device is already open");
  auto device_arguments = "type=b200,serial=" + configuration.serial;
  if (!configuration.fpga_path.empty()) {
    device_arguments += ",fpga=" + configuration.fpga_path;
  }
  device_arguments +=
      ",num_recv_frames=" + std::to_string(configuration.receive_frame_count);
  implementation_->usrp = uhd::usrp::multi_usrp::make(device_arguments);
  implementation_->configuration = configuration;
  auto& usrp = *implementation_->usrp;
  if (configuration.enable_receive || configuration.enable_coherent_receive) {
    usrp.set_rx_rate(configuration.sample_rate, configuration.rx_channel);
    usrp.set_rx_freq(configuration.center_frequency_hz,
                     configuration.rx_channel);
    usrp.set_rx_gain(configuration.rx_gain_db, configuration.rx_channel);
    if (!configuration.rx_antenna.empty()) {
      usrp.set_rx_antenna(configuration.rx_antenna,
                          configuration.rx_channel);
    }
  }
#if defined(BH61_ENABLE_TX)
  if (configuration.enable_transmit) {
    usrp.set_tx_rate(configuration.sample_rate, configuration.tx_channel);
    usrp.set_tx_freq(configuration.center_frequency_hz,
                     configuration.tx_channel);
    usrp.set_tx_gain(configuration.tx_gain_db, configuration.tx_channel);
    if (!configuration.tx_antenna.empty()) {
      usrp.set_tx_antenna(configuration.tx_antenna,
                          configuration.tx_channel);
    }
  }
#endif
  if (configuration.enable_receive) {
    uhd::stream_args_t rx_args("fc32", "sc16");
    rx_args.channels = {configuration.rx_channel};
    implementation_->rx = usrp.get_rx_stream(rx_args);
  }
  if (configuration.enable_coherent_receive &&
      usrp.get_rx_num_channels() >= 2U) {
    const auto second_channel = configuration.rx_channel == 0U ? 1U : 0U;
    usrp.set_rx_rate(configuration.sample_rate, second_channel);
    usrp.set_rx_freq(configuration.center_frequency_hz, second_channel);
    usrp.set_rx_gain(configuration.rx_gain_db, second_channel);
    uhd::stream_args_t pair_args("fc32", "sc16");
    pair_args.channels = {configuration.rx_channel, second_channel};
    implementation_->rx_pair = usrp.get_rx_stream(pair_args);
  }
#if defined(BH61_ENABLE_TX)
  if (configuration.enable_transmit) {
    uhd::stream_args_t tx_args("fc32", "sc16");
    tx_args.channels = {configuration.tx_channel};
    implementation_->tx = usrp.get_tx_stream(tx_args);
  }
#endif
  implementation_->sample_index = 0U;
  implementation_->continuous_receive = false;
}

void LibuhdTransport::start_receive_stream() {
  if (!implementation_->rx) {
    throw std::logic_error("UHD RX requires an open device");
  }
  if (implementation_->continuous_receive) {
    throw std::logic_error("UHD continuous RX is already active");
  }
  uhd::stream_cmd_t command(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
  command.stream_now = true;
  implementation_->rx->issue_stream_cmd(command);
  implementation_->continuous_receive = true;
}

void LibuhdTransport::stop_receive_stream() {
  if (!implementation_->rx) {
    throw std::logic_error("UHD RX requires an open device");
  }
  if (!implementation_->continuous_receive) return;
  uhd::stream_cmd_t command(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
  implementation_->rx->issue_stream_cmd(command);
  implementation_->continuous_receive = false;
}

auto LibuhdTransport::receive(std::size_t maximum_samples) -> SampleBlock {
  if (!implementation_->rx) throw std::logic_error("UHD RX requires an open device");
  throw_on_tx_async_error(implementation_->tx);
  std::vector<std::complex<float>> samples(maximum_samples);
  if (!implementation_->continuous_receive) {
    uhd::stream_cmd_t command(
        uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
    command.num_samps = maximum_samples;
    command.stream_now = true;
    implementation_->rx->issue_stream_cmd(command);
  }
  uhd::rx_metadata_t metadata;
  const auto count = implementation_->rx->recv(samples.data(), samples.size(),
                                                metadata, 1.0, false);
  samples.resize(count);
  if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
    return {{}, implementation_->sample_index, 0U, false, {},
            SampleBlockStatus::Timeout};
  }
  if (metadata.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
    return {{}, implementation_->sample_index, 0U, true,
            metadata.strerror(), SampleBlockStatus::Error};
  }
  const auto first = implementation_->sample_index;
  implementation_->sample_index += count;
  const auto device_time_ns = static_cast<std::uint64_t>(
      metadata.time_spec.get_real_secs() * 1'000'000'000.0);
  return {std::move(samples), first, device_time_ns, false, {},
          SampleBlockStatus::Data};
}

auto LibuhdTransport::receive_pair(std::size_t maximum_samples)
    -> CoherentSamplePair {
  if (!implementation_->rx_pair) {
    throw std::logic_error("UHD coherent RX requires two receive channels");
  }
  std::vector<std::complex<float>> channel_a(maximum_samples);
  std::vector<std::complex<float>> channel_b(maximum_samples);
  std::vector<std::complex<float>*> buffers{channel_a.data(), channel_b.data()};
  uhd::stream_cmd_t command(uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
  command.num_samps = maximum_samples;
  command.stream_now = true;
  implementation_->rx_pair->issue_stream_cmd(command);
  uhd::rx_metadata_t metadata;
  const auto count = implementation_->rx_pair->recv(
      buffers, maximum_samples, metadata, 1.0, false);
  channel_a.resize(count);
  channel_b.resize(count);
  auto status = SampleBlockStatus::Data;
  std::string reason;
  if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
    status = SampleBlockStatus::Timeout;
  } else if (metadata.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
    status = SampleBlockStatus::Error;
    reason = metadata.strerror();
  }
  const auto first = implementation_->sample_index;
  implementation_->sample_index += count;
  const auto time_ns = static_cast<std::uint64_t>(
      metadata.time_spec.get_real_secs() * 1'000'000'000.0);
  return {{std::move(channel_a), first, time_ns, status == SampleBlockStatus::Error,
           reason, status},
          {std::move(channel_b), first, time_ns, status == SampleBlockStatus::Error,
           reason, status}};
}

auto LibuhdTransport::current_time_ns() const -> std::uint64_t {
  if (!implementation_->usrp) {
    throw std::logic_error("UHD hardware clock requires an open device");
  }
  return static_cast<std::uint64_t>(
      implementation_->usrp->get_time_now().get_real_secs() *
      1'000'000'000.0);
}

void LibuhdTransport::transmit(
    std::span<const std::complex<float>> samples,
    std::uint64_t requested_time_ns) {
#if defined(BH61_ENABLE_TX)
  if (!implementation_->tx) throw std::logic_error("UHD TX requires an open device");
  uhd::tx_metadata_t metadata;
  metadata.start_of_burst = true;
  // UHD fragments a caller buffer internally and preserves end_of_burst on
  // the final fragment. Submit each finite BH61 waveform as one complete
  // burst instead of following it with a second, blocking zero-sample send.
  metadata.end_of_burst = true;
  metadata.has_time_spec = requested_time_ns != 0U;
  if (metadata.has_time_spec) {
    metadata.time_spec = uhd::time_spec_t(
        static_cast<double>(requested_time_ns) / 1'000'000'000.0);
  }
  const auto count = implementation_->tx->send(
      samples.data(), samples.size(), metadata, 0.1);
  if (count != samples.size()) {
    throw std::runtime_error("UHD TX did not accept the complete finite burst");
  }
#else
  static_cast<void>(samples);
  static_cast<void>(requested_time_ns);
  throw std::logic_error("UHD transmit is unavailable in this build");
#endif
}

void LibuhdTransport::close() {
  if (implementation_->rx && implementation_->continuous_receive) {
    stop_receive_stream();
  }
  implementation_->rx.reset();
  implementation_->rx_pair.reset();
  implementation_->tx.reset();
  implementation_->usrp.reset();
}

}  // namespace bh61::radio
