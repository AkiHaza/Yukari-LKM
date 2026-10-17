#include "binder_hooks.h"

#include "yukari/parcel.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#if __has_include(<zygisk.hpp>)
#include <zygisk.hpp>
#define YUKARI_HAS_ZYGISK_API 1
#else
#define YUKARI_HAS_ZYGISK_API 0
#endif

#if __has_include(<jni.h>)
#include <jni.h>
#define YUKARI_HAS_JNI 1
#else
#define YUKARI_HAS_JNI 0
#endif

namespace yukari {
namespace {

// The symbol is intentionally resolved by name. Parcel is an opaque C++
// object here; no libbinder layout is duplicated in the module.
using TransactFn = int32_t (*)(void*, int32_t, uint32_t, const void*, void*, uint32_t);
using ParcelDataFn = const void* (*)(const void*);
using ParcelSizeFn = size_t (*)(const void*);
using ParcelObjectsFn = size_t (*)(const void*);
using ParcelSetDataFn = int32_t (*)(void*, const void*, size_t);
using SystemPropertyGetFn = int (*)(const char*, char*);

// Keep liblog optional.  Zygisk processes normally already load liblog, but
// a missing symbol must never prevent the hook from loading or change Binder
// behavior.  The stderr fallback still makes a failed self-check observable
// in stripped-down test processes.
extern "C" int __android_log_write(int, const char*, const char*) __attribute__((weak));

constexpr char kTransactSymbol[] =
    "_ZN7android14IPCThreadState8transactEijRKNS_6ParcelEPS1_j";
constexpr char kParcelDataSymbol[] = "_ZNK7android6Parcel4dataEv";
constexpr char kParcelSizeSymbol[] = "_ZNK7android6Parcel8dataSizeEv";
constexpr char kParcelObjectsSymbol[] = "_ZNK7android6Parcel12objectsCountEv";
// size_t is `unsigned long` on LP64 (`m`) and `unsigned int` on the 32-bit
// Android ABI (`j`), so the parameter type is part of the mangled name.
constexpr char kParcelSetDataSymbol64[] = "_ZN7android6Parcel7setDataEPKhm";
constexpr char kParcelSetDataSymbol32[] = "_ZN7android6Parcel7setDataEPKhj";
constexpr char kServiceManagerDescriptor[] = "android.os.IServiceManager";

TransactFn g_original_transact = nullptr;
ParcelDataFn g_parcel_data = nullptr;
ParcelSizeFn g_parcel_size = nullptr;
ParcelObjectsFn g_parcel_objects = nullptr;
ParcelSetDataFn g_parcel_set_data = nullptr;
std::vector<std::string> g_hidden_tokens;
std::vector<const char*> g_hidden_token_ptrs;
unsigned int g_match_mode = YUKARI_PARCEL_MATCH_SUBSTRING;
std::atomic<BinderHookSelfCheckState> g_self_check_state{
    BinderHookSelfCheckState::kDisabled};
std::atomic<bool> g_self_check_done{false};
thread_local bool g_in_hook = false;

constexpr char kLogTag[] = "YukariBinder";

const char* selfCheckStateName(BinderHookSelfCheckState state) {
  switch (state) {
    case BinderHookSelfCheckState::kDisabled:
      return "disabled";
    case BinderHookSelfCheckState::kPending:
      return "pending";
    case BinderHookSelfCheckState::kEffective:
      return "effective";
    case BinderHookSelfCheckState::kNoMatchingEntry:
      return "no_matching_entry";
    case BinderHookSelfCheckState::kSymbolMissing:
      return "symbol_missing";
    case BinderHookSelfCheckState::kParcelInvalid:
      return "parcel_invalid";
    case BinderHookSelfCheckState::kRewriteInvalid:
      return "rewrite_invalid";
  }
  return "unknown";
}

void logSelfCheck(int priority, const char* format, ...) {
  char message[256] = {};
  va_list args;
  va_start(args, format);
  (void)std::vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (__android_log_write != nullptr) {
    (void)__android_log_write(priority, kLogTag, message);
    return;
  }
  char fallback[300] = {};
  const int length = std::snprintf(fallback, sizeof(fallback), "[%s] %s\n", kLogTag,
                                   message);
  if (length > 0)
    (void)write(STDERR_FILENO, fallback,
                static_cast<size_t>(std::min(length,
                                             static_cast<int>(sizeof(fallback) - 1))));
}

void forceSelfCheckState(BinderHookSelfCheckState state, const char* reason) {
  g_self_check_state.store(state, std::memory_order_release);
  logSelfCheck(state == BinderHookSelfCheckState::kEffective ? 4 : 6,
               "self-check=%s: %s", selfCheckStateName(state),
               reason == nullptr ? "unspecified" : reason);
}

void finishSelfCheck(BinderHookSelfCheckState state, const char* reason) {
  BinderHookSelfCheckState expected = BinderHookSelfCheckState::kPending;
  if (!g_self_check_state.compare_exchange_strong(expected, state,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire))
    return;
  logSelfCheck(state == BinderHookSelfCheckState::kEffective ? 4 : 6,
               "self-check=%s: %s", selfCheckStateName(state),
               reason == nullptr ? "unspecified" : reason);
}

struct ElfIdentity {
  dev_t device;
  ino_t inode;
  std::string path;
};

std::vector<ElfIdentity> loadedElfs() {
  std::ifstream maps("/proc/self/maps");
  std::vector<ElfIdentity> result;
  std::set<std::pair<dev_t, ino_t>> seen;
  std::string line;
  while (std::getline(maps, line)) {
    const size_t slash = line.find('/');
    if (slash == std::string::npos) continue;
    std::string path = line.substr(slash);
    const size_t deleted = path.find(" (deleted)");
    if (deleted != std::string::npos) path.resize(deleted);
    struct stat info {};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
    if (seen.emplace(info.st_dev, info.st_ino).second)
      result.push_back({info.st_dev, info.st_ino, std::move(path)});
  }
  return result;
}

void* resolve(const char* symbol) {
  void* address = dlsym(RTLD_DEFAULT, symbol);
  return address != nullptr ? address : dlsym(RTLD_NEXT, symbol);
}

bool android14Runtime() {
#if defined(__ANDROID__)
  auto property_get = reinterpret_cast<SystemPropertyGetFn>(
      dlsym(RTLD_DEFAULT, "__system_property_get"));
  if (property_get == nullptr) return false;
  char sdk[16] = {};
  if (property_get("ro.build.version.sdk", sdk) <= 0) return false;
  return std::strcmp(sdk, "34") == 0;
#else
  return false;
#endif
}

uint32_t get32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

void put32(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
  data[2] = static_cast<uint8_t>(value >> 16);
  data[3] = static_cast<uint8_t>(value >> 24);
}

bool requestHasServiceManagerDescriptor(const void* request, size_t size,
                                        size_t* descriptor_end = nullptr) {
  if (request == nullptr || size < 4) return false;
  const auto* bytes = static_cast<const uint8_t*>(request);
  constexpr size_t descriptor_units = sizeof(kServiceManagerDescriptor) - 1;
  constexpr size_t descriptor_bytes = descriptor_units * 2;
  constexpr size_t descriptor_payload = 4 + descriptor_bytes + 2;
  constexpr size_t descriptor_padded = (descriptor_payload + 3) & ~size_t(3);
  // Parcel::writeInterfaceToken writes the StrictMode policy before the
  // String16 descriptor. Keep the offset exact so arbitrary request data
  // cannot be mistaken for an IServiceManager call.
  constexpr size_t descriptor_offset = 12;
  // B_PACK_CHARS is an integer assembled in big-endian character order;
  // Parcel writes that value as little-endian bytes on Android targets.
  constexpr uint32_t kSystemVendorHeader = 0x53595354u;  // B_PACK_CHARS('S','Y','S','T')
  if (size < descriptor_offset + descriptor_padded ||
      get32(bytes + 8) != kSystemVendorHeader ||
      get32(bytes + descriptor_offset) != descriptor_units)
    return false;
  const uint8_t* text = bytes + descriptor_offset + 4;
  for (size_t i = 0; i < descriptor_units; ++i)
    if (text[i * 2] != static_cast<uint8_t>(kServiceManagerDescriptor[i]) ||
        text[i * 2 + 1] != 0)
      return false;
  if (text[descriptor_bytes] != 0 || text[descriptor_bytes + 1] != 0)
    return false;
  for (size_t i = descriptor_payload; i < descriptor_padded; ++i)
    if (bytes[descriptor_offset + i] != 0)
      return false;
  if (descriptor_end != nullptr) *descriptor_end = descriptor_offset + descriptor_padded;
  return true;
}

enum class ServiceManagerMethod {
  kUnsupported,
  kGetService,
  kCheckService,
  kListServices,
  kIsDeclared,
  kGetServiceDebugInfo,
};

ServiceManagerMethod serviceManagerMethod(uint32_t code) {
  // Source: AOSP android14-release (Android 14 / SDK 34),
  // https://android.googlesource.com/platform/frameworks/native/+/android14-release/
  // libs/binder/aidl/android/os/IServiceManager.aidl.
  // The generated BnServiceManager::onTransact uses FIRST_CALL_TRANSACTION
  // plus the zero-based method declaration index. In that exact source,
  // getService=1, checkService=2, listServices=4, isDeclared=7, and
  // getServiceDebugInfo=14. Do not infer other values.
  constexpr uint32_t kGetService = 1u;
  constexpr uint32_t kCheckService = 2u;
  constexpr uint32_t kListServices = 4u;
  constexpr uint32_t kIsDeclared = 7u;
  constexpr uint32_t kGetServiceDebugInfo = 14u;
  switch (code) {
    case kGetService:
      return ServiceManagerMethod::kGetService;
    case kCheckService:
      return ServiceManagerMethod::kCheckService;
    case kListServices:
      return ServiceManagerMethod::kListServices;
    case kIsDeclared:
      return ServiceManagerMethod::kIsDeclared;
    case kGetServiceDebugInfo:
      return ServiceManagerMethod::kGetServiceDebugInfo;
    default:
      return ServiceManagerMethod::kUnsupported;
  }
}

bool restoreReply(void* reply, const uint8_t* original, size_t original_size) {
  if (reply == nullptr || original == nullptr || g_parcel_set_data == nullptr ||
      g_parcel_data == nullptr || g_parcel_size == nullptr)
    return false;
  if (g_parcel_set_data(reply, original, original_size) != 0)
    return false;
  const void* restored = g_parcel_data(reply);
  return restored != nullptr && g_parcel_size(reply) == original_size &&
         std::memcmp(restored, original, original_size) == 0;
}

bool publishReply(void* reply, const uint8_t* replacement, size_t replacement_size,
                  const uint8_t* original, size_t original_size,
                  size_t expected_objects) {
  if (reply == nullptr || replacement == nullptr || original == nullptr ||
      g_parcel_set_data == nullptr || g_parcel_data == nullptr ||
      g_parcel_size == nullptr || g_parcel_objects == nullptr)
    return false;
  if (g_parcel_set_data(reply, replacement, replacement_size) != 0)
    return false;
  const void* published = g_parcel_data(reply);
  const bool valid = published != nullptr && g_parcel_size(reply) == replacement_size &&
                     g_parcel_objects(reply) == expected_objects &&
                     std::memcmp(published, replacement, replacement_size) == 0;
  if (valid) return true;
  // setData() normally clears and rebuilds mObjects. If a future libbinder
  // changes that contract, restore the original bytes and fail open rather
  // than leave a reply whose object table points at removed data.
  (void)restoreReply(reply, original, original_size);
  return false;
}

enum class VectorRewriteOutcome {
  kApplied,
  kNoMatchingEntry,
  kParcelInvalid,
  kRewriteInvalid,
};

VectorRewriteOutcome rewriteVectorReply(void* reply, bool debug_info) {
  if (reply == nullptr || g_hidden_token_ptrs.empty() || g_parcel_data == nullptr ||
      g_parcel_size == nullptr || g_parcel_set_data == nullptr)
    return VectorRewriteOutcome::kParcelInvalid;
  const void* input = g_parcel_data(reply);
  const size_t input_size = g_parcel_size(reply);
  if (input == nullptr || input_size == 0 || input_size > YUKARI_PARCEL_MAX_BYTES)
    return VectorRewriteOutcome::kParcelInvalid;
  if (g_parcel_objects == nullptr || g_parcel_objects(reply) != 0)
    return VectorRewriteOutcome::kParcelInvalid;

  auto* original = static_cast<uint8_t*>(std::malloc(input_size));
  if (original == nullptr) return VectorRewriteOutcome::kRewriteInvalid;
  std::memcpy(original, input, input_size);

  auto* filtered = static_cast<uint8_t*>(std::malloc(input_size));
  if (filtered == nullptr) {
    std::free(original);
    return VectorRewriteOutcome::kRewriteInvalid;
  }
  yukari_parcel_result result{};
  const enum yukari_parcel_status status =
      debug_info ? yukari_parcel_filter_debug_info_mode(
                       input, input_size, filtered, input_size,
                       g_hidden_token_ptrs.data(), g_hidden_token_ptrs.size(), g_match_mode,
                       &result)
                 : yukari_parcel_filter_strings_mode(
                       input, input_size, filtered, input_size,
                       g_hidden_token_ptrs.data(), g_hidden_token_ptrs.size(), g_match_mode,
                       &result);
  if (status != YUKARI_PARCEL_OK || result.removed == 0 || result.size >= input_size) {
    std::free(original);
    std::free(filtered);
    if (status != YUKARI_PARCEL_OK)
      return VectorRewriteOutcome::kParcelInvalid;
    if (result.removed == 0)
      return VectorRewriteOutcome::kNoMatchingEntry;
    return VectorRewriteOutcome::kRewriteInvalid;
  }
  // Reparse the compacted vector before exposing it through Parcel. This is
  // the same parser used by the filter and proves the count/end offset pair
  // is self-consistent after removal.
  yukari_parcel_result replay{};
  const enum yukari_parcel_status replay_status =
      debug_info ? yukari_parcel_validate_debug_info(filtered, result.size, &replay)
                 : yukari_parcel_validate_strings(filtered, result.size, &replay);
  const bool replay_valid = replay_status == YUKARI_PARCEL_OK &&
                            replay.size == result.size && replay.kept == result.kept &&
                            replay.removed == 0;
  bool rewritten = replay_valid &&
                   publishReply(reply, filtered, result.size, original, input_size, 0);
  if (rewritten) {
    // Validate the bytes held by the actual Parcel after setData(), not only
    // the temporary compacted buffer. This catches ABI-specific publication
    // or object-table behavior before the caller can observe the reply.
    const void* published = g_parcel_data(reply);
    const size_t published_size = g_parcel_size(reply);
    yukari_parcel_result published_replay{};
    const enum yukari_parcel_status published_status =
        (published != nullptr && g_parcel_objects(reply) == 0)
            ? (debug_info ? yukari_parcel_validate_debug_info(
                               published, published_size, &published_replay)
                          : yukari_parcel_validate_strings(
                               published, published_size, &published_replay))
            : YUKARI_PARCEL_INVALID;
    const bool published_valid =
        published_status == YUKARI_PARCEL_OK && published_size == result.size &&
        published_replay.size == result.size && published_replay.kept == result.kept &&
        published_replay.removed == 0;
    if (!published_valid) {
      (void)restoreReply(reply, original, input_size);
      rewritten = false;
    }
  }
  std::free(original);
  std::free(filtered);
  if (!replay_valid) return VectorRewriteOutcome::kRewriteInvalid;
  return rewritten ? VectorRewriteOutcome::kApplied : VectorRewriteOutcome::kRewriteInvalid;
}

bool validateVectorReply(void* reply, bool debug_info) {
  if (reply == nullptr || g_parcel_data == nullptr || g_parcel_size == nullptr ||
      g_parcel_objects == nullptr)
    return false;
  const void* input = g_parcel_data(reply);
  const size_t input_size = g_parcel_size(reply);
  if (input == nullptr || input_size == 0 || input_size > YUKARI_PARCEL_MAX_BYTES ||
      g_parcel_objects(reply) != 0)
    return false;
  yukari_parcel_result parsed{};
  const enum yukari_parcel_status status =
      debug_info ? yukari_parcel_validate_debug_info(input, input_size, &parsed)
                 : yukari_parcel_validate_strings(input, input_size, &parsed);
  return status == YUKARI_PARCEL_OK && parsed.size == input_size && parsed.removed == 0;
}

void handleEnumeration(void* reply, bool debug_info) {
  bool expected = false;
  const bool self_check =
      !g_self_check_done.load(std::memory_order_acquire) &&
      g_self_check_done.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire);
  if (!self_check) {
    const VectorRewriteOutcome outcome = rewriteVectorReply(reply, debug_info);
    if (outcome == VectorRewriteOutcome::kApplied &&
        g_self_check_state.load(std::memory_order_acquire) ==
            BinderHookSelfCheckState::kNoMatchingEntry) {
      forceSelfCheckState(BinderHookSelfCheckState::kEffective,
                          "later enumeration was filtered and replay-validated");
    } else if (outcome == VectorRewriteOutcome::kParcelInvalid) {
      forceSelfCheckState(BinderHookSelfCheckState::kParcelInvalid,
                          "later enumeration failed Parcel validation; original reply kept");
    } else if (outcome == VectorRewriteOutcome::kRewriteInvalid) {
      forceSelfCheckState(
          BinderHookSelfCheckState::kRewriteInvalid,
          "later enumeration failed replay/publication validation; original reply kept");
    }
    return;
  }

