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

The repository workflow runs on a GitHub-hosted `ubuntu-24.04` runner and uses
the same `build/build.sh` entry point. It prepares an Android 14/GKI 6.1 Kleaf
workspace from the Android kernel manifest, installs the requested Android NDK,
downloads the pinned Magisk `zygisk.hpp`, and then builds the module.

The following repository Variables are optional overrides. The workflow has
defaults for all of them, so an empty `workflow_dispatch` run uses the public
Android 14/GKI 6.1 common manifest:

| Variable | Required | Value |
| --- | --- | --- |
| `YUKARI_KERNEL_MANIFEST_URL` | no | HTTPS manifest URL; defaults to `https://android.googlesource.com/kernel/manifest` |
| `YUKARI_KERNEL_MANIFEST_REF` | no | Manifest branch; defaults to `common-android14-6.1` |
| `YUKARI_KERNEL_BUILD_LABEL` | no | Exact label; defaults to `//common:kernel_aarch64` |
| `YUKARI_KERNEL_HEADERS_LABEL` | no | Headers label; defaults to `//common:all_headers` |
| `YUKARI_NDK_VERSION` | no | SDK NDK version; defaults to `26.3.11579264` |
| `YUKARI_MAGISK_ZYGISK_URL` | no | HTTPS URL for `zygisk.hpp`; defaults to Magisk `v28.1` |
| `YUKARI_MAGISK_ZYGISK_SHA256` | no | Optional 64-character SHA-256 for the downloaded header |
| `YUKARI_ABIS` | no | Comma-separated ABIs; defaults to `arm64-v8a` |

`workflow_dispatch` inputs are optional overrides and take precedence over
repository Variables. The workflow downloads the `repo` tool, syncs the
selected manifest, verifies the workspace marker and requested Bazel labels,
and checks `ndk-build` and `zygisk.hpp` before invoking the build. The default
labels target AOSP common GKI; a device-specific kernel requires matching
manifest/ref and labels supplied through inputs or Variables.

For example, repository Variables can be set with the GitHub CLI from a
trusted machine when building a device-specific kernel:

```sh
gh variable set YUKARI_KERNEL_MANIFEST_URL --body https://android.googlesource.com/kernel/manifest
gh variable set YUKARI_KERNEL_MANIFEST_REF --body common-android14-6.1
gh variable set YUKARI_KERNEL_BUILD_LABEL --body //common:kernel_aarch64
gh variable set YUKARI_NDK_VERSION --body 26.3.11579264
```

The local Windows checkout does not contain the cloud dependencies, so the
workflow itself must be run on GitHub Actions to validate the remote manifest,
NDK download, and full Kleaf build. Standard hosted runners have finite disk
space; a larger runner may be needed if the selected kernel manifest produces
large generated outputs.

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
