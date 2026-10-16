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
