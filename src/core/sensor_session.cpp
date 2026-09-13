#include "bh61/core/sensor_session.hpp"

#include "bh61/core/sha256.hpp"

#include <openssl/crypto.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

namespace bh61::core {
namespace {

constexpr int kSessionSchemaVersion = 2;
constexpr int kLegacyReversedCounterSchemaVersion = 1;

auto state_name(SensorSessionState state) -> std::string_view {
  switch (state) {
    case SensorSessionState::Created: return "created";
    case SensorSessionState::JoinSent: return "join_sent";
    case SensorSessionState::ResponseReceived: return "response_received";
    case SensorSessionState::KeyDerived: return "key_derived";
    case SensorSessionState::Joined: return "joined";
    case SensorSessionState::EventSent: return "event_sent";
    case SensorSessionState::Acknowledged: return "acknowledged";
  }
  throw std::logic_error("unknown sensor session state");
}

auto parse_state(std::string_view value) -> SensorSessionState {
  if (value == "created") return SensorSessionState::Created;
  if (value == "join_sent") return SensorSessionState::JoinSent;
  if (value == "response_received") return SensorSessionState::ResponseReceived;
  if (value == "key_derived") return SensorSessionState::KeyDerived;
  if (value == "joined") return SensorSessionState::Joined;
  if (value == "event_sent") return SensorSessionState::EventSent;
  if (value == "acknowledged") return SensorSessionState::Acknowledged;
  throw std::runtime_error("session state name is invalid");
}

auto hex_digit(std::uint8_t value) -> char {
  constexpr std::string_view digits = "0123456789abcdef";
  return digits[value & 0x0fU];
}

template <std::size_t N>
auto to_hex(const std::array<std::uint8_t, N>& bytes) -> std::string {
  std::string result(bytes.size() * 2U, '0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    result[index * 2U] = hex_digit(static_cast<std::uint8_t>(bytes[index] >> 4U));
    result[index * 2U + 1U] = hex_digit(bytes[index]);
  }
  return result;
}

auto nibble(char value) -> std::uint8_t {
  if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
  if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
  throw std::runtime_error("session contains noncanonical hexadecimal text");
}

template <std::size_t N>
auto from_hex(std::string_view text) -> std::array<std::uint8_t, N> {
  if (text.size() != N * 2U) {
    throw std::runtime_error("session hexadecimal field has the wrong length");
  }
  std::array<std::uint8_t, N> result{};
  for (std::size_t index = 0; index < N; ++index) {
    result[index] = static_cast<std::uint8_t>((nibble(text[index * 2U]) << 4U) |
                                              nibble(text[index * 2U + 1U]));
  }
  return result;
}

auto valid_serial(std::string_view serial) -> bool {
  return !serial.empty() && serial.size() <= 32U &&
         std::all_of(serial.begin(), serial.end(), [](unsigned char value) {
           return std::isalnum(value) != 0 || value == '-' || value == '_' ||
                  value == '.';
         });
}

void write_all(int descriptor, std::span<const std::uint8_t> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = ::write(descriptor, bytes.data() + offset,
                               bytes.size() - offset);
    if (count < 0) {
      throw std::system_error(errno, std::generic_category(), "write session file");
    }
    offset += static_cast<std::size_t>(count);
  }
}

void atomic_write(const std::filesystem::path& path,
                  std::span<const std::uint8_t> bytes, mode_t mode) {
  const auto temporary = path.string() + "." + std::to_string(::getpid()) + ".tmp";
  const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, mode);
  if (descriptor < 0) {
    throw std::system_error(errno, std::generic_category(), "create temporary session file");
  }
  bool descriptor_open = true;
  try {
    write_all(descriptor, bytes);
    if (::fsync(descriptor) != 0) {
      throw std::system_error(errno, std::generic_category(), "fsync session file");
    }
    if (::close(descriptor) != 0) {
      descriptor_open = false;
      throw std::system_error(errno, std::generic_category(), "close session file");
    }
    descriptor_open = false;
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
      throw std::system_error(errno, std::generic_category(), "rename session file");
    }
    const int directory = ::open(path.parent_path().c_str(), O_RDONLY);
    if (directory >= 0) {
      (void)::fsync(directory);
      (void)::close(directory);
    }
  } catch (...) {
    if (descriptor_open) (void)::close(descriptor);
    (void)::unlink(temporary.c_str());
    throw;
  }
}