  // This is deliberately the first real reply delivered through the hook.
  // Constructing an opaque Parcel just to probe the hook would require a
  // private libbinder layout and could itself crash the target process.
  if (!validateVectorReply(reply, debug_info)) {
    finishSelfCheck(BinderHookSelfCheckState::kParcelInvalid,
                    "enumeration reply failed read-only Parcel validation");
    return;
  }
  const VectorRewriteOutcome outcome = rewriteVectorReply(reply, debug_info);
  switch (outcome) {
    case VectorRewriteOutcome::kApplied:
      finishSelfCheck(BinderHookSelfCheckState::kEffective,
                      "first enumeration was filtered and replay-validated");
      break;
    case VectorRewriteOutcome::kNoMatchingEntry:
      finishSelfCheck(BinderHookSelfCheckState::kNoMatchingEntry,
                      "first enumeration contained no configured pattern");
      break;
    case VectorRewriteOutcome::kParcelInvalid:
      finishSelfCheck(BinderHookSelfCheckState::kParcelInvalid,
                      "enumeration rewrite rejected the Parcel");
      break;
    case VectorRewriteOutcome::kRewriteInvalid:
      finishSelfCheck(BinderHookSelfCheckState::kRewriteInvalid,
                      "filtered Parcel failed replay or publication checks");
      break;
  }
}

void markEnumerationParcelInvalid() {
  bool expected = false;
  if (g_self_check_done.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
    finishSelfCheck(BinderHookSelfCheckState::kParcelInvalid,
                    "enumeration request failed interface/Parcel validation");
  } else {
    logSelfCheck(6, "enumeration request=fail-open: interface/Parcel validation failed");
  }
}

enum class QueryMatchOutcome { kNoMatch, kMatch, kInvalid };

QueryMatchOutcome requestMatchesHiddenService(const void* request, size_t request_size) {
  if (g_hidden_token_ptrs.empty()) return QueryMatchOutcome::kNoMatch;
  if (request == nullptr || request_size == 0)
    return QueryMatchOutcome::kInvalid;
  size_t name_offset = 0;
  if (!requestHasServiceManagerDescriptor(request, request_size, &name_offset))
    return QueryMatchOutcome::kInvalid;
  int matched = 0;
  size_t name_end = 0;
  const enum yukari_parcel_status status = yukari_parcel_match_string16(
      request, request_size, name_offset, g_hidden_token_ptrs.data(),
      g_hidden_token_ptrs.size(), g_match_mode, &matched, &name_end);
  // Query methods have exactly one String16 argument. Reject trailing data so
  // a malformed or different request can never be mistaken for a match.
  if (status != YUKARI_PARCEL_OK || name_end != request_size)
    return QueryMatchOutcome::kInvalid;
  return matched != 0 ? QueryMatchOutcome::kMatch : QueryMatchOutcome::kNoMatch;
}

enum class QueryReplyKind { kNullBinder, kBoolean };

bool rewriteQueryReply(void* reply, QueryReplyKind kind) {
  if (reply == nullptr || g_parcel_data == nullptr || g_parcel_size == nullptr ||
      g_parcel_objects == nullptr || g_parcel_set_data == nullptr)
    return false;
  const void* input = g_parcel_data(reply);
  const size_t input_size = g_parcel_size(reply);
  if (input == nullptr || input_size == 0 || input_size > YUKARI_PARCEL_MAX_BYTES)
    return false;
  const size_t object_count = g_parcel_objects(reply);
  const auto* input_bytes = static_cast<const uint8_t*>(input);
  if (input_size < 4u || get32(input_bytes) != 0u)
    return false;  // Never replace an exception/non-successful reply.
  // A hidden declaration that is already reported absent is semantically
  // filtered; avoid a needless setData() call that could disturb Parcel
  // bookkeeping.
  if (kind == QueryReplyKind::kBoolean && object_count == 0u && input_size == 8u &&
      get32(input_bytes + 4u) == 0u)
    return true;

  auto* original = static_cast<uint8_t*>(std::malloc(input_size));
  if (original == nullptr) return false;
  std::memcpy(original, input, input_size);

  bool rewritten = false;
  if (kind == QueryReplyKind::kNullBinder) {
    // A getService/checkService reply contains at most one flat object. An
    // unexpected object table is not a format we can safely replace.
    constexpr uint32_t kBinderTypeBinder = 0x73622a85u;
    constexpr uint32_t kBinderTypeHandle = 0x73682a85u;
    const size_t expected_size = 16u + 2u * sizeof(void*);
    if (object_count > 1u || input_size != expected_size ||
        (get32(input_bytes + 4u) != kBinderTypeBinder &&
         get32(input_bytes + 4u) != kBinderTypeHandle)) {
      std::free(original);
      return false;
    }
    // Binder pointer width follows the process ABI for libbinder's Parcel.
    // The helper emits the Android 14 successful status + null flat object.
    uint8_t replacement[32] = {};
    size_t replacement_size = 0;
    const enum yukari_parcel_status make_status = yukari_parcel_make_null_binder(
        replacement, sizeof(replacement), static_cast<unsigned int>(sizeof(void*)),
        &replacement_size);
    if (make_status == YUKARI_PARCEL_OK)
      rewritten = publishReply(reply, replacement, replacement_size, original, input_size, 0);
  } else {
    // isDeclared() returns binder::Status (int32 zero) followed by an AIDL
    // boolean (int32). It never carries Binder objects; reject an unexpected
    // object table and malformed status/value rather than guessing a layout.
    if (object_count == 0 && input_size == 8u &&
        (get32(input_bytes + 4u) == 0u || get32(input_bytes + 4u) == 1u)) {
      auto* replacement = static_cast<uint8_t*>(std::malloc(input_size));
      if (replacement != nullptr) {
        std::memcpy(replacement, original, input_size);
        put32(replacement + 4u, 0u);
        rewritten = publishReply(reply, replacement, input_size, original, input_size, 0);
        std::free(replacement);
      }
    }
  }
  std::free(original);
  return rewritten;
}

int32_t hookedTransact(void* self, int32_t handle, uint32_t code, const void* data,
                       void* reply, uint32_t flags) {
  // A process may have no matching relocation in the first ELF scanned. The
  // PLT API can therefore leave the original pointer unset even though a
  // later registration will be committed. Resolve the exported symbol as a
  // fail-open fallback before giving up.
  TransactFn original = g_original_transact;
  if (original == nullptr)
    original = reinterpret_cast<TransactFn>(resolve(kTransactSymbol));
  if (original == nullptr) {
    forceSelfCheckState(BinderHookSelfCheckState::kSymbolMissing,
                        "IPCThreadState::transact original is unavailable");
    return -38;  // -ENOSYS, without requiring errno headers in the hot path.
  }
  if (g_in_hook)
    return original(self, handle, code, data, reply, flags);
  g_in_hook = true;
  struct HookScope final {
    ~HookScope() { g_in_hook = false; }
  } scope;
  const int32_t status = original(self, handle, code, data, reply, flags);
  if (status != 0 || handle != 0 || reply == nullptr ||
      data == nullptr)
    return status;
  const ServiceManagerMethod method = serviceManagerMethod(code);
  if (method == ServiceManagerMethod::kUnsupported) return status;
  if (g_parcel_data == nullptr || g_parcel_size == nullptr) {
    if (method == ServiceManagerMethod::kListServices ||
        method == ServiceManagerMethod::kGetServiceDebugInfo)
      finishSelfCheck(BinderHookSelfCheckState::kParcelInvalid,
                      "Parcel data/size symbols are unavailable");
    return status;
  }
  const void* request_bytes = g_parcel_data(data);
  const size_t request_size = g_parcel_size(data);
  const bool request_valid =
      request_bytes != nullptr && requestHasServiceManagerDescriptor(request_bytes, request_size);
  if (!request_valid) {
    if (method == ServiceManagerMethod::kListServices ||
        method == ServiceManagerMethod::kGetServiceDebugInfo)
      markEnumerationParcelInvalid();
    else
      logSelfCheck(6, "query request failed interface validation; original reply kept");
    return status;
  }
  switch (method) {
    case ServiceManagerMethod::kListServices:
      handleEnumeration(reply, false);
      break;
    case ServiceManagerMethod::kGetServiceDebugInfo:
      handleEnumeration(reply, true);
      break;
    case ServiceManagerMethod::kGetService:
    case ServiceManagerMethod::kCheckService:
      {
        const QueryMatchOutcome match =
            requestMatchesHiddenService(request_bytes, request_size);
        if (match == QueryMatchOutcome::kInvalid) {
          logSelfCheck(6, "query request failed String16 validation; original reply kept");
        } else if (match == QueryMatchOutcome::kMatch &&
                   !rewriteQueryReply(reply, QueryReplyKind::kNullBinder)) {
          forceSelfCheckState(BinderHookSelfCheckState::kRewriteInvalid,
                              "getService/checkService rewrite failed; original reply kept");
        }
      }
      break;
    case ServiceManagerMethod::kIsDeclared:
      {
        const QueryMatchOutcome match =
            requestMatchesHiddenService(request_bytes, request_size);
        if (match == QueryMatchOutcome::kInvalid) {
          logSelfCheck(6, "declaration query failed String16 validation; original reply kept");
        } else if (match == QueryMatchOutcome::kMatch &&
                   !rewriteQueryReply(reply, QueryReplyKind::kBoolean)) {
          forceSelfCheckState(BinderHookSelfCheckState::kRewriteInvalid,
                              "isDeclared rewrite failed; original reply kept");
        }
      }
      break;
    case ServiceManagerMethod::kUnsupported:
      break;
  }
  return status;
}

}  // namespace

