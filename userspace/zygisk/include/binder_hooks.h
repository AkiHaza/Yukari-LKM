#pragma once

#include <string>
#include <vector>

namespace zygisk {
class Api;
}

namespace yukari {

// Process-local status for the first real enumeration transaction after
// injection.  The flag is diagnostic only; every non-effective state keeps
// the original Binder reply unchanged (fail-open).
enum class BinderHookSelfCheckState {
  kDisabled = 0,
  kPending,
  kEffective,
  kNoMatchingEntry,
  kSymbolMissing,
  kParcelInvalid,
  kRewriteInvalid,
};

// Installs the optional libbinder transact hook for the already-matched
// target process. A missing libbinder symbol, unsupported SDK, or malformed
// Parcel leaves the original transaction untouched.
void installBinderHooks(::zygisk::Api* api,
                        const std::vector<std::string>& hidden_tokens,
                        unsigned int match_mode);

// After PLT registration, invoke the public Java enumeration once so the
// hook's own transact path can record a real self-check result. The argument
// is an opaque JNIEnv pointer to keep this header usable in host builds.
void runBinderSelfCheck(void* jni_env);

BinderHookSelfCheckState binderHookSelfCheckState();

}  // namespace yukari