auto quoted_field(const std::string& text, std::string_view name) -> std::string {
  const std::string marker = "  \"" + std::string(name) + "\": \"";
  const auto start = text.find(marker);
  if (start == std::string::npos) throw std::runtime_error("session field is missing");
  const auto value_start = start + marker.size();
  const auto end = text.find('"', value_start);
  if (end == std::string::npos || text.find('\\', value_start) < end) {
    throw std::runtime_error("session string field is malformed");
  }
  return text.substr(value_start, end - value_start);
}

auto number_field(const std::string& text, std::string_view name) -> std::uint64_t {
  const std::string marker = "  \"" + std::string(name) + "\": ";
  const auto start = text.find(marker);
  if (start == std::string::npos) throw std::runtime_error("session field is missing");
  const auto value_start = start + marker.size();
  const auto end = text.find_first_not_of("0123456789", value_start);
  if (end == value_start) throw std::runtime_error("session number field is malformed");
  std::uint64_t value{};
  const auto result = std::from_chars(text.data() + value_start, text.data() + end, value);
  if (result.ec != std::errc{} || result.ptr != text.data() + end) {
    throw std::runtime_error("session number field is invalid");
  }
  return value;
}

auto canonical_json(const SensorIdentity& identity, SensorSessionState state,
                    bool remote_present,
                    const std::array<std::uint8_t, 64>& remote_public,
                    std::string_view fingerprint, std::uint16_t send_counter,
                    std::uint16_t receive_counter, std::uint8_t join_format,
                    const std::optional<std::uint32_t>& pending_event,
                    int schema_version = kSessionSchemaVersion) -> std::string {
  return "{\n"
         "  \"schema_version\": " + std::to_string(schema_version) + ",\n" +
         "  \"state\": \"" + std::string(state_name(state)) + "\",\n" +
         "  \"serial\": \"" + identity.serial + "\",\n" +
         "  \"physical_id_hex\": \"" + to_hex(identity.physical_device_id) + "\",\n" +
         "  \"eui64_hex\": \"" + to_hex(identity.eui64) + "\",\n" +
         "  \"nonce_hex\": \"" + to_hex(identity.nonce) + "\",\n" +
         "  \"public_key_hex\": \"" + to_hex(identity.public_xy) + "\",\n" +
         "  \"remote_public_key_hex\": \"" +
             (remote_present ? to_hex(remote_public) : std::string()) + "\",\n" +
         "  \"key_fingerprint_sha256\": \"" + std::string(fingerprint) + "\",\n" +
         "  \"send_counter\": " + std::to_string(send_counter) + ",\n" +
         "  \"receive_counter\": " + std::to_string(receive_counter) + ",\n" +
         "  \"join_format\": " + std::to_string(join_format) + ",\n" +
         "  \"pending_event_id\": \"" +
             (pending_event ? std::to_string(*pending_event) : std::string()) + "\"\n"
         "}\n";
}

}  // namespace

auto public_key_hash4(std::span<const std::uint8_t, 64> public_xy)
    -> std::array<std::uint8_t, 4> {
  const auto hash = sha256_hex(public_xy);
  return from_hex<4>(std::string_view(hash).substr(0, 8));
}

auto session_key_fingerprint(const Aes128Key& key) -> std::string {
  return sha256_hex(key);
}

auto sensor_session_state_name(SensorSessionState state) -> std::string_view {
  return state_name(state);
}

SensorSession::SensorSession(std::filesystem::path output_directory,
                             SensorIdentity identity,
                             std::array<std::uint8_t, 32> private_scalar)
    : output_directory_(std::move(output_directory)),
      identity_(std::move(identity)),
      private_scalar_(private_scalar) {}

