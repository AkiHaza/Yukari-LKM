#include "native_hooks.h"
#include "binder_hooks.h"
#include "service_filter.h"

#if __has_include(<zygisk.hpp>)
#include <zygisk.hpp>
#define YUKARI_HAS_ZYGISK_API 1
#else
#define YUKARI_HAS_ZYGISK_API 0
#endif

#include <fstream>
#include <string>
#include <unistd.h>

namespace {

std::string processName() {
  std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
  std::string result;
  std::getline(cmdline, result, '\0');
  return result;
}

}  // namespace

#if YUKARI_HAS_ZYGISK_API

class YukariModule final : public zygisk::ModuleBase {
 public:
  void onLoad(zygisk::Api* api, JNIEnv* env) override {
    api_ = api;
    env_ = env;
  }

  void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
    uid_t uid = getuid();
    std::string process = processName();
    if (args != nullptr) {
      if (args->uid != nullptr) uid = static_cast<uid_t>(*args->uid);
      if (args->nice_name != nullptr && *args->nice_name != nullptr && env_ != nullptr) {
        const char* value = env_->GetStringUTFChars(*args->nice_name, nullptr);
        if (value != nullptr) {
          process = value;
          env_->ReleaseStringUTFChars(*args->nice_name, value);
        }
      }
    }
    const yukari::FilterConfig config = yukari::ServiceFilter::loadConfig();
    target_ = yukari::ServiceFilter(config).matchesCaller(uid, process);
    if (target_) {
      yukari::installNativeHooks(api_, process.c_str());
    } else if (api_ != nullptr) {
      api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }
  }

  void postAppSpecialize(const zygisk::AppSpecializeArgs*) override {
    if (target_) yukari::runBinderSelfCheck(static_cast<void*>(env_));
  }

 private:
  zygisk::Api* api_ = nullptr;
  JNIEnv* env_ = nullptr;
  bool target_ = false;
};

REGISTER_ZYGISK_MODULE(YukariModule);

#else

// The source remains buildable on a host without Magisk headers. Android
// module builds provide zygisk.hpp from the Magisk NDK and enable the entry
// point above; host tests exercise ServiceFilter independently.
extern "C" void zygisk_module_entry(void*, void*) {}

#endif

