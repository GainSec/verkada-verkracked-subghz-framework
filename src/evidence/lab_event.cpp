#include "bh61/evidence/lab_event.hpp"

#include <charconv>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace bh61::evidence {
namespace {

class JsonCursor {
 public:
  explicit JsonCursor(std::string_view input) : input_(input) {}

  void whitespace() {
    while (offset_ < input_.size() &&
           std::isspace(static_cast<unsigned char>(input_[offset_]))) {
      ++offset_;
    }
  }

  void expect(char value) {
    whitespace();
    if (offset_ >= input_.size() || input_[offset_] != value) {
      throw std::invalid_argument("invalid lab event JSON");
    }
    ++offset_;
  }

  auto string() -> std::string {
    whitespace();
    if (offset_ >= input_.size() || input_[offset_++] != '"') {
      throw std::invalid_argument("JSON string required");
    }
    std::string result;
    while (offset_ < input_.size()) {
      const auto value = input_[offset_++];
      if (value == '"') return result;
      if (value == '\\') {
        if (offset_ >= input_.size()) throw std::invalid_argument("bad escape");
        const auto escaped = input_[offset_++];
        switch (escaped) {
          case '"': result.push_back('"'); break;
          case '\\': result.push_back('\\'); break;
          case '/': result.push_back('/'); break;
          case 'b': result.push_back('\b'); break;
          case 'f': result.push_back('\f'); break;
          case 'n': result.push_back('\n'); break;
          case 'r': result.push_back('\r'); break;
          case 't': result.push_back('\t'); break;
          default: throw std::invalid_argument("unsupported JSON escape");
        }
      } else {
        if (static_cast<unsigned char>(value) < 0x20U) {
          throw std::invalid_argument("control byte in JSON string");
        }
        result.push_back(value);
      }
    }
    throw std::invalid_argument("unterminated JSON string");
  }

  auto raw_value() -> std::string {
    whitespace();
    const auto start = offset_;
    consume_value();
    return std::string(input_.substr(start, offset_ - start));
  }

  auto done() -> bool {
    whitespace();
    return offset_ == input_.size();
  }

  auto peek() -> char {
    whitespace();
    return offset_ < input_.size() ? input_[offset_] : '\0';
  }

 private:
  void consume_value() {
    whitespace();
    if (offset_ >= input_.size()) throw std::invalid_argument("value required");
    if (input_[offset_] == '"') {
      (void)string();
      return;
    }
    if (input_[offset_] == '{' || input_[offset_] == '[') {
      const auto open = input_[offset_++];
      const auto close = open == '{' ? '}' : ']';
      whitespace();
      if (offset_ < input_.size() && input_[offset_] == close) {
        ++offset_;
        return;
      }
      while (true) {
        if (open == '{') {
          (void)string();
          expect(':');
        }
        consume_value();
        whitespace();
        if (offset_ < input_.size() && input_[offset_] == close) {
          ++offset_;
          return;
        }
        expect(',');
      }
    }
    const auto start = offset_;
    while (offset_ < input_.size() && input_[offset_] != ',' &&
           input_[offset_] != '}' && input_[offset_] != ']' &&
           !std::isspace(static_cast<unsigned char>(input_[offset_]))) {
      ++offset_;
    }
    if (offset_ == start) throw std::invalid_argument("invalid JSON literal");
  }

  std::string_view input_;
  std::size_t offset_{};
};

auto fields(std::string_view json) -> std::map<std::string, std::string> {
  JsonCursor cursor(json);
  std::map<std::string, std::string> result;
  cursor.expect('{');
  if (cursor.peek() == '}') {
    cursor.expect('}');
  } else {
    while (true) {
      const auto key = cursor.string();
      cursor.expect(':');
      if (!result.emplace(key, cursor.raw_value()).second) {
        throw std::invalid_argument("duplicate lab event field");
      }
      if (cursor.peek() == '}') {
        cursor.expect('}');
        break;
      }
      cursor.expect(',');
    }
  }
  if (!cursor.done()) throw std::invalid_argument("trailing JSON data");
  return result;
}

auto decode_json_string(std::string_view raw) -> std::string {
  JsonCursor cursor(raw);
  const auto result = cursor.string();
  if (!cursor.done()) throw std::invalid_argument("string field malformed");
  return result;
}

auto required_string(std::map<std::string, std::string>& source,
                     std::string_view name) -> std::string {
  const auto found = source.find(std::string(name));
  if (found == source.end()) throw std::invalid_argument("required field absent");
  auto result = decode_json_string(found->second);
  source.erase(found);
  if (result.empty() || result.find_first_of("\r\n\0") != std::string::npos) {
    throw std::invalid_argument("invalid string field");
  }
  return result;
}

auto optional_string(std::map<std::string, std::string>& source,
                     std::string_view name) -> std::string {
  const auto found = source.find(std::string(name));
  if (found == source.end()) return {};
  auto result = decode_json_string(found->second);
  source.erase(found);
  if (result.find_first_of("\r\n\0") != std::string::npos) {
    throw std::invalid_argument("invalid optional string field");
  }
  return result;
}

auto escape(std::string_view value) -> std::string {
  std::ostringstream out;
  for (const auto ch : value) {
    switch (ch) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << ch;
    }
  }
  return out.str();
}