SensorSession::SensorSession(SensorSession&& other) noexcept
    : output_directory_(std::move(other.output_directory_)),
      identity_(std::move(other.identity_)),
      private_scalar_(other.private_scalar_), state_(other.state_),
      remote_public_present_(other.remote_public_present_),
      remote_public_xy_(other.remote_public_xy_),
      key_fingerprint_(std::move(other.key_fingerprint_)),
      send_counter_(other.send_counter_), receive_counter_(other.receive_counter_),
      join_format_(other.join_format_), pending_event_id_(other.pending_event_id_) {
  OPENSSL_cleanse(other.private_scalar_.data(), other.private_scalar_.size());
}

auto SensorSession::operator=(SensorSession&& other) noexcept -> SensorSession& {
  if (this != &other) {
    OPENSSL_cleanse(private_scalar_.data(), private_scalar_.size());
    output_directory_ = std::move(other.output_directory_);
    identity_ = std::move(other.identity_);
    private_scalar_ = other.private_scalar_;
    state_ = other.state_;
    remote_public_present_ = other.remote_public_present_;
    remote_public_xy_ = other.remote_public_xy_;
    key_fingerprint_ = std::move(other.key_fingerprint_);
    send_counter_ = other.send_counter_;
    receive_counter_ = other.receive_counter_;
    join_format_ = other.join_format_;
    pending_event_id_ = other.pending_event_id_;
    OPENSSL_cleanse(other.private_scalar_.data(), other.private_scalar_.size());
  }
  return *this;
}

SensorSession::~SensorSession() {
  OPENSSL_cleanse(private_scalar_.data(), private_scalar_.size());
}

