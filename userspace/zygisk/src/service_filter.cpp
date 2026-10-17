#include "service_filter.h"
#include "yukari/control.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <fcntl.h>
#include <iterator>
#include <sys/ioctl.h>
#include <limits>
#include <sstream>
#include <sys/stat.h>
#include <utility>
#include <unistd.h>

namespace yukari {
namespace {

std::string lower(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

std::vector<std::string> split(std::string_view value) {
  std::vector<std::string> result;
  size_t start = 0;
  while (start <= value.size()) {
    const size_t comma = value.find(',', start);
    const size_t end = comma == std::string_view::npos ? value.size() : comma;
    std::string item(value.substr(start, end - start));
    const auto first = item.find_first_not_of(" \t\r\n");
    const auto last = item.find_last_not_of(" \t\r\n");
    if (first != std::string::npos) result.emplace_back(item.substr(first, last - first + 1));
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return result;
}

std::string readFile(const char* path) {
  if (path == nullptr) return {};
  std::ifstream input(path, std::ios::binary);
  std::string result;
  char buffer[4096];
  while (input.read(buffer, sizeof(buffer)) || input.gcount() != 0) {
    result.append(buffer, static_cast<size_t>(input.gcount()));
    if (result.size() > 16384) return {};
  }
  return result;
}

class JsonParser final {
 public:
  struct Field {
    std::string key;
    size_t start;
    size_t end;
  };

  explicit JsonParser(std::string_view text) : text_(text) {}

  bool parse() {
    skipWhitespace();
    if (pos_ >= text_.size() || text_[pos_] != '{' || !parseObject(true)) return false;
    skipWhitespace();
    return pos_ == text_.size();
  }

  bool valid() const { return valid_; }

  bool has(std::string_view key) const {
    return field(key).start != std::string_view::npos;
  }

  const std::vector<Field>& fields() const { return fields_; }

  std::string_view value(std::string_view key) const {
    const Field match = field(key);
    if (match.start == std::string_view::npos) return {};
    return text_.substr(match.start, match.end - match.start);
  }

 private:
  Field field(std::string_view key) const {
    for (const Field& field : fields_)
      if (field.key == key) return field;
    return {std::string(), std::string_view::npos, std::string_view::npos};
  }

  void skipWhitespace() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
  }

  bool parseValue(bool collect_fields = false) {
    skipWhitespace();
    if (pos_ >= text_.size()) return false;
    if (depth_ >= 8) return false;
    ++depth_;
    const bool result = parseValueBody(collect_fields);
    --depth_;
    return result;
  }

  bool parseValueBody(bool collect_fields) {
    switch (text_[pos_]) {
      case '{': return parseObject(collect_fields);
      case '[': return parseArray();
      case '"': return parseString();
      case 't': return consume("true");
      case 'f': return consume("false");
      case 'n': return consume("null");
      default: return parseNumber();
    }
  }

  bool consume(std::string_view value) {
    if (text_.substr(pos_, value.size()) != value) return false;
    pos_ += value.size();
    return true;
  }

  bool parseObject(bool collect_fields) {
    ++pos_;  // '{'
    skipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    for (;;) {
      skipWhitespace();
      std::string key;
      if (!parseString(&key)) return false;
      for (const Field& field : fields_)
        if (collect_fields && field.key == key) return false;
      skipWhitespace();
      if (pos_ >= text_.size() || text_[pos_] != ':') return false;
      ++pos_;
      skipWhitespace();
      const size_t value_start = pos_;
      if (!parseValue(false)) return false;
      size_t value_end = pos_;
      while (value_end > value_start && std::isspace(static_cast<unsigned char>(text_[value_end - 1])))
        --value_end;
      if (collect_fields) fields_.push_back({key, value_start, value_end});
      skipWhitespace();
      if (pos_ >= text_.size()) return false;
      if (text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      if (text_[pos_] != ',') return false;
      ++pos_;
    }
  }

  bool parseArray() {
    ++pos_;  // '['
    skipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    for (;;) {
      if (!parseValue()) return false;
      skipWhitespace();
      if (pos_ >= text_.size()) return false;
      if (text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      if (text_[pos_] != ',') return false;
      ++pos_;
    }
  }

  bool parseString(std::string* result = nullptr) {
    if (pos_ >= text_.size() || text_[pos_] != '"') return false;
    ++pos_;
    while (pos_ < text_.size()) {
      const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
      if (c == '"') return true;
      if (c < 0x20) return false;
      if (c != '\\') {
        if (c >= 0x80) valid_ = false;
        if (result != nullptr) result->push_back(static_cast<char>(c));
        continue;
      }
      if (pos_ >= text_.size()) return false;
      const char escaped = text_[pos_++];
      if (escaped == 'u') {
        unsigned int codepoint = 0;
        for (int i = 0; i < 4; ++i) {
          if (pos_ >= text_.size()) return false;
          const unsigned char digit = static_cast<unsigned char>(text_[pos_++]);
          if (!std::isxdigit(digit)) return false;
          codepoint <<= 4;
          if (digit >= '0' && digit <= '9') codepoint |= digit - '0';
          else if (digit >= 'a' && digit <= 'f') codepoint |= digit - 'a' + 10;
          else codepoint |= digit - 'A' + 10;
        }
        if (codepoint > 0x7f) valid_ = false;
        if (result != nullptr) result->push_back(static_cast<char>(codepoint));
      } else if (escaped == '"' || escaped == '\\' || escaped == '/') {
        if (result != nullptr) result->push_back(escaped);
      } else if (escaped == 'b' || escaped == 'f' || escaped == 'n' || escaped == 'r' ||
                 escaped == 't') {
        if (result != nullptr) {
          if (escaped == 'b') result->push_back('\b');
          else if (escaped == 'f') result->push_back('\f');
          else if (escaped == 'n') result->push_back('\n');
          else if (escaped == 'r') result->push_back('\r');
          else result->push_back('\t');
        }
      } else {
        return false;
      }
    }
    return false;
  }

  bool parseNumber() {
    const size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    if (pos_ >= text_.size()) return false;
    if (text_[pos_] == '0') {
      ++pos_;
    } else {
      if (!std::isdigit(static_cast<unsigned char>(text_[pos_]))) return false;
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      const size_t fraction = pos_;
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
      if (fraction == pos_) return false;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      const size_t exponent = pos_;
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
      if (exponent == pos_) return false;
    }
    return start != pos_;
  }

  std::string_view text_;
  size_t pos_ = 0;
  unsigned int depth_ = 0;
  bool valid_ = true;
  std::vector<Field> fields_;
};

bool jsonBoolValue(std::string_view text, bool* value) {
  if (text == "true") {
    *value = true;
    return true;
  }
  if (text == "false") {
    *value = false;
    return true;
  }
  return false;
}

std::string jsonStringValue(std::string_view text) {
  if (text.size() < 2 || text.front() != '"' || text.back() != '"') return {};
  std::string result;
  bool escaped = false;
  for (size_t pos = 1; pos + 1 < text.size(); ++pos) {
    const char c = text[pos];
    if (escaped) {
      if (c == '"' || c == '\\' || c == '/') result.push_back(c);
      else if (c == 'n') result.push_back('\n');
      else if (c == 'r') result.push_back('\r');
      else if (c == 't') result.push_back('\t');
      else return {};
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else if (c == '"') {
      return result;
    } else {
      result.push_back(c);
    }
  }
  return result;
}

template <typename Callback>
bool jsonArrayValue(std::string_view text, Callback callback) {
  if (text.size() < 2 || text.front() != '[' || text.back() != ']') return false;
  const size_t start = 0;
  size_t pos = start + 1;
  bool need_value = false;
  while (pos < text.size()) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    if (pos >= text.size()) return false;
    if (text[pos] == ']') return !need_value;
    if (need_value) need_value = false;
    if (text[pos] == '"') {
      std::string value;
      bool escaped = false;
      ++pos;
      for (; pos < text.size(); ++pos) {
        const char c = text[pos];
        if (escaped) {
          if (c == '"' || c == '\\' || c == '/') value.push_back(c);
          else if (c == 'n') value.push_back('\n');
          else if (c == 'r') value.push_back('\r');
          else if (c == 't') value.push_back('\t');
          else return false;
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '"') {
          ++pos;
          callback(value, true);
          break;
        } else {
          value.push_back(c);
        }
      }
    } else {
      const size_t begin = pos;
      while (pos < text.size() && text[pos] != ',' && text[pos] != ']') ++pos;
      size_t end = pos;
      while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
      if (begin == end) return false;
      callback(std::string(text.substr(begin, end - begin)), false);
    }
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    if (pos < text.size() && text[pos] == ']') return true;
    if (pos >= text.size() || text[pos] != ',') return false;
    ++pos;
    need_value = true;
  }
  return false;
}

void trim(std::string* value);
bool parseUid(std::string_view value, uid_t* out);

bool trustedRegularFile(const char* path) {
  struct stat info {};
  return path != nullptr && lstat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == 0 &&
         (info.st_mode & 0022) == 0;
}

void resolvePackages(FilterConfig* config, std::string_view source_json) {
  if (config == nullptr || config->target_packages.empty()) return;
  std::string line;
  if (geteuid() == 0) {
    std::ifstream input("/data/system/packages.list");
    while (std::getline(input, line)) {
      std::istringstream fields(line);
      std::string package;
      unsigned long uid = 0;
      if (!(fields >> package >> uid)) continue;
      if (std::find(config->target_packages.begin(), config->target_packages.end(), package) ==
          config->target_packages.end())
        continue;
      if (uid <= static_cast<unsigned long>(std::numeric_limits<uid_t>::max()))
        config->target_uids.push_back(static_cast<uid_t>(uid));
    }
  }
  if (geteuid() == 0) {
    std::sort(config->target_uids.begin(), config->target_uids.end());
    config->target_uids.erase(std::unique(config->target_uids.begin(), config->target_uids.end()),
                              config->target_uids.end());
    return;
  }
  // The root provisioning helper publishes resolved package UIDs for injected
  // app processes that cannot read /data/system/packages.list directly.
  const char* sidecars[] = {"/data/adb/modules/Yukari/config.uids",
                            "/data/adb/modules/yukari-service-filter/config.uids"};
  for (const char* sidecar : sidecars) {
    if (!trustedRegularFile(sidecar)) continue;
    std::ifstream resolved(sidecar);
    std::string header;
    std::getline(resolved, header);
    constexpr std::string_view prefix = "YUKARI_RESOLVED_V1 ";
    if (header.compare(0, prefix.size(), prefix) != 0) continue;
    size_t json_size = 0;
    const std::string size_text = header.substr(prefix.size());
    const auto parsed = std::from_chars(size_text.data(), size_text.data() + size_text.size(),
                                        json_size, 10);
    if (parsed.ec != std::errc() || parsed.ptr != size_text.data() + size_text.size() ||
        json_size > 16384 || json_size != source_json.size()) continue;
    std::string cached_json(json_size, '\0');
    resolved.read(cached_json.data(), static_cast<std::streamsize>(cached_json.size()));
    if (cached_json != source_json) continue;
    if (resolved.peek() == '\n') resolved.get();
    while (std::getline(resolved, line)) {
      trim(&line);
      uid_t uid = 0;
      if (parseUid(line, &uid)) config->target_uids.push_back(uid);
    }
  }
  std::sort(config->target_uids.begin(), config->target_uids.end());
  config->target_uids.erase(std::unique(config->target_uids.begin(), config->target_uids.end()),
                            config->target_uids.end());
}

bool validJsonSchema(const JsonParser& parser) {
  bool valid = true;
  if (parser.has("target_uids") && parser.value("target_uids").size() > 4096) valid = false;
  if (parser.has("target_packages") && parser.value("target_packages").size() > 4096) valid = false;
  if (parser.has("filter_patterns") && parser.value("filter_patterns").size() > 4096) valid = false;
  constexpr std::string_view known[] = {"target_uids", "target_packages", "filter_patterns",
                                        "match_mode", "kernel_hook_enabled",
                                        "userspace_hook_enabled"};
  for (const JsonParser::Field& field : parser.fields()) {
    bool recognized = false;
    for (const std::string_view key : known)
      if (field.key == key) recognized = true;
    if (!recognized) valid = false;
  }
  for (const std::string_view key : {std::string_view("target_uids"),
                                     std::string_view("target_packages"),
                                     std::string_view("filter_patterns")}) {
    const std::string_view value = parser.value(key);
    if (parser.has(key) && (value.empty() || value.front() != '[')) valid = false;
  }
  if (parser.has("target_uids") &&
      !jsonArrayValue(parser.value("target_uids"),
                      [&valid](const std::string& value, bool string_value) {
                        uid_t uid = 0;
                        if (string_value || !parseUid(value, &uid)) valid = false;
                      }))
    valid = false;
  if (parser.has("target_packages") &&
      !jsonArrayValue(parser.value("target_packages"),
                      [&valid](const std::string& value, bool string_value) {
                        if (!string_value || value.empty()) valid = false;
                        for (const unsigned char c : value)
                          if (c < 0x20 || c >= 0x7f) valid = false;
                      }))
    valid = false;
  if (parser.has("filter_patterns") &&
      !jsonArrayValue(parser.value("filter_patterns"),
                      [&valid](const std::string& value, bool string_value) {
                        if (!string_value || value.empty()) valid = false;
                        for (const unsigned char c : value)
                          if (c < 0x20 || c >= 0x7f) valid = false;
                      }))
    valid = false;
  if (parser.has("match_mode")) {
    const std::string_view value = parser.value("match_mode");
    if (value.empty() || value.front() != '"') valid = false;
    const std::string mode = jsonStringValue(value);
    if (mode != "prefix" && mode != "substring" && mode != "exact") valid = false;
  }
  for (const std::string_view key : {std::string_view("kernel_hook_enabled"),
                                     std::string_view("userspace_hook_enabled")}) {
    if (parser.has(key)) {
      bool ignored = false;
      if (!jsonBoolValue(parser.value(key), &ignored)) valid = false;
    }
  }
  return valid;
}

void trim(std::string* value) {
  const auto first = value->find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    value->clear();
    return;
  }
  const auto last = value->find_last_not_of(" \t\r\n");
  *value = value->substr(first, last - first + 1);
}

bool parseUid(std::string_view value, uid_t* out) {
  if (value.empty()) return false;
  for (const unsigned char c : value)
    if (c < '0' || c > '9') return false;
  char* end = nullptr;
  const std::string copy(value);
  errno = 0;
  const unsigned long parsed = std::strtoul(copy.c_str(), &end, 10);
  if (errno != 0 || end == copy.c_str() || *end != '\0' ||
      parsed > static_cast<unsigned long>(std::numeric_limits<uid_t>::max()))
    return false;
  *out = static_cast<uid_t>(parsed);
  return true;
}

std::string serviceName(std::string_view line) {
  // Android's service tool emits "name: [descriptor]".  Binder debug builds
  // may emit "name (descriptor)"; accepting either format keeps filtering
  // independent of the service binary version.
  const size_t colon = line.find(':');
  const size_t paren = line.find('(');
  if (colon == std::string_view::npos && paren == std::string_view::npos) return {};
  size_t end = std::min(colon == std::string_view::npos ? line.size() : colon,
                        paren == std::string_view::npos ? line.size() : paren);
  if (end == std::string_view::npos) end = line.size();
  while (end > 0 && std::isspace(static_cast<unsigned char>(line[end - 1]))) --end;
  size_t begin = 0;
  while (begin < end && std::isspace(static_cast<unsigned char>(line[begin]))) ++begin;
  for (size_t i = begin; i < end; ++i) {
    const unsigned char c = static_cast<unsigned char>(line[i]);
    if (c <= 0x20 || c >= 0x7f) return {};
  }
  return std::string(line.substr(begin, end - begin));
}

std::string_view lineBody(std::string_view line) {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  return line;
}

bool parseServiceHeader(std::string_view line) {
  line = lineBody(line);
  constexpr std::string_view prefix = "Found ";
  constexpr std::string_view suffix = " services:";
  if (line.size() <= prefix.size() + suffix.size() ||
      line.substr(0, prefix.size()) != prefix ||
      line.substr(line.size() - suffix.size()) != suffix)
    return false;

  const std::string_view count = line.substr(prefix.size(),
                                              line.size() - prefix.size() - suffix.size());
  if (count.empty()) return false;
  size_t parsed = 0;
  const auto result = std::from_chars(count.data(), count.data() + count.size(), parsed, 10);
  return result.ec == std::errc() && result.ptr == count.data() + count.size();
}

struct IndexedServiceLine {
  size_t index_start;
  size_t index_end;
  size_t service_start;
  std::string service_name;
};

bool parseIndexedServiceLine(std::string_view line, IndexedServiceLine* parsed) {
  const std::string_view body = lineBody(line);
  size_t index_start = 0;
  while (index_start < body.size() &&
         (body[index_start] == ' ' || body[index_start] == '\t'))
    ++index_start;
  const size_t digits_start = index_start;
  while (index_start < body.size() &&
         std::isdigit(static_cast<unsigned char>(body[index_start])))
    ++index_start;
  if (index_start == digits_start || index_start == body.size() ||
      !std::isspace(static_cast<unsigned char>(body[index_start])))
    return false;
  size_t index = 0;
  const auto index_result = std::from_chars(body.data() + digits_start,
                                            body.data() + index_start, index, 10);
  if (index_result.ec != std::errc()) return false;
  (void)index;

  size_t service_start = index_start;
  while (service_start < body.size() &&
         std::isspace(static_cast<unsigned char>(body[service_start])))
    ++service_start;
  if (service_start == body.size()) return false;
  const std::string name = serviceName(body.substr(service_start));
  if (name.empty()) return false;
  if (parsed != nullptr) {
    parsed->index_start = digits_start;
    parsed->index_end = index_start;
    parsed->service_start = service_start;
    parsed->service_name = name;
  }
  return true;
}

}  // namespace

FilterConfig ServiceFilter::configFromEnvironment() {
  FilterConfig config;
  const char* uid_text = std::getenv("YUKARI_TARGET_UIDS");
  if (uid_text != nullptr) {
    for (const std::string& item : split(uid_text)) {
      uid_t uid = 0;
      if (parseUid(item, &uid)) config.target_uids.push_back(uid);
    }
  }
  const char* process_text = std::getenv("YUKARI_TARGET_PROCESSES");
  if (process_text != nullptr) config.target_processes = split(process_text);

  const char* token_text = std::getenv("YUKARI_HIDE_TOKENS");
  config.hidden_tokens = token_text == nullptr ? std::vector<std::string>{"lineage"}
                                                 : split(token_text);
  for (std::string& token : config.hidden_tokens) token = lower(token);
  return config;
}

FilterConfig ServiceFilter::configFromFile(const char* path) {
  FilterConfig config;
  config.hidden_tokens = {"lineage"};
  if (path == nullptr) return config;
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line)) {
    trim(&line);
    if (line.empty() || line[0] == '#') continue;
    const size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    std::string key = line.substr(0, equals);
    std::string value = line.substr(equals + 1);
    trim(&key);
    trim(&value);
    if (key == "uids") {
      for (const std::string& item : split(value)) {
        uid_t uid = 0;
        if (parseUid(item, &uid)) config.target_uids.push_back(uid);
      }
    } else if (key == "processes") {
      config.target_processes = split(value);
    } else if (key == "hide_tokens") {
      config.hidden_tokens = split(value);
      for (std::string& token : config.hidden_tokens) token = lower(token);
    } else if (key == "match_mode") {
      if (value == "prefix") config.match_mode = FilterConfig::MatchMode::kPrefix;
      else if (value == "exact") config.match_mode = FilterConfig::MatchMode::kExact;
      else if (value == "substring") config.match_mode = FilterConfig::MatchMode::kSubstring;
    }
  }
  return config;
}

FilterConfig ServiceFilter::configFromJsonFile(const char* path) {
  FilterConfig config;
  config.hidden_tokens = {"lineage"};
  // New JSON configurations must explicitly enable each hook. This keeps a
  // partially provisioned file from changing process behavior.
  config.kernel_hook_enabled = false;
  config.userspace_hook_enabled = false;
  const std::string text = readFile(path);
  JsonParser parser(text);
  if (text.empty() || !parser.parse() || !parser.valid() || !validJsonSchema(parser)) return config;

  jsonArrayValue(parser.value("target_uids"), [&config](const std::string& value, bool string_value) {
    if (string_value) return;
    uid_t uid = 0;
    if (parseUid(value, &uid)) config.target_uids.push_back(uid);
  });
  jsonArrayValue(parser.value("target_packages"), [&config](const std::string& value, bool string_value) {
    if (string_value && !value.empty()) config.target_packages.push_back(value);
  });
  const bool has_patterns = parser.has("filter_patterns");
  if (has_patterns) config.hidden_tokens.clear();
  jsonArrayValue(parser.value("filter_patterns"), [&config](const std::string& value, bool string_value) {
    if (string_value && !value.empty()) config.hidden_tokens.push_back(lower(value));
  });
  const std::string mode = jsonStringValue(parser.value("match_mode"));
  if (mode == "prefix") config.match_mode = FilterConfig::MatchMode::kPrefix;
  else if (mode == "exact") config.match_mode = FilterConfig::MatchMode::kExact;
  else if (mode == "substring" || mode.empty())
    config.match_mode = FilterConfig::MatchMode::kSubstring;
  (void)jsonBoolValue(parser.value("kernel_hook_enabled"), &config.kernel_hook_enabled);
  (void)jsonBoolValue(parser.value("userspace_hook_enabled"), &config.userspace_hook_enabled);
  resolvePackages(&config, text);
  std::sort(config.target_uids.begin(), config.target_uids.end());
  config.target_uids.erase(std::unique(config.target_uids.begin(), config.target_uids.end()),
                           config.target_uids.end());
  if (config.target_uids.size() > YUKARI_MAX_TARGET_UIDS ||
      config.target_packages.size() > YUKARI_MAX_TARGET_UIDS ||
      config.hidden_tokens.size() > YUKARI_MAX_FILTER_PATTERNS) {
    config.target_uids.clear();
    config.kernel_hook_enabled = false;
    config.userspace_hook_enabled = false;
  }
  for (const std::string& pattern : config.hidden_tokens) {
    if (pattern.empty() || pattern.size() >= YUKARI_FILTER_PATTERN_SIZE) {
      config.target_uids.clear();
      config.kernel_hook_enabled = false;
      config.userspace_hook_enabled = false;
    }
  }
  return config;
}

bool ServiceFilter::syncKernelConfig(const FilterConfig& config) {
  const bool shape_valid =
      config.target_uids.size() <= YUKARI_MAX_TARGET_UIDS &&
      config.hidden_tokens.size() <= YUKARI_MAX_FILTER_PATTERNS &&
      static_cast<unsigned int>(config.match_mode) <= YUKARI_MATCH_EXACT;
  const int fd = open(YUKARI_CONTROL_DEVICE, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const auto clear = [fd]() {
    (void)ioctl(fd, YUKARI_IOCTL_CLEAR_CONFIG);
  };
  if (!shape_valid) {
    clear();
    close(fd);
    return false;
  }
  for (const std::string& pattern : config.hidden_tokens) {
    if (pattern.empty() || pattern.size() >= YUKARI_FILTER_PATTERN_SIZE) {
      clear();
      close(fd);
      return false;
    }
    for (const unsigned char c : pattern)
      if (c < 0x21 || c > 0x7e) {
        clear();
        close(fd);
        return false;
      }
  }
  yukari_control_config control{};
  control.version = YUKARI_CONTROL_VERSION;
  if (config.kernel_hook_enabled) control.flags |= YUKARI_CONTROL_FLAG_KERNEL_ENABLED;
  if (config.userspace_hook_enabled) control.flags |= YUKARI_CONTROL_FLAG_USERSPACE_ENABLED;
  control.match_mode = static_cast<yukari_u32>(config.match_mode);
  control.uid_count = static_cast<yukari_u32>(config.target_uids.size());
  for (size_t i = 0; i < control.uid_count; ++i)
    control.target_uids[i] = static_cast<yukari_u32>(config.target_uids[i]);
  control.pattern_count = static_cast<yukari_u32>(config.hidden_tokens.size());
  for (size_t i = 0; i < control.pattern_count; ++i) {
    const std::string& pattern = config.hidden_tokens[i];
    const size_t length = pattern.size();
    std::memcpy(control.patterns[i], pattern.data(), length);
    control.patterns[i][length] = '\0';
  }
  const bool ok = ioctl(fd, YUKARI_IOCTL_SET_CONFIG, &control) == 0;
  if (!ok) clear();
  close(fd);
  return ok;
}

FilterConfig ServiceFilter::loadConfig() {
  constexpr const char* kJsonConfigPath = "/data/adb/modules/Yukari/config.json";
  constexpr const char* kJsonFallbackPath =
      "/data/adb/modules/yukari-service-filter/config.json";
  constexpr const char* kConfigPath =
      "/data/adb/modules/yukari-service-filter/config/targets.conf";
  struct stat info {};
  // The selector is security-sensitive.  Never accept a process environment
  // override or a writable config file supplied by an app.
  if (stat(kJsonConfigPath, &info) == 0) {
    if (!trustedRegularFile(kJsonConfigPath)) return FilterConfig{{}, {}, {"lineage"}, {}};
    return configFromJsonFile(kJsonConfigPath);
  }
  if (trustedRegularFile(kJsonFallbackPath)) {
    return configFromJsonFile(kJsonFallbackPath);
  }
  if (!trustedRegularFile(kConfigPath)) {
    return FilterConfig{{}, {}, {"lineage"}, {}};
  }
  FilterConfig config = configFromFile(kConfigPath);
  // A UID is the only identity value that survives the proxy child boundary
  // without trusting app-controlled process metadata. Process names further
  // narrow a UID match; they cannot authorize filtering on their own.
  if (config.target_uids.empty()) config.target_processes.clear();
  return config;
}

ServiceFilter::ServiceFilter(FilterConfig config) : config_(std::move(config)) {}

bool ServiceFilter::matchesCaller(uid_t uid, std::string_view process_name) const {
  if (!config_.userspace_hook_enabled) return false;
  // Android encodes a per-user appId modulo AID_USER_OFFSET (100000).
  // Match only application appIds so a broad
  // configuration cannot affect system/core UIDs for any user.
  constexpr uid_t kAndroidUserOffset = 100000;
  constexpr uid_t kApplicationUidStart = 10000;
  if ((uid % kAndroidUserOffset) < kApplicationUidStart) return false;
  // Both selectors are allow lists when present.  This avoids accidentally
  // filtering every process when only one identity half was configured.
  const bool uid_match = config_.target_uids.empty() ||
                         std::find(config_.target_uids.begin(), config_.target_uids.end(), uid) !=
                             config_.target_uids.end();
  const bool process_match = config_.target_processes.empty() ||
                             std::find(config_.target_processes.begin(),
                                       config_.target_processes.end(), process_name) !=
                                 config_.target_processes.end();
  return uid_match && process_match && (!config_.target_uids.empty() ||
                                        !config_.target_processes.empty());
}

bool ServiceFilter::shouldHide(std::string_view service_name) const {
  const std::string candidate = lower(service_name);
  for (const std::string& token : config_.hidden_tokens) {
    if (token.empty()) continue;
    if (config_.match_mode == FilterConfig::MatchMode::kPrefix && candidate.rfind(token, 0) == 0)
      return true;
    if (config_.match_mode == FilterConfig::MatchMode::kExact && candidate == token) return true;
    if (config_.match_mode == FilterConfig::MatchMode::kSubstring &&
        candidate.find(token) != std::string::npos)
      return true;
  }
  return false;
}

std::string ServiceFilter::filterServiceList(std::string_view output) const {
  std::string result;
  result.reserve(output.size());

  bool has_header = false;
  size_t service_count = 0;
  size_t header_start = std::string_view::npos;
  size_t start = 0;
  while (start < output.size()) {
    const size_t newline = output.find('\n', start);
    const size_t end = newline == std::string_view::npos ? output.size() : newline;
    if (parseServiceHeader(output.substr(start, end - start))) {
      has_header = true;
      header_start = start;
      break;
    }
    if (newline == std::string_view::npos) break;
    start = newline + 1;
  }

  if (has_header) {
    const size_t header_end = output.find('\n', header_start);
    start = header_end == std::string_view::npos ? output.size() : header_end + 1;
    while (start < output.size()) {
      const size_t newline = output.find('\n', start);
      const size_t end = newline == std::string_view::npos ? output.size() : newline;
      IndexedServiceLine indexed{};
      if (parseIndexedServiceLine(output.substr(start, end - start), &indexed) &&
          !shouldHide(indexed.service_name))
        ++service_count;
      if (newline == std::string_view::npos) break;
      start = newline + 1;
    }
  }

  size_t next_index = 0;
  bool header_seen = !has_header;
  start = 0;
  while (start < output.size()) {
    const size_t newline = output.find('\n', start);
    const size_t end = newline == std::string_view::npos ? output.size() : newline;
    const std::string_view line = output.substr(start, end - start);
    const std::string_view body = lineBody(line);
    if (has_header && parseServiceHeader(line)) {
      header_seen = true;
      result.append("Found ");
      result.append(std::to_string(service_count));
      result.append(" services:");
      if (body.size() != line.size()) result.push_back('\r');
    } else {
      IndexedServiceLine indexed{};
      const bool is_indexed = header_seen && parseIndexedServiceLine(line, &indexed);
      const bool hide = is_indexed ? shouldHide(indexed.service_name)
                                   : (!has_header && shouldHide(serviceName(line)));
      if (hide) {
        if (newline == std::string_view::npos) break;
        start = newline + 1;
        continue;
      }
      if (has_header && is_indexed) {
        result.append(line.substr(0, indexed.index_start));
        result.append(std::to_string(next_index++));
        result.append(line.substr(indexed.index_end, indexed.service_start - indexed.index_end));
        result.append(body.substr(indexed.service_start));
        if (body.size() != line.size()) result.push_back('\r');
      } else {
        result.append(line);
      }
    }
    if (newline != std::string_view::npos) result.push_back('\n');
    if (newline == std::string_view::npos) break;
    start = newline + 1;
  }
  return result;
}

extern "C" char* yukari_filter_service_list(const char* data, size_t size, uid_t uid,
                                             const char* process_name, size_t* out_size) {
  if (out_size == nullptr || data == nullptr) return nullptr;
  const FilterConfig config = ServiceFilter::loadConfig();
  ServiceFilter filter(config);
  const std::string process = process_name == nullptr ? std::string() : process_name;
  if (!filter.matchesCaller(uid, process)) {
    char* copy = static_cast<char*>(std::malloc(size));
    if (copy == nullptr && size != 0) return nullptr;
    if (size != 0) std::memcpy(copy, data, size);
    *out_size = size;
    return copy;
  }
  const std::string filtered = filter.filterServiceList(std::string_view(data, size));
  char* result = static_cast<char*>(std::malloc(filtered.size()));
  if (result == nullptr && !filtered.empty()) return nullptr;
  if (!filtered.empty()) std::memcpy(result, filtered.data(), filtered.size());
  *out_size = filtered.size();
  return result;
}

extern "C" void yukari_filter_free(void* data) { std::free(data); }

}  // namespace yukari