void installBinderHooks(::zygisk::Api* api,
                        const std::vector<std::string>& hidden_tokens,
                        unsigned int match_mode) {
  g_self_check_done.store(false, std::memory_order_release);
  g_self_check_state.store(BinderHookSelfCheckState::kDisabled,
                           std::memory_order_release);
#if YUKARI_HAS_ZYGISK_API
  if (api == nullptr) {
    forceSelfCheckState(BinderHookSelfCheckState::kDisabled,
                        "Zygisk API is unavailable");
    return;
  }
  if (!android14Runtime()) {
    forceSelfCheckState(BinderHookSelfCheckState::kDisabled,
                        "runtime SDK is not Android 14 / SDK 34");
    return;
  }
  if (hidden_tokens.empty() || match_mode > YUKARI_PARCEL_MATCH_EXACT) {
    forceSelfCheckState(BinderHookSelfCheckState::kDisabled,
                        "filter configuration is empty or invalid");
    return;
  }
  g_parcel_data = reinterpret_cast<ParcelDataFn>(resolve(kParcelDataSymbol));
  g_parcel_size = reinterpret_cast<ParcelSizeFn>(resolve(kParcelSizeSymbol));
  g_parcel_objects = reinterpret_cast<ParcelObjectsFn>(resolve(kParcelObjectsSymbol));
  const char* set_data_symbol = sizeof(size_t) == 8u ? kParcelSetDataSymbol64
                                                      : kParcelSetDataSymbol32;
  g_parcel_set_data = reinterpret_cast<ParcelSetDataFn>(resolve(set_data_symbol));
  if (g_parcel_data == nullptr || g_parcel_size == nullptr || g_parcel_objects == nullptr ||
      g_parcel_set_data == nullptr) {
    forceSelfCheckState(BinderHookSelfCheckState::kSymbolMissing,
                        "one or more Parcel ABI symbols are unavailable");
    return;
  }
  if (resolve(kTransactSymbol) == nullptr) {
    forceSelfCheckState(BinderHookSelfCheckState::kSymbolMissing,
                        "IPCThreadState::transact symbol is unavailable");
    return;
  }

  // Keep a direct fallback before registering PLT entries.  The first ELF in
  // /proc/self/maps may not import transact, so its registration may not
  // populate the trampoline pointer even though a later ELF does.
  g_original_transact = reinterpret_cast<TransactFn>(resolve(kTransactSymbol));

  g_hidden_tokens = hidden_tokens;
  g_match_mode = match_mode;
  g_hidden_token_ptrs.clear();
  for (const std::string& token : g_hidden_tokens) {
    if (token.empty()) {
      forceSelfCheckState(BinderHookSelfCheckState::kDisabled,
                          "filter configuration contains an empty pattern");
      return;
    }
    g_hidden_token_ptrs.push_back(token.c_str());
  }
  const std::vector<ElfIdentity> elfs = loadedElfs();
  if (elfs.empty()) {
    forceSelfCheckState(BinderHookSelfCheckState::kSymbolMissing,
                        "no loaded ELF had a PLT table to register");
    return;
  }
  bool original_saved = false;
  for (const ElfIdentity& elf : elfs) {
    api->pltHookRegister(elf.device, elf.inode, kTransactSymbol,
                         reinterpret_cast<void*>(hookedTransact),
                         original_saved ? nullptr :
                             reinterpret_cast<void**>(&g_original_transact));
    original_saved = true;
  }
  // pltHookCommit() is owned by native_hooks.cpp.  Until the first actual
  // enumeration reaches this hook, installation alone is not evidence that
  // filtering is effective.
  forceSelfCheckState(BinderHookSelfCheckState::kPending,
                      "PLT registration queued; waiting for first enumeration");
#else
  (void)api;
  (void)hidden_tokens;
  (void)match_mode;
  forceSelfCheckState(BinderHookSelfCheckState::kDisabled,
                      "Zygisk headers are unavailable in this build");
#endif
}