auto SensorSession::create(const std::filesystem::path& output_directory,
                           SensorIdentity identity,
                           std::array<std::uint8_t, 32> private_scalar)
    -> SensorSession {
  if (!valid_serial(identity.serial)) {
    throw std::invalid_argument("synthetic sensor serial is invalid");
  }
  if (p256_public_from_private(private_scalar) != identity.public_xy) {
    throw std::invalid_argument("sensor public key does not match private scalar");
  }
  if (!std::filesystem::create_directory(output_directory)) {
    throw std::runtime_error("sensor session output directory already exists");
  }
  std::filesystem::permissions(output_directory,
                               std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  atomic_write(output_directory / "sensor-private.key", private_scalar, 0600);
  SensorSession session(output_directory, std::move(identity), private_scalar);
  session.persist();
  return session;
}

auto SensorSession::load(const std::filesystem::path& output_directory)
    -> SensorSession {
  const auto private_path = output_directory / "sensor-private.key";
  const auto permissions = std::filesystem::status(private_path).permissions();
  using P = std::filesystem::perms;
  if ((permissions & (P::group_all | P::others_all)) != P::none) {
    throw std::runtime_error("sensor private key permissions are not 0600-compatible");
  }
  std::ifstream private_input(private_path, std::ios::binary);
  const std::vector<std::uint8_t> private_bytes{
      std::istreambuf_iterator<char>(private_input), std::istreambuf_iterator<char>()};
  if (private_bytes.size() != 32U) {
    throw std::runtime_error("sensor private key file must contain exactly 32 bytes");
  }
  std::array<std::uint8_t, 32> private_scalar{};
  std::copy(private_bytes.begin(), private_bytes.end(), private_scalar.begin());

  std::ifstream state_input(output_directory / "session.json", std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(state_input),
                         std::istreambuf_iterator<char>()};
  const auto schema_version = number_field(text, "schema_version");
  if (schema_version != kSessionSchemaVersion &&
      schema_version != kLegacyReversedCounterSchemaVersion) {
    throw std::runtime_error("unsupported sensor session schema");
  }
  SensorIdentity identity{
      quoted_field(text, "serial"),
      from_hex<8>(quoted_field(text, "physical_id_hex")),
      from_hex<8>(quoted_field(text, "eui64_hex")),
      from_hex<8>(quoted_field(text, "nonce_hex")),
      from_hex<64>(quoted_field(text, "public_key_hex"))};
  if (!valid_serial(identity.serial) ||
      p256_public_from_private(private_scalar) != identity.public_xy) {
    throw std::runtime_error("sensor session identity does not match private key");
  }
  SensorSession session(output_directory, identity, private_scalar);
  session.state_ = parse_state(quoted_field(text, "state"));
  const auto remote = quoted_field(text, "remote_public_key_hex");
  if (!remote.empty()) {
    session.remote_public_xy_ = from_hex<64>(remote);
    session.remote_public_present_ = true;
    (void)p256_shared_x(private_scalar, session.remote_public_xy_);
  }
  session.key_fingerprint_ = quoted_field(text, "key_fingerprint_sha256");
  if (!session.key_fingerprint_.empty()) {
    (void)from_hex<32>(session.key_fingerprint_);
  }
  const auto send = number_field(text, "send_counter");
  const auto receive = number_field(text, "receive_counter");
  const auto format = number_field(text, "join_format");
  if (send > 0xffffU || receive > 0xffffU || format > 0xffU) {
    throw std::runtime_error("sensor session numeric field is out of range");
  }
  session.send_counter_ = static_cast<std::uint16_t>(send);
  session.receive_counter_ = static_cast<std::uint16_t>(receive);
  session.join_format_ = static_cast<std::uint8_t>(format);
  const auto pending = quoted_field(text, "pending_event_id");
  if (!pending.empty()) {
    std::uint32_t value{};
    const auto parsed = std::from_chars(pending.data(), pending.data() + pending.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != pending.data() + pending.size()) {
      throw std::runtime_error("pending event ID is invalid");
    }
    session.pending_event_id_ = value;
  }
  if (canonical_json(session.identity_, session.state_, session.remote_public_present_,
                     session.remote_public_xy_, session.key_fingerprint_,
                     session.send_counter_, session.receive_counter_,
                     session.join_format_, session.pending_event_id_,
                     static_cast<int>(schema_version)) != text) {
    throw std::runtime_error("sensor session JSON is noncanonical or corrupt");
  }
  if (schema_version == kLegacyReversedCounterSchemaVersion) {
    std::swap(session.send_counter_, session.receive_counter_);
    session.persist();
  }
  return session;
}

auto SensorSession::state() const noexcept -> SensorSessionState { return state_; }
auto SensorSession::identity() const noexcept -> const SensorIdentity& { return identity_; }
auto SensorSession::private_scalar() const noexcept
    -> const std::array<std::uint8_t, 32>& { return private_scalar_; }
auto SensorSession::remote_public_xy() const
    -> const std::array<std::uint8_t, 64>& {
  if (!remote_public_present_) throw std::logic_error("join response has not been accepted");
  return remote_public_xy_;
}
auto SensorSession::key_fingerprint() const noexcept -> const std::string& { return key_fingerprint_; }
auto SensorSession::send_counter() const noexcept -> std::uint16_t { return send_counter_; }
auto SensorSession::receive_counter() const noexcept -> std::uint16_t { return receive_counter_; }

void SensorSession::persist() const {
  const auto text = canonical_json(identity_, state_, remote_public_present_,
                                   remote_public_xy_, key_fingerprint_,
                                   send_counter_, receive_counter_, join_format_,
                                   pending_event_id_);
  atomic_write(output_directory_ / "session.json",
               std::span<const std::uint8_t>(
                   reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
               0600);
}

void SensorSession::mark_join_sent() {
  if (state_ == SensorSessionState::JoinSent) return;
  if (state_ != SensorSessionState::Created) {
    throw std::logic_error("join can only start from created");
  }
  state_ = SensorSessionState::JoinSent;
  persist();
}

void SensorSession::accept_join_response(const JoinResponse& response) {
  if (state_ == SensorSessionState::ResponseReceived) {
    if (remote_public_present_ && remote_public_xy_ == response.local_public_xy &&
        send_counter_ == response.local_counter &&
        receive_counter_ == response.remote_counter && join_format_ == response.format) {
      return;
    }
    throw std::logic_error("join response differs from the recorded response");
  }
  if (state_ != SensorSessionState::JoinSent) {
    throw std::logic_error("join response is out of sequence");
  }
  if ((response.format == 4U || response.format == 5U) &&
      response.remote_public_hash != public_key_hash4(identity_.public_xy)) {
    throw std::invalid_argument("join response is not bound to the synthetic sensor public key");
  }
  (void)p256_shared_x(private_scalar_, response.local_public_xy);
  remote_public_xy_ = response.local_public_xy;
  remote_public_present_ = true;
  send_counter_ = response.local_counter;
  receive_counter_ = response.remote_counter;
  join_format_ = response.format;
  state_ = SensorSessionState::ResponseReceived;
  persist();
}

void SensorSession::install_key_fingerprint(std::string fingerprint) {
  if (state_ != SensorSessionState::ResponseReceived) {
    throw std::logic_error("key derivation is out of sequence");
  }
  (void)from_hex<32>(fingerprint);
  const auto expected = session_key_fingerprint(derive_peer_key(
      p256_shared_x(private_scalar_, remote_public_xy_)));
  if (fingerprint != expected) {
    throw std::invalid_argument("derived key fingerprint does not match session ECDH");
  }
  key_fingerprint_ = std::move(fingerprint);
  state_ = SensorSessionState::KeyDerived;
  persist();
}

void SensorSession::mark_joined(const JoinSignature& signature) {
  if (state_ != SensorSessionState::KeyDerived) {
    throw std::logic_error("join completion is out of sequence");
  }
  if (signature.signature.empty() || signature.signature.size() > 255U) {
    throw std::invalid_argument("join signature is empty or oversized");
  }
  state_ = SensorSessionState::Joined;
  persist();
}

void SensorSession::mark_protected_request_sent() {
  if (state_ != SensorSessionState::Joined) {
    throw std::logic_error("protected request transmission is out of sequence");
  }
  send_counter_ = next_counter(send_counter_);
  persist();
}

void SensorSession::mark_event_sent(std::uint32_t event_id) {
  if (state_ != SensorSessionState::Joined) {
    throw std::logic_error("event transmission is out of sequence");
  }
  pending_event_id_ = event_id;
  send_counter_ = next_counter(send_counter_);
  state_ = SensorSessionState::EventSent;
  persist();
}

void SensorSession::accept_received_counter(std::uint16_t received_counter) {
  if (state_ != SensorSessionState::Joined &&
      state_ != SensorSessionState::EventSent &&
      state_ != SensorSessionState::Acknowledged) {
    throw std::logic_error("protected downlink is out of sequence");
  }
  if (!counter_is_acceptable(receive_counter_, received_counter)) {
    throw std::invalid_argument("protected downlink counter is outside the receive window");
  }
  receive_counter_ = next_counter(received_counter);
  persist();
}

void SensorSession::mark_acknowledged(const SenseAckRequest& acknowledgement,
                                      std::uint16_t received_counter) {
  if (state_ != SensorSessionState::EventSent) {
    throw std::logic_error("event acknowledgement is out of sequence");
  }
  if (!pending_event_id_ || *pending_event_id_ != acknowledgement.event_id) {
    throw std::invalid_argument("event acknowledgement ID does not match");
  }
  if (!counter_is_acceptable(receive_counter_, received_counter)) {
    throw std::invalid_argument("event acknowledgement counter is outside the receive window");
  }
  receive_counter_ = next_counter(received_counter);
  pending_event_id_.reset();
  state_ = SensorSessionState::Acknowledged;
  persist();
}

void SensorSession::complete_event() {
  if (state_ != SensorSessionState::EventSent &&
      state_ != SensorSessionState::Acknowledged) {
    throw std::logic_error("event completion is out of sequence");
  }
  pending_event_id_.reset();
  state_ = SensorSessionState::Joined;
  persist();
}

}  // namespace bh61::core