void validate(const LabEvent& event) {
  if (event.event_id.empty() || event.source.empty() || event.type.empty()) {
    throw std::invalid_argument("lab event required field is empty");
  }
  if (event.utc.size() < 20U || event.utc[4] != '-' || event.utc[10] != 'T' ||
      (event.utc.back() != 'Z' && event.utc.find('+', 19U) == std::string::npos &&
       event.utc.find('-', 19U) == std::string::npos)) {
    throw std::invalid_argument("lab event UTC is not RFC3339");
  }
  if (event.direction != "inbound" && event.direction != "outbound" &&
      event.direction != "internal") {
    throw std::invalid_argument("invalid lab event direction");
  }
  const auto payload_fields = fields(event.payload_json);
  (void)payload_fields;
}

}  // namespace

auto parse_lab_event(std::string_view json) -> LabEvent {
  auto source = fields(json);
  if (required_string(source, "schema") != lab_event_schema) {
    throw std::invalid_argument("unsupported lab event schema");
  }
  LabEvent event;
  event.event_id = required_string(source, "event_id");
  event.utc = required_string(source, "utc");
  event.source = required_string(source, "source");
  event.device_id = optional_string(source, "device_id");
  event.sensor_id = optional_string(source, "sensor_id");
  event.correlation_id = optional_string(source, "correlation_id");
  event.type = required_string(source, "type");
  event.direction = required_string(source, "direction");
  if (const auto found = source.find("monotonic_ns"); found != source.end()) {
    std::uint64_t parsed{};
    const auto result = std::from_chars(found->second.data(),
                                        found->second.data() + found->second.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != found->second.data() + found->second.size()) {
      throw std::invalid_argument("invalid monotonic_ns");
    }
    event.monotonic_ns = parsed;
    source.erase(found);
  }
  if (const auto found = source.find("evidence_refs"); found != source.end()) {
    JsonCursor cursor(found->second);
    cursor.expect('[');
    if (cursor.peek() != ']') {
      while (true) {
        event.evidence_refs.push_back(cursor.string());
        if (cursor.peek() == ']') break;
        cursor.expect(',');
      }
    }
    cursor.expect(']');
    if (!cursor.done()) throw std::invalid_argument("invalid evidence_refs");
    source.erase(found);
  }
  const auto payload = source.find("payload");
  if (payload == source.end() || payload->second.empty() || payload->second.front() != '{') {
    throw std::invalid_argument("object payload required");
  }
  event.payload_json = payload->second;
  source.erase(payload);
  event.extensions = std::move(source);
  validate(event);
  return event;
}

auto serialize_lab_event(const LabEvent& event) -> std::string {
  validate(event);
  std::ostringstream out;
  out << "{\"schema\":\"" << lab_event_schema << "\",\"event_id\":\""
      << escape(event.event_id) << "\",\"utc\":\"" << escape(event.utc) << '"';
  if (event.monotonic_ns) out << ",\"monotonic_ns\":" << *event.monotonic_ns;
  out << ",\"source\":\"" << escape(event.source) << '"';
  if (!event.device_id.empty()) out << ",\"device_id\":\"" << escape(event.device_id) << '"';
  if (!event.sensor_id.empty()) out << ",\"sensor_id\":\"" << escape(event.sensor_id) << '"';
  if (!event.correlation_id.empty()) out << ",\"correlation_id\":\"" << escape(event.correlation_id) << '"';
  out << ",\"type\":\"" << escape(event.type) << "\",\"direction\":\""
      << escape(event.direction) << '"';
  if (!event.evidence_refs.empty()) {
    out << ",\"evidence_refs\":[";
    for (std::size_t i = 0; i < event.evidence_refs.size(); ++i) {
      if (i != 0U) out << ',';
      out << '"' << escape(event.evidence_refs[i]) << '"';
    }
    out << ']';
  }
  out << ",\"payload\":" << event.payload_json;
  for (const auto& [key, value] : event.extensions) {
    out << ",\"" << escape(key) << "\":" << value;
  }
  out << '}';
  return out.str();
}

}  // namespace bh61::evidence
