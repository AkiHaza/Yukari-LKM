#pragma once

namespace zygisk {
class Api;
}

namespace yukari {

// Installs process-local wrappers for execve/posix_spawn/system/popen.  The
// wrappers only redirect an exact `service list` invocation to service-proxy;
// all other commands go to their original libc implementation.
void installNativeHooks(::zygisk::Api* api, const char* caller_process);

}  // namespace yukari