BinderHookSelfCheckState binderHookSelfCheckState() {
  return g_self_check_state.load(std::memory_order_acquire);
}

void runBinderSelfCheck(void* jni_env) {
#if YUKARI_HAS_JNI
  if (g_self_check_state.load(std::memory_order_acquire) !=
      BinderHookSelfCheckState::kPending)
    return;
  if (jni_env == nullptr) {
    forceSelfCheckState(BinderHookSelfCheckState::kSymbolMissing,
                        "postAppSpecialize supplied no JNIEnv");
    return;
  }
  auto* env = static_cast<JNIEnv*>(jni_env);
  jclass service_manager = env->FindClass("android/os/ServiceManager");
  if (service_manager == nullptr) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    forceSelfCheckState(BinderHookSelfCheckState::kSymbolMissing,
                        "android.os.ServiceManager class lookup failed");
    return;
  }
  const jmethodID list_services =
      env->GetStaticMethodID(service_manager, "listServices", "()[Ljava/lang/String;");
  if (list_services == nullptr) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(service_manager);
    forceSelfCheckState(BinderHookSelfCheckState::kSymbolMissing,
                        "ServiceManager.listServices method lookup failed");
    return;
  }
  jobject result = env->CallStaticObjectMethod(service_manager, list_services);
  const bool threw = env->ExceptionCheck();
  if (threw) env->ExceptionClear();
  const bool result_missing = result == nullptr;
  if (result != nullptr) env->DeleteLocalRef(result);
  env->DeleteLocalRef(service_manager);
  if (threw) {
    forceSelfCheckState(BinderHookSelfCheckState::kParcelInvalid,
                        "ServiceManager.listServices raised during self-check");
    return;
  }
  if (result_missing &&
      g_self_check_state.load(std::memory_order_acquire) ==
          BinderHookSelfCheckState::kPending) {
    forceSelfCheckState(BinderHookSelfCheckState::kParcelInvalid,
                        "ServiceManager.listServices returned null during self-check");
    return;
  }
  if (g_self_check_state.load(std::memory_order_acquire) ==
      BinderHookSelfCheckState::kPending) {
    // The Java call completed without the hooked transaction observing an
    // enumeration. This is the diagnostic signature of a missing PLT edge or
    // an internal direct call; no data was changed.
    forceSelfCheckState(BinderHookSelfCheckState::kSymbolMissing,
                        "self-check call completed but transact hook was not observed");
  }
#else
  (void)jni_env;
  forceSelfCheckState(BinderHookSelfCheckState::kDisabled,
                      "JNI headers are unavailable in this build");
#endif
}

}  // namespace yukari
