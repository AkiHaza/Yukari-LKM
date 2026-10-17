# P2.1 path coverage

The checked-in userspace hook targets Android 14 / SDK 34 and only runs after
the configured UID and process selector matches. It changes a reply in the
target process after a successful `IPCThreadState::transact`; every symbol,
ABI, Parcel, and object-table uncertainty fails open. The kernel module remains
an observation-only kretprobe, so it does not rewrite direct Binder ioctl
replies or replies received by processes that were not injected.

## Work item status

| Work item | Status | Boundary |
| --- | --- | --- |
| T1 query consistency (`getService`, `checkService`, `isDeclared`) | 已实现未构建验证 | Android 14 transaction codes 1, 2, and 7; hidden names use the shared UTF-16 matcher and query reply codec |
| T2 runtime self-check and diagnostics | 已实现未构建验证 | `postAppSpecialize` calls Java `ServiceManager.listServices()` through the normal Binder path; the hook records pass/fail state and never claims success from installation alone |
| T3 Parcel replay and object-table safety | 已实现未构建验证 | Vector replies require an empty object table and replay validation; query replacement uses `Parcel::setData()` after preflight, verifies the published bytes/object table, and attempts fail-open restoration if publication validation fails; Android 14 object-lifetime behavior remains device-pending |
| T4 libbinder inspection script | 已实现且已构建验证 | `tools/inspect_libbinder.sh` is shell-checked and exercised against `/bin/ls`; a target `libbinder.so` is still required for a device conclusion |
| T5 userspace/kernel configuration decoupling | 已实现未构建验证 | Userspace reads the root-owned JSON directly; `/dev/yukari_filter` is an optional best-effort sync channel |
| T6 userspace build path | 已实现未构建验证 | Android NDK + Magisk `zygisk.hpp` build commands are wired, but this checkout lacks that toolchain |
| W4 kernel Binder reply rewrite | 未实现 | `kernel/yukari_filter.c` remains observational as required by this phase |

## Coverage matrix

Each cell states the implementation boundary followed by the available
verification. “Device pending” means no connected target was available in this
workspace; it is not a claim that the path has passed on hardware.

| Operation | Java | Native | Shell | Raw `BINDER_WRITE_READ` |
| --- | --- | --- | --- | --- |
| Enumeration (`listServices`) | Conditional hook; source review of `ServiceManager` -> `IServiceManager` and Parcel replay; device Java call pending | Conditional hook; native `IServiceManager::listServices` source/ABI review; device native call pending | Implemented proxy; host Parcel/proxy tests and shell syntax checks; device `service list` pending | Not implemented; kernel source review only, direct ioctl test pending |
| Name query (`getService`, `checkService`) | Conditional hook for uncached calls; Android 14 code 1/2 and null-Binder reply review; device Java query pending | Conditional hook; same transaction/reply review with native `sp<IBinder>` semantics; device native query pending | No separate shell rewrite; `service check` is outside the list proxy and is covered only when its caller itself reaches the hook; device command pending | Not implemented; kernel source review only, direct ioctl test pending |
| Declaration query (`isDeclared`) | Conditional hook; Android 14 code 7 and boolean reply review; device Java call pending | Conditional hook; same AIDL boolean reply review; device native call pending | No text path; Binder-backed command coverage is conditional on injection; device command pending | Not implemented; kernel source review only, direct ioctl test pending |
| Debug information (`getServiceDebugInfo`) | Conditional hook; Android 14 code 14 and debug-info replay review; device Java call pending | Conditional hook; native debug-info reply review; device native call pending | No text path; source review confirms Binder path, device test pending | Not implemented; kernel source review only, direct ioctl test pending |

The Android 14 transaction-code source is the declaration order in
[`IServiceManager.aidl`](https://android.googlesource.com/platform/frameworks/native/+/android14-release/libs/binder/aidl/android/os/IServiceManager.aidl);
generated Binder stubs assign `FIRST_CALL_TRANSACTION` plus the zero-based
method index. Unknown codes remain untouched. The hook also requires the
ServiceManager interface token, a successful transport return, and a valid
Parcel before it can change a reply.

`ServiceManager.getService()` and `checkService()` can return a process-local
`sCache` entry without issuing Binder. Those cache hits cannot be observed by a
single `IPCThreadState::transact` hook; device validation must use an uncached
name or add a separate Java cache hook. The 32-bit and LP64 `Parcel::setData`
symbols are selected from the process ABI; an absent symbol still fails open.

## Configuration boundary

`ServiceFilter::loadConfig()` reads `/data/adb/modules/Yukari/config.json`
inside the injected process and validates its ownership and shape. It does not
depend on the kernel device. `service-proxy --sync` may send the same validated
snapshot to `/dev/yukari_filter`; an unavailable or failed ioctl leaves the
userspace hook active and the kernel side fail-open. The kernel never parses
JSON.

## Verification limits

The host C Parcel test passes with the repository TinyCC build. This Windows
workspace has no C++ compiler, CMake, Android NDK, Kleaf/Bazel workspace, or
online device, so Android userspace, kernel, and device behavior are **未验证**.
The minimum Android build is:

```sh
make -C userspace NDK_HOME="$ANDROID_NDK_HOME" \
  MAGISK_INCLUDE=/path/to/magisk/include
```

The complete kernel-plus-package workflow is `build/build.sh` with
`KERNEL_WORKSPACE`, `KERNEL_BUILD_LABEL`, `ANDROID_NDK_HOME`, and
`MAGISK_INCLUDE` set as documented in [`docs/build.md`](build.md).
