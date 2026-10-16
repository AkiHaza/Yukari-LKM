#include "native_hooks.h"

#include <dlfcn.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#if __has_include(<zygisk.hpp>)
#include <zygisk.hpp>
#define YUKARI_HAS_ZYGISK_API 1
#else
#define YUKARI_HAS_ZYGISK_API 0
#endif

// Dobby is an optional native hook backend supplied by the module build.
// Weak declarations let development builds load without that backend; the
// wrappers then remain inactive.
extern "C" int DobbyHook(void* address, void* replace, void** origin) __attribute__((weak));
extern "C" char** environ;

namespace yukari {
namespace {

using ExecveFn = int (*)(const char*, char* const[], char* const[]);
using PosixSpawnFn = int (*)(pid_t*, const char*, const posix_spawn_file_actions_t*,
                             const posix_spawnattr_t*, char* const[], char* const[]);
using SystemFn = int (*)(const char*);
using PopenFn = FILE* (*)(const char*, const char*);

ExecveFn g_execve;
PosixSpawnFn g_posix_spawn;
SystemFn g_system;
PopenFn g_popen;
std::string g_caller_process;
std::string g_proxy_path;

#if YUKARI_HAS_ZYGISK_API
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
  std::stable_sort(result.begin(), result.end(), [](const ElfIdentity& lhs,
                                                    const ElfIdentity& rhs) {
    const bool lhs_libc = lhs.path.find("libc.so") != std::string::npos;
    const bool rhs_libc = rhs.path.find("libc.so") != std::string::npos;
    return lhs_libc && !rhs_libc;
  });
  return result;
}
#endif

void* resolve(const char* symbol) { return dlsym(RTLD_NEXT, symbol); }

bool isServiceExecutable(const char* path) {
  return path != nullptr &&
         (std::strcmp(path, "/system/bin/service") == 0 ||
          std::strcmp(path, "service") == 0);
}

bool isList(char* const argv[]) {
  return argv != nullptr && argv[0] != nullptr && argv[1] != nullptr &&
         std::strcmp(argv[1], "list") == 0;
}

bool shouldProxy(const char* path, char* const argv[]) {
  return isServiceExecutable(path) && isList(argv);
}

std::vector<char*> proxyArgv(char* const argv[]) {
  std::vector<char*> result;
  result.reserve(4);
  result.push_back(const_cast<char*>(g_proxy_path.c_str()));
  result.push_back(const_cast<char*>("--exec"));
  result.push_back(const_cast<char*>("/system/bin/service"));
  for (size_t i = 1; argv != nullptr && argv[i] != nullptr; ++i) result.push_back(argv[i]);
  result.push_back(nullptr);
  return result;
}

struct ProxyEnvironment {
  std::vector<char*> entries;
  std::string process;
};

void proxyEnv(char* const envp[], ProxyEnvironment* result) {
  if (result == nullptr) return;
  char* const* source = envp == nullptr ? environ : envp;
  constexpr char kProcessPrefix[] = "YUKARI_CALLER_PROCESS=";
  constexpr char kUidPrefix[] = "YUKARI_CALLER_UID=";
  for (size_t i = 0; source != nullptr && source[i] != nullptr; ++i) {
    // Do not leave app-supplied identity keys ahead of the hook's value.
    if (std::strncmp(source[i], kProcessPrefix, sizeof(kProcessPrefix) - 1) == 0 ||
        std::strncmp(source[i], kUidPrefix, sizeof(kUidPrefix) - 1) == 0)
      continue;
    result->entries.push_back(source[i]);
  }
  result->process = std::string(kProcessPrefix) + g_caller_process;
  result->entries.push_back(const_cast<char*>(result->process.c_str()));
  result->entries.push_back(nullptr);
}

std::string shellQuote(std::string_view value) {
  std::string result;
  result.reserve(value.size() + 2);
  result.push_back('\'');
  for (const char c : value) {
    if (c == '\'') result.append("'\\''");
    else result.push_back(c);
  }
  result.push_back('\'');
  return result;
}

std::string proxyCommand() {
  return "YUKARI_CALLER_PROCESS=" + shellQuote(g_caller_process) + " " + g_proxy_path +
         " --exec /system/bin/service list";
}

extern "C" int hooked_execve(const char* path, char* const argv[], char* const envp[]) {
  if (g_execve == nullptr) g_execve = reinterpret_cast<ExecveFn>(resolve("execve"));
  if (g_execve == nullptr) {
    errno = ENOSYS;
    return -1;
  }
  if (!shouldProxy(path, argv) || g_proxy_path.empty()) return g_execve(path, argv, envp);
  const auto args = proxyArgv(argv);
  ProxyEnvironment env;
  proxyEnv(envp, &env);
  return g_execve(g_proxy_path.c_str(), args.data(), env.entries.data());
}

extern "C" int hooked_posix_spawn(pid_t* pid, const char* path,
                                    const posix_spawn_file_actions_t* file_actions,
                                    const posix_spawnattr_t* attributes, char* const argv[],
                                    char* const envp[]) {
  if (g_posix_spawn == nullptr)
    g_posix_spawn = reinterpret_cast<PosixSpawnFn>(resolve("posix_spawn"));
  if (g_posix_spawn == nullptr) return ENOSYS;
  if (!shouldProxy(path, argv) || g_proxy_path.empty())
    return g_posix_spawn(pid, path, file_actions, attributes, argv, envp);
  const auto args = proxyArgv(argv);
  ProxyEnvironment env;
  proxyEnv(envp, &env);
  return g_posix_spawn(pid, g_proxy_path.c_str(), file_actions, attributes, args.data(),
                       env.entries.data());
}

extern "C" int hooked_system(const char* command) {
  if (g_system == nullptr) g_system = reinterpret_cast<SystemFn>(resolve("system"));
  if (g_system == nullptr) {
    errno = ENOSYS;
    return -1;
  }
  if (command == nullptr || g_proxy_path.empty()) return g_system(command);
  // Deliberately require an exact command shape.  Arbitrary shell commands
  // are never rewritten, avoiding quoting changes and command injection.
  if (std::strcmp(command, "/system/bin/service list") != 0 &&
      std::strcmp(command, "service list") != 0)
    return g_system(command);
  const std::string replacement = proxyCommand();
  return g_system(replacement.c_str());
}

extern "C" FILE* hooked_popen(const char* command, const char* mode) {
  if (g_popen == nullptr) g_popen = reinterpret_cast<PopenFn>(resolve("popen"));
  if (g_popen == nullptr) {
    errno = ENOSYS;
    return nullptr;
  }
  if (command == nullptr || mode == nullptr || g_proxy_path.empty()) return g_popen(command, mode);
  if (std::strcmp(command, "/system/bin/service list") != 0 &&
      std::strcmp(command, "service list") != 0)
    return g_popen(command, mode);
  const std::string replacement = proxyCommand();
  return g_popen(replacement.c_str(), mode);
}

template <typename T>
void hookSymbol(const char* name, T replacement, T* original) {
  if (DobbyHook == nullptr || original == nullptr) return;
  void* address = resolve(name);
  if (address == nullptr) address = dlsym(RTLD_DEFAULT, name);
  if (address == nullptr) return;
  void* origin = nullptr;
  if (DobbyHook(address, reinterpret_cast<void*>(replacement), &origin) == 0)
    *original = reinterpret_cast<T>(origin);
}

}  // namespace

