# Yukari-LKM

Yukari-LKM contains a per-caller service-view filter for a configured app.
The policy is intentionally narrow: it matches an explicit UID and optional
process allowlist, then hides service names containing configured literal
tokens such as `lineage`. It never edits servicemanager's registration table.

The repository contains two layers:

* `userspace/` contains the filtering core, a `service list` proxy, and the
  Zygisk integration point. The proxy keeps command output consistent with
  the core filter. An optional Android 14 libbinder `IPCThreadState::transact`
  PLT hook applies the same bounded Parcel codec to list, debug-info, name,
  and declaration replies when the caller imports the exported symbol and
  runtime checks pass.
* `kernel/` contains a GKI-loadable, fail-open Binder probe. A stock GKI LKM
  cannot safely rewrite a Binder reply after `binder_ioctl_write_read()` and
  preserve Parcel object offsets. Reply rewriting requires a vendor Binder
  hook at the point described in [docs/kernel-hook.md](docs/kernel-hook.md).

This boundary is deliberate. It prevents a module from corrupting Binder or
silently returning a different result on one Android release than another.
The userspace hook also applies the same hidden-name decision to
`getService`, `checkService`, and `isDeclared`, while direct
`BINDER_WRITE_READ` remains outside the injected process boundary and fails
open. See [docs/coverage.md](docs/coverage.md) for the complete matrix and
verification status.

## Configuration

The Zygisk process selector must be configured with a trusted user-app UID
and, when needed, exact process names. The installed module reads:

```text
uids=12345
processes=com.example.app,com.example.app:worker
hide_tokens=lineage
```

An empty or UID-less target selector disables filtering. Core/system UIDs with
an Android appId below 10000 are always excluded. A privileged system package
that owns an application-range UID cannot be identified from the stock LKM
interface alone, so provisioning must never place such a UID in the target
allowlist. Tokens are literal, case-insensitive substrings; regular
expressions are unsupported. Populate the selector during installation or
provisioning from the package's verified UID and process list. The file is
root-owned and readable by the injected app, but not writable by it. Do not
use an untrusted app-provided environment variable as authorization in a
production module.

An optional root-owned `/data/adb/modules/Yukari/config.json` provides the
extended schema:

```json
{
  "target_uids": [10234],
  "target_packages": ["com.example.target"],
  "filter_patterns": ["lineage"],
  "match_mode": "substring",
  "kernel_hook_enabled": false,
  "userspace_hook_enabled": true
}
```

Package names are resolved through `/data/system/packages.list`; resolved UIDs
are used directly by the userspace selector and may also be sent to the
root-only `/dev/yukari_filter` control device. The ioctl is best effort and
optional. The stock-GKI kernel module stores the validated snapshot for a
future vendor Binder hook and remains fail-open. If `config.json` is absent,
`targets.conf` keeps the legacy behavior.

## Build

`build/build.sh` is the entry point used by the workflow; the complete
workflow is documented in [docs/build.md](docs/build.md). It expects a
matching Android 14/GKI 6.1 Kleaf workspace in `KERNEL_WORKSPACE`, the exact
`kernel_build` label in `KERNEL_BUILD_LABEL`, an Android NDK in
`ANDROID_NDK_HOME`, and Magisk headers in `MAGISK_INCLUDE`. The matching
headers label defaults to `//common:all_headers` and can be overridden with
`KERNEL_HEADERS_LABEL`. `BAZEL` may point to the DDK's Bazel wrapper. The
script stages the module into that workspace, builds the `.ko`, builds the
proxy and Zygisk library, then assembles a Magisk module zip.

```sh
KERNEL_WORKSPACE=/path/to/ddk KERNEL_BUILD_LABEL=//common:kernel_aarch64 \
  ANDROID_NDK_HOME=/path/to/ndk MAGISK_INCLUDE=/path/to/magisk/include \
  ./build/build.sh
```

The workflow runs on a self-hosted runner labelled `android-ddk`. Configure
`YUKARI_KERNEL_WORKSPACE`, `YUKARI_KERNEL_BUILD_LABEL`,
`YUKARI_ANDROID_NDK_HOME`, and `YUKARI_MAGISK_INCLUDE` as repository Variables
or runner environment variables; optional headers, Bazel, and ABI variables
are documented in [docs/build.md](docs/build.md). Manual workflow inputs can
override those values. A preflight step validates all paths before invoking
the same script. A successful build proves compilation only; GKI KMI symbol
availability, module signing, and device loading still need validation on the
target image.

## Verification

The host tests exercise caller matching, literal service-name matching,
preservation of output formatting, and the command proxy's filtering contract.
They do not emulate a Binder driver. Device tests must cover target and
non-target callers, app and worker processes, system apps, malformed Parcels,
concurrent transactions, list/get/debug-info consistency, and a direct native
`BINDER_WRITE_READ` client before enabling any vendor rewrite hook.
