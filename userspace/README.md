# Yukari userspace ServiceManager view

This directory contains the Magisk/Zygisk companion for the Yukari-LKM
ServiceManager policy. It is deliberately process-local: it never writes the
servicemanager registry and it only installs hooks after the Zygisk UID/process
selector matches.

`zygisk/src/service_filter.cpp` is the single filtering implementation shared
by the injected library and `service-proxy`. Names are compared
case-insensitively against configurable tokens (the default token is
`lineage`). The same matcher is used for the command-line path, so a hidden
name cannot reappear through a second `service list` invocation.

The module redirects an exact `service list` or `/system/bin/service list`
invocation made with `execve`, `posix_spawn`, `system`, or `popen` to
`bin/service-proxy`. The proxy executes the real service command, filters
newline-delimited output, preserves its exit status, and rewrites the standard
`Found N services` header and numeric row indexes when that format is present. On a
Magisk build the redirectors use Zygisk's PLT hook API across the loaded ELF
objects. A non-Zygisk host build may use the optional Dobby backend; without a
hook backend the companion fails open.

## Configuration

The installed module reads `config/targets.conf`:

```text
uids=10234
processes=com.example.app,com.example.app:isolated
hide_tokens=lineage
```

`uids` is required for an installed configuration. When `processes` is also
non-empty, both selectors must match the current caller. The installed module
rejects missing, non-regular, non-root-owned, or group/world-writable
config files. Core/system UIDs with an appId below 10000 are excluded; the
stock LKM cannot distinguish a privileged system package that owns an
application-range UID, so provisioning must keep those UIDs out of the target
allowlist. `configFromEnvironment()` remains available only to host test
fixtures; the injected module never accepts environment overrides because the
target app controls its own environment.

The Zygisk companion also has a conditional Android 14 (SDK 34) PLT hook for
the exported `IPCThreadState::transact` symbol. It validates the
`IServiceManager` request header and applies the shared Parcel codec to list,
debug-info, name-query, and declaration-query replies. Hidden names therefore
produce the same not-found result through list and by-name APIs. PLT cannot
intercept a libbinder internal direct call that does not import the symbol.
The hook is fail-open on other SDK versions, missing symbols, malformed
Parcels, unsuccessful replies, and unsupported object tables. The command hook
still only covers exact `service list` and `/system/bin/service list` calls
through intercepted libc entries; Runtime.exec JNI internals, direct syscalls,
shell wrappers, and direct `BINDER_WRITE_READ` clients require a framework or
vendor Binder integration.

The injected process reads `/data/adb/modules/Yukari/config.json` directly.
`/dev/yukari_filter` is an optional best-effort control channel used by the
root-side sync helper; its absence never disables the independent userspace
policy.

After specialization, the module invokes `ServiceManager.listServices()` once
through JNI. The hook marks the process self-check effective only after the
real reply has passed Parcel replay and publication checks; installation by
itself is reported as pending.

## Building

The Android build expects an Android NDK and Magisk's `zygisk.hpp` on the
include path. With the NDK environment loaded:

```sh
make -C userspace NDK_HOME="$ANDROID_NDK_HOME" MAGISK_INCLUDE=/path/to/magisk/include
```

The result is a flashable `dist/yukari-service-filter.zip` containing
`module.prop`, the Zygisk library for each requested ABI, `bin/service-proxy`,
and the sample `config/targets.conf`. `make test` builds the host matcher test
when a C++ compiler is available.