void installNativeHooks(::zygisk::Api* api, const char* caller_process) {
  g_caller_process = caller_process == nullptr ? std::string() : caller_process;
  g_proxy_path = "/data/adb/modules/yukari-service-filter/bin/service-proxy";
#if YUKARI_HAS_ZYGISK_API
  if (api != nullptr) {
    bool originals_saved = false;
    for (const ElfIdentity& elf : loadedElfs()) {
      api->pltHookRegister(elf.device, elf.inode, "execve",
                           reinterpret_cast<void*>(hooked_execve),
                           originals_saved ? nullptr : reinterpret_cast<void**>(&g_execve));
      api->pltHookRegister(elf.device, elf.inode, "posix_spawn",
                           reinterpret_cast<void*>(hooked_posix_spawn),
                           originals_saved ? nullptr : reinterpret_cast<void**>(&g_posix_spawn));
      api->pltHookRegister(elf.device, elf.inode, "system",
                           reinterpret_cast<void*>(hooked_system),
                           originals_saved ? nullptr : reinterpret_cast<void**>(&g_system));
      api->pltHookRegister(elf.device, elf.inode, "popen",
                           reinterpret_cast<void*>(hooked_popen),
                           originals_saved ? nullptr : reinterpret_cast<void**>(&g_popen));
      originals_saved = true;
    }
    (void)api->pltHookCommit();
    return;
  }
#endif
  // Dobby is optional for non-Zygisk/vendor builds. Without a hook backend
  // these process-local wrappers remain inactive.
  if (DobbyHook == nullptr) return;
  hookSymbol("execve", hooked_execve, &g_execve);
  hookSymbol("posix_spawn", hooked_posix_spawn, &g_posix_spawn);
  hookSymbol("system", hooked_system, &g_system);
  hookSymbol("popen", hooked_popen, &g_popen);
}

}  // namespace yukari

