# Android 14 / GKI 6.1 build

The supported entry point is `build/build.sh`. It stages the kernel source
into the supplied Kleaf workspace, builds the DDK module for the selected
`kernel_build`, invokes `ndk-build` for every requested ABI, and repacks one
Magisk ZIP containing the user-space binaries, scripts, configuration, and
`yukari_filter.ko`.

The build host must provide:

* an Android 14 / GKI 6.1 Kleaf workspace with `MODULE.bazel`, `WORKSPACE`,
  or `WORKSPACE.bazel`;
* the exact `kernel_build` label for the device kernel;
* the matching kernel headers target, normally `//common:all_headers`;
* an Android NDK containing `ndk-build`; and
* Magisk headers containing `zygisk.hpp`.

Example:

```sh
KERNEL_WORKSPACE=/path/to/kernel-workspace \
KERNEL_BUILD_LABEL=//common:kernel_aarch64 \
KERNEL_HEADERS_LABEL=//common:all_headers \
ANDROID_NDK_HOME=/path/to/android-ndk \
MAGISK_INCLUDE=/path/to/magisk/include \
ABIS=arm64-v8a \
./build/build.sh
```

The script prefers `$KERNEL_WORKSPACE/tools/bazel`; set `BAZEL` when the DDK
requires another wrapper. `OUT_DIR` selects the repository output directory.
The label values are validated before being inserted into the staged BUILD
file. The generated ZIP is `OUT_DIR/yukari-service-filter.zip`.

The kernel module is intentionally fail-open on an unmodified GKI image. A
successful build verifies compilation and packaging only; it does not provide
Binder reply rewriting. KMI symbol availability, module signing, loading, and
the target-device service-view tests still require the matching Android image.
This repository's Windows checkout does not include a
Kleaf/NDK toolchain, so no real Android 14/GKI 6.1 build is claimed here.

## libbinder PLT inspection

Before enabling the userspace Binder hook on a device image, inspect that
image's actual `libbinder.so` with the host-side script:

```sh
tools/inspect_libbinder.sh /path/to/device/libbinder.so
```

The script prints stable `KEY=VALUE` fields for the ELF `SYMBOLIC` and
`BIND_NOW` flags, the `IPCThreadState::transact` PLT relocation, and the call
form found in `BpServiceManager`. It ends with `CONCLUSION=PLT_HOOK_POSSIBLE`
or `CONCLUSION=PLT_HOOK_LIKELY_INEFFECTIVE`; missing binutils, an unreadable
file, or an uninspectable section produces `CONCLUSION=unknown` and a
non-zero status. `CONCLUSION_TEXT` provides the corresponding human-readable
"PLT hook may work" or "PLT hook likely ineffective" explanation.
Set `READELF` and `OBJDUMP` when cross-inspecting with a toolchain-specific
binutils pair. The script never assumes a firmware or mount path.
