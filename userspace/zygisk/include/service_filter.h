#pragma once

#include <sys/types.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace yukari {

// Configuration is intentionally process local.  No service-manager state is
// changed by this class; it only decides which names may be returned to the
// current caller.
struct FilterConfig {
  // Exact Android UID allow list, including the user-id component.
  std::vector<uid_t> target_uids;
  // Comma separated process names.  An empty list disables name matching.
  std::vector<std::string> target_processes;
  // Case-insensitive tokens in service names that are hidden for a target.
  std::vector<std::string> hidden_tokens;
};

class ServiceFilter final {
 public:
  // Reads the legacy environment-based fixture configuration. Production
  // injection uses loadConfig(), which only accepts the root-owned module
  // configuration file.
  static FilterConfig configFromEnvironment();
  // Loads the optional line-oriented module config.  Unknown keys are ignored
  // so the file can be extended without breaking older binaries.
  static FilterConfig configFromFile(const char* path);
  // Reads the root-owned module configuration file. Invalid or writable files
  // disable caller matching rather than accepting app-controlled overrides.
  static FilterConfig loadConfig();

  explicit ServiceFilter(FilterConfig config);

  bool matchesCaller(uid_t uid, std::string_view process_name) const;
  bool shouldHide(std::string_view service_name) const;

  // Filters the newline-delimited output of "service list". Recognized
  // headers and indexed entries receive an updated count and contiguous
  // indexes; unrecognized lines retain their bytes and line endings.
  // Legacy unindexed name/descriptor lines are filtered without rewriting.
  std::string filterServiceList(std::string_view output) const;

 private:
  FilterConfig config_;
};

// C ABI used by the optional native hook and by service-proxy.  The returned
// buffer is allocated with malloc and must be released with
// yukari_filter_free().
extern "C" char* yukari_filter_service_list(const char* data, size_t size,
                                             uid_t uid,
                                             const char* process_name,
                                             size_t* out_size);
extern "C" void yukari_filter_free(void* data);

}  // namespace yukari

