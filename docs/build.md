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

## GitHub Actions

The repository workflow runs on a self-hosted runner with the labels
`self-hosted`, `linux`, and `android-ddk`. It uses the same `build/build.sh`
entry point. Configure these repository Variables once, or export the matching
`YUKARI_*` variables in the runner service environment:

| Variable | Required | Value |
| --- | --- | --- |
| `YUKARI_KERNEL_WORKSPACE` | yes | Absolute path to the Android 14/GKI 6.1 Kleaf workspace |
| `YUKARI_KERNEL_BUILD_LABEL` | yes | Exact device `kernel_build` Bazel label |
| `YUKARI_KERNEL_HEADERS_LABEL` | no | Headers label; defaults to `//common:all_headers` |
| `YUKARI_ANDROID_NDK_HOME` | yes | Absolute Android NDK directory containing `ndk-build` |
| `YUKARI_MAGISK_INCLUDE` | yes | Absolute include directory containing `zygisk.hpp` |
| `YUKARI_BAZEL` | no | Bazel wrapper; empty selects workspace `tools/bazel`, then `PATH` |
| `YUKARI_ABIS` | no | Comma-separated ABIs; defaults to `arm64-v8a` |

`workflow_dispatch` inputs are optional overrides and take precedence over
repository Variables, which take precedence over runner environment variables.
The workflow performs a preflight check for every required path, the Kleaf
workspace marker, Bazel, the NDK compiler, and `zygisk.hpp` before invoking the
build. No firmware-specific paths or kernel labels are invented by the
workflow; the exact values must match the device image.

For example, repository Variables can be set with the GitHub CLI from a
trusted machine (replace the values with the runner's real paths):

```sh
gh variable set YUKARI_KERNEL_WORKSPACE --body /opt/android/gki-6.1
gh variable set YUKARI_KERNEL_BUILD_LABEL --body //common:kernel_aarch64
gh variable set YUKARI_ANDROID_NDK_HOME --body /opt/android-ndk
gh variable set YUKARI_MAGISK_INCLUDE --body /opt/magisk/include
```

The local checkout in this repository does not contain a Linux Kleaf/NDK
toolchain, so a successful GitHub Actions run still requires a registered
`android-ddk` runner with those dependencies installed.

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
