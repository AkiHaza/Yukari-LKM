#include "service_filter.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <limits>
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
    }
  }
  return config;
}

FilterConfig ServiceFilter::loadConfig() {
  constexpr const char* kConfigPath =
      "/data/adb/modules/yukari-service-filter/config/targets.conf";
  struct stat info {};
  // The selector is security-sensitive.  Never accept a process environment
  // override or a writable config file supplied by an app.
  if (stat(kConfigPath, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != 0 ||
      (info.st_mode & 0022) != 0) {
    return FilterConfig{{}, {}, {"lineage"}};
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
    if (!token.empty() && candidate.find(token) != std::string::npos) return true;
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

